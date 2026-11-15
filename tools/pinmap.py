"""Walk a one (or, on the I2C pins, a zero) across every P4 <-> Nano wire in both directions, with the
Nano running fpga/boards/tangnano20k/pintest_top. Prints one line per wire: ok, open, swapped or shorted."""
import sys
import time

import serial
from serial.tools.list_ports import comports

# Name, P4 GPIO, Nano pin; order matches the Nano's p4 bits 15..0, then i2c bits 1..0.
LANES = [("PCLK", 4, 26), ("D13", 2, 29), ("D12", 3, 30), ("D11", 21, 31), ("D10", 22, 15), ("D9", 5, 16),
         ("D8", 49, 17), ("D7", 28, 20), ("D6", 29, 19), ("D5", 30, 18), ("D4", 31, 72), ("D3", 32, 71),
         ("D2", 46, 54), ("D1", 47, 55), ("D0", 48, 56), ("IRQ", 52, 79)]
I2C = [("SCL", 8, 53), ("SDA", 7, 52)]
WIRES = LANES + I2C


def find(vid, pid, pick=None):
    ports = sorted(p.device for p in comports() if (p.vid, p.pid) == (vid, pid))
    if pick is not None:
        ports = [p for p in ports if p.endswith(pick)]
    if len(ports) != 1:
        sys.exit(f"expected one {vid:04x}:{pid:04x} port, found {ports}")
    return ports[0]


class P4:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = self.s.rts = False
        self.s.open()
        # Opening the CH343 resets the P4 through the board's auto-reset circuit.
        time.sleep(3)
        self.cmd("")

    def cmd(self, line):
        self.s.reset_input_buffer()
        self.s.write((line + "\r").encode())
        out, end = b"", time.time() + 2
        while time.time() < end and not out.rstrip().endswith(b">"):
            out += self.s.read(4096)
        return out.decode(errors="replace")

    def read(self):
        for tok in self.cmd("pins read").split():
            if tok == "pins":
                fields = {}
            elif "=" in tok:
                k, v = tok.split("=")
                fields[k] = int(v)
        return [fields[n] for n, _, _ in WIRES]


class Nano:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.5)

    def send(self, b):
        self.s.write(b)
        time.sleep(0.005)

    def read(self):
        for _ in range(5):
            self.s.reset_input_buffer()
            self.s.write(b"r")
            line = self.s.readline().decode(errors="replace").split()
            if len(line) == 3 and all(t.count("=") == 1 for t in line):
                break
        f = dict(t.split("=") for t in line)
        p4, i2c = int(f["p4"], 16), int(f["i2c"], 16)
        return [(p4 >> (15 - i)) & 1 for i in range(16)] + [(i2c >> 1) & 1, i2c & 1]


def verdict(i, seen, idle):
    """seen: levels while wire i is driven; idle: levels with nothing driven."""
    changed = [j for j in range(len(WIRES)) if seen[j] != idle[j]]
    others = [WIRES[j][0] for j in changed if j != i]
    if i in changed and not others:
        return "ok"
    if i in changed:
        return "shorted to " + ", ".join(others)
    if others:
        return "swapped with " + ", ".join(others)
    return "open"


def main():
    # The CH343 UART console: the USB console still stalls after a few replies.
    p4 = P4(find(0x1A86, 0x55D3))
    nano = Nano(find(0x0403, 0x6010, pick="1"))
    p4.cmd("pins release")
    nano.send(b"z")
    idle_nano, idle_p4 = nano.read(), p4.read()
    print("idle  Nano sees", idle_nano, " P4 sees", idle_p4)

    results = []
    for i, (name, gpio, pin) in enumerate(WIRES):
        i2c = name in ("SCL", "SDA")
        level = 0 if i2c else 1
        letter = bytes([ord("a") + i])

        p4.cmd(f"pins drive {name} {level}")
        p4_to_nano = verdict(i, nano.read(), idle_nano)
        p4_self = verdict(i, p4.read(), idle_p4)
        p4.cmd("pins release")

        nano.send((b"l" if i2c else b"h") + letter)
        nano_to_p4 = verdict(i, p4.read(), idle_p4)
        nano.send(b"z")
        results.append((name, gpio, pin, p4_to_nano, nano_to_p4, p4_self))

    print(f"{'wire':5} {'P4':>4} {'Nano':>4}  {'P4 -> Nano':24} {'Nano -> P4':24} P4 own pins")
    for name, gpio, pin, a, b, c in results:
        print(f"{name:5} {gpio:4} {pin:4}  {a:24} {b:24} {c}")
    p4.cmd("pins release")
    nano.send(b"z")


if __name__ == "__main__":
    main()
