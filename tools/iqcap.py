"""Capture I/Q from the C5 through the Nano pass-through and save it: iqcap.py [samples] [channel].
Talks to the P4's CH343 console, which resets the P4, so it sets everything up in one session.
Writes captures/iq-<channel>-<time>.u16 (little-endian words: I low byte, Q high byte, each 2v+1)."""
import base64
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from p4cmd import open_console  # noqa: E402


def cmd(s, line, wait=6.0):
    s.write((line + "\r").encode())
    out, end = b"", time.time() + wait
    while time.time() < end and not out.rstrip().endswith(b">"):
        out += s.read(65536)
    text = out.decode(errors="replace")
    print(text.strip())
    return text


def main():
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 131072
    channel = sys.argv[2] if len(sys.argv) > 2 else "R4"
    s = open_console(uart=True)
    every = sys.argv[3] if len(sys.argv) > 3 else "2"
    cmd(s, f"iq start {channel} {every}", wait=30)
    cmd(s, f"iq {samples}", wait=10)
    s.write(b"iq dump\r")
    data, buf, end = [], b"", time.time() + samples / 2000 + 30
    while time.time() < end:
        buf += s.read(65536)
        if b"iq dump end" in buf:
            break
    cmd(s, "pins bus off")
    text = buf.decode(errors="replace")
    body = text[text.index("iq dump begin"):text.index("iq dump end")].splitlines()[1:]
    raw = base64.b64decode("".join(line.strip() for line in body))
    out = pathlib.Path("captures") / f"iq-{channel}-{time.strftime('%Y%m%d-%H%M%S')}.u16"
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(raw)
    print(f"saved {len(raw) // 2} samples to {out}")


if __name__ == "__main__":
    main()
