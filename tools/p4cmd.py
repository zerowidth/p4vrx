"""Send console commands to the P4's USB console and print the replies: p4cmd.py CMD [CMD ...]."""
import sys
import time

import serial
from serial.tools.list_ports import comports


def console_port():
    ports = [p.device for p in comports() if (p.vid, p.pid) == (0x303A, 0x8000)]
    if len(ports) != 1:
        sys.exit(f"expected one P4 console (303a:8000), found {ports}")
    return ports[0]


def run(port, cmds, wait=1.0):
    with serial.Serial(port, 115200, timeout=0.1) as s:
        s.reset_input_buffer()
        for cmd in cmds:
            s.write((cmd + "\r").encode())
            end = time.time() + wait
            out = b""
            while time.time() < end:
                chunk = s.read(4096)
                out += chunk
                if out.rstrip().endswith(b">"):
                    break
            sys.stdout.write(out.decode(errors="replace"))
            sys.stdout.flush()


if __name__ == "__main__":
    run(console_port(), sys.argv[1:])
