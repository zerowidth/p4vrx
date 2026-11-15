"""Check the C5 -> Nano wires: the C5 runs firmware/c5-pinwalk, the Nano runs pintest_top, and the P4 relays
the C5's step log over its CH343 console. Prints one line per wire: ok, open, swapped or shorted."""
import re
import sys
import threading
import time

import serial
from serial.tools.list_ports import comports

# Nano c5 bits 14..0, with C5 GPIO and Nano pin.
WIRES = [("CLK", 0, 77), ("Q7", 1, 73), ("Q6", 2, 74), ("Q5", 3, 75), ("Q4", 4, 85), ("Q3", 23, 27),
         ("Q2", 24, 28), ("Q1", 25, 25), ("I7", 6, 76), ("I6", 7, 80), ("I5", 8, 42), ("I4", 9, 41),
         ("I3", 10, 48), ("I2", 13, 49), ("I1", 14, 86)]


def port(vid, pid, suffix=""):
    ports = sorted(p.device for p in comports() if (p.vid, p.pid) == (vid, pid) and p.device.endswith(suffix))
    if len(ports) != 1:
        sys.exit(f"expected one {vid:04x}:{pid:04x} port, found {ports}")
    return ports[0]


def main(seconds=12):
    p4 = serial.Serial()
    p4.port, p4.baudrate, p4.timeout = port(0x1A86, 0x55D3), 115200, 0.05
    p4.dtr = p4.rts = False
    p4.open()
    time.sleep(3)
    p4.write(b"\rc5 log on\r")
    nano = serial.Serial(port(0x0403, 0x6010, "1"), 115200, timeout=0.2)

    samples, steps, stop = [], [], threading.Event()

    def read_log():
        buf = b""
        while not stop.is_set():
            buf += p4.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = re.search(rb"pinwalk (\d+) (\w+) (high|low)(.*)", line)
                if m:
                    followers = re.findall(rb"follower=(\w+)", m.group(4))
                    steps.append((time.time(), int(m.group(1)), m.group(3) == b"high",
                                  [f.decode() for f in followers]))

    t = threading.Thread(target=read_log, daemon=True)
    t.start()
    end = time.time() + seconds
    while time.time() < end:
        nano.reset_input_buffer()
        nano.write(b"r")
        line = nano.readline().decode(errors="replace").split()
        if len(line) == 3 and all(t.count("=") == 1 for t in line):
            samples.append((time.time(), int(dict(t.split("=") for t in line)["c5"], 16)))
    stop.set()
    t.join()
    p4.write(b"\rc5 log off\r")

    phases = {}
    for ts, k, high, followers in steps:
        window = [v for t2, v in samples if ts + 0.05 < t2 < ts + 0.2]
        if window:
            phases.setdefault(k, {})[high] = (max(set(window), key=window.count), followers)
    # A wire changes only its own Nano bit between its high and low phases.
    seen = {k: (p[True][0] ^ p[False][0], p[False][1]) for k, p in phases.items() if len(p) == 2}
    print(f"{'wire':4} {'C5':>3} {'Nano':>4}  {'C5 -> Nano':28} C5 own pins")
    for k, (name, gpio, pin) in enumerate(WIRES):
        if k not in seen:
            print(f"{name:4} {gpio:3} {pin:4}  no sample")
            continue
        v, followers = seen[k]
        bits = [WIRES[i][0] for i in range(15) if (v >> (14 - i)) & 1]
        others = [b for b in bits if b != name]
        if name in bits and not others:
            verdict = "ok"
        elif name in bits:
            verdict = "shorted to " + ", ".join(others)
        elif others:
            verdict = "swapped with " + ", ".join(others)
        else:
            verdict = "open"
        print(f"{name:4} {gpio:3} {pin:4}  {verdict:28} {'followers ' + ', '.join(followers) if followers else 'ok'}")


if __name__ == "__main__":
    main()
