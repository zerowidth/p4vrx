"""Save what the P4's panel shows as a landscape PNG: lcdgrab.py OUT.png [90|270].
Reads the frame buffer last handed to the panel over the USB console; the angle is `lcd rotate`'s (270 by default)."""
import base64
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from p4cmd import open_console  # noqa: E402
from still import write_png  # noqa: E402


def main():
    out = sys.argv[1]
    angle = int(sys.argv[2]) if len(sys.argv) > 2 else 270
    s = open_console()
    s.write(b"lcd grab\r")
    buf, end = b"", time.time() + 30
    while time.time() < end and b"lcd grab end" not in buf:
        buf += s.read(65536)
    text = buf.decode(errors="replace")
    head = text[text.index("lcd grab begin"):].splitlines()
    w, h = (int(v) for v in head[0].split()[3:5])
    body = text[text.index("lcd grab begin"):text.index("lcd grab end")].splitlines()[1:]
    raw = base64.b64decode("".join(line.strip() for line in body))
    px = [raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2)]
    # Landscape is h wide and w tall; undo the panel's turn so the picture reads as the viewer sees it.
    rgb = bytearray(h * w * 3)
    for y in range(w):
        for x in range(h):
            p = px[x * w + (w - 1 - y)] if angle == 270 else px[(h - 1 - x) * w + y]
            o = (y * h + x) * 3
            rgb[o], rgb[o + 1], rgb[o + 2] = (p >> 11) << 3, (p >> 5 & 63) << 2, (p & 31) << 3
    write_png(pathlib.Path(out), h, w, bytes(rgb), colour=True)
    print(out)


if __name__ == "__main__":
    main()
