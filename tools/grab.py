"""Start the P4's NTSC decoder and save one decoded 720x480 frame as a colour PNG: grab.py [channel].
Talks to the P4's CH343 console; the Nano must be running passthru_top."""
import base64
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from iqcap import cmd  # noqa: E402
from p4cmd import open_console  # noqa: E402
from still import write_png  # noqa: E402


def packed420_to_rgb(data, width, height):
    """Rows of (C, Y0, Y1) per pixel pair, C being V on even rows and U on odd ones."""
    rgb = bytearray(width * height * 3)
    stride = width * 3 // 2
    clamp = lambda v: 0 if v < 0 else 255 if v > 255 else int(v)
    for y in range(height):
        even = y & ~1
        for x in range(0, width, 2):
            o = y * stride + x * 3 // 2
            v = data[even * stride + x * 3 // 2] - 128
            u = data[(even + 1) * stride + x * 3 // 2] - 128
            for k in range(2):
                luma = (data[o + 1 + k] - 16) * 1.164
                p = (y * width + x + k) * 3
                rgb[p] = clamp(luma + 1.596 * v)
                rgb[p + 1] = clamp(luma - 0.392 * u - 0.813 * v)
                rgb[p + 2] = clamp(luma + 2.017 * u)
    return bytes(rgb)


def main():
    channel = sys.argv[1] if len(sys.argv) > 1 else "R4"
    s = open_console(uart=True)
    cmd(s, f"decode on {channel}", wait=40)
    time.sleep(1)
    cmd(s, "decode")
    s.write(b"video grab\r")
    buf, end = b"", time.time() + 120
    while time.time() < end and b"video grab end" not in buf and b"err " not in buf[-200:]:
        buf += s.read(65536)
    cmd(s, "video")
    cmd(s, "decode")
    text = buf.decode(errors="replace")
    if "video grab begin" not in text:
        sys.exit(text.strip())
    head, body = text[text.index("video grab begin"):text.index("video grab end")].split("\n", 1)
    width, height = map(int, head.split()[3:5])
    data = base64.b64decode("".join(body.split()))
    out = pathlib.Path("captures") / f"frame-{channel}-{time.strftime('%Y%m%d-%H%M%S')}.png"
    out.parent.mkdir(exist_ok=True)
    write_png(out, width, height, packed420_to_rgb(data, width, height), colour=True)
    print(f"saved {out}")


if __name__ == "__main__":
    main()
