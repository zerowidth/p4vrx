"""Send console commands to the P4 and print the replies: p4cmd.py [-u] CMD [CMD ...].
-u uses the CH343 UART console (which resets the P4 when opened) instead of the USB console."""
import sys
import time

import serial
from serial.tools.list_ports import comports


def console_port(uart=False):
    vid_pid = (0x1A86, 0x55D3) if uart else (0x303A, 0x8000)
    ports = [p.device for p in comports() if (p.vid, p.pid) == vid_pid]
    if len(ports) != 1:
        sys.exit(f"expected one P4 console ({vid_pid[0]:04x}:{vid_pid[1]:04x}), found {ports}")
    return ports[0]


def open_console(uart=False):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = console_port(uart), 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    if uart:
        time.sleep(3)  # the CH343's auto-reset circuit restarts the P4
        s.read(65536)
    return s


def run(s, cmds, wait=1.0):
    for cmd in cmds:
        wait_for = float(cmd.split("@")[1]) if "@" in cmd else wait
        cmd = cmd.split("@")[0]
        s.write((cmd + "\r").encode())
        end = time.time() + wait_for
        out = b""
        while time.time() < end:
            out += s.read(4096)
            if out.rstrip().endswith(b">"):
                break
        sys.stdout.write(out.decode(errors="replace"))
        sys.stdout.flush()


if __name__ == "__main__":
    args = sys.argv[1:]
    uart = bool(args) and args[0] == "-u"
    run(open_console(uart), args[1:] if uart else args)
