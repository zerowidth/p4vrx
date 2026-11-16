"""Grab one decoded field from the R4 camera (or another channel) and save it as a PNG: still.py [channel] [inv].
Talks to the P4's CH343 console; the Nano must be running passthru_top."""
import base64
import pathlib
import struct
import sys
import time
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from iqcap import cmd  # noqa: E402
from p4cmd import open_console  # noqa: E402


def write_png(path, width, height, pixels, colour=False):
    row = width * (3 if colour else 1)
    rows = b"".join(b"\0" + pixels[y * row:(y + 1) * row] for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2 if colour else 0, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    channel = sys.argv[1] if len(sys.argv) > 1 else "R4"
    inv = len(sys.argv) > 2 and sys.argv[2] == "inv"
    s = open_console(uart=True)
    for line in (f"iq start {channel}",):
        cmd(s, line)
    cmd(s, "iq 1400000", wait=10)
    s.write(b"still inv\r" if inv else b"still\r")
    buf, end = b"", time.time() + 60
    while time.time() < end and b"still end" not in buf and b"err" not in buf[-200:]:
        buf += s.read(65536)
    cmd(s, "pins bus off")
    text = buf.decode(errors="replace")
    if "still begin" not in text:
        sys.exit(text.strip())
    head, body = text[text.index("still begin"):text.index("still end")].split("\n", 1)
    print(head)
    width, height = map(int, head.split()[2:4])
    grey = base64.b64decode("".join(body.split()))
    out = pathlib.Path("captures") / f"still-{channel}-{time.strftime('%Y%m%d-%H%M%S')}.png"
    out.parent.mkdir(exist_ok=True)
    write_png(out, width, height, grey)
    print(f"saved {out}")


if __name__ == "__main__":
    main()
