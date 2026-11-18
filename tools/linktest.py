"""The Nano-to-P4 link at 40 MHz: the Nano sends n x 0x2d5b mod 2^14 for clock n (`iq grab N count`), which toggles
about half the lanes each clock as spectra do, and this counts the words the P4 doubled or missed.

    uv run --with pyserial --with numpy python tools/linktest.py [--samples 400000] [--edge pos|neg] [--spectra]
"""
import argparse
import base64
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hdzwatch import B64, Console  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=400_000)
    ap.add_argument("--edge", default="pos")
    ap.add_argument("--spectra", action="store_true")
    ap.add_argument("--repeat", type=int, default=1)
    a = ap.parse_args()
    con = Console()
    for _ in range(a.repeat):
        mode = " spectra" if a.spectra else ""
        reply = [l for l in con.cmd(f"iq grab {a.samples}{mode} count {a.edge}", 15) if l.startswith(("ok grab", "err"))]
        if not reply or not reply[0].startswith("ok"):
            sys.exit(f"grab: {reply}")
        con.s.write(b"iq dump\r")
        body = con.lines(time.time() + 30 + a.samples / 50_000, rb"iq dump end")
        w = np.frombuffer(base64.b64decode(b"".join(l for l in body if B64.match(l) and not l.startswith(b"iq"))), np.uint16)
        con.lines(time.time() + 0.5, rb"^> ?$")
        i = (w & 0xFF) >> 1
        q = (w >> 8) >> 1
        # the Nano sends n x 0x2d5b mod 2^14; undo the factor
        c = ((i | q << 7).astype(np.int64) * pow(0x2D5B, -1, 1 << 14)) % (1 << 14)
        d = (c[1:] - c[:-1]) % (1 << 14)
        print(f"{a.edge}: {len(w)} words: {int(np.sum(d == 1))} steps of 1, {int(np.sum(d == 0))} doubled, "
              f"{int(np.sum(d > 1))} jumps (missed or garbled), largest {int(d.max())}; "
              f"bit 0 low in {int(np.sum((w & 0x0101) != 0x0101))} words", flush=True)


if __name__ == "__main__":
    main()
