"""A raw I/Q grab's health after the P4 has dropped doubled words (Nano build 6 on): how many it dropped, how often
neighbouring samples still repeat exactly, and how many jumps of 64 or more lane units are left (a wrong guess at
which words were flipped would leave one every other sample).

    uv run --with pyserial --with numpy python tools/grabcheck.py [--samples 1400000] [--repeat 3]
"""
import argparse
import base64
import pathlib
import sys
import time
import zlib

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hdzwatch import B64, Console, keyvals  # noqa: E402


def stats(w):
    i = ((w & 0xFF).astype(np.uint8).view(np.int8).astype(int) - 1) >> 1
    q = ((w >> 8).astype(np.uint8).view(np.int8).astype(int) - 1) >> 1
    di, dq = np.abs(np.diff(i)), np.abs(np.diff(q))
    return int(np.sum(w[1:] == w[:-1])), int(np.sum((di >= 64) | (dq >= 64))), float(np.sqrt(np.mean(i ** 2 + q ** 2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=1_400_000)
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--extra", default="", help="more iq grab words, e.g. wide")
    a = ap.parse_args()
    con = Console()
    for _ in range(a.repeat):
        reply = [l for l in con.cmd(f"iq grab {a.samples} hold {a.extra}", 15) if l.startswith(("ok grab", "err"))]
        if not reply or not reply[0].startswith("ok"):
            con.cmd("iq resume", 2)
            sys.exit(f"grab: {reply}")
        info = keyvals(reply[0])
        con.s.write(b"iq dump\r")
        body = con.lines(time.time() + 40 + a.samples / 50_000, rb"iq dump end")
        raw = base64.b64decode(b"".join(l for l in body if B64.match(l) and not l.startswith(b"iq")))
        con.lines(time.time() + 0.5, rb"^> ?$")
        con.cmd("iq resume", 2)
        w = np.frombuffer(raw, np.uint16)
        rep, jumps, rms = stats(w)
        print(f"samples {info['samples']} (asked {a.samples}), doubled {info.get('doubled')} dropped, CRC "
              f"{'ok' if f'{zlib.crc32(raw):08x}' == info.get('crc') else 'BAD'}; neighbours equal {rep} "
              f"({1e6 * rep / len(w):.0f} ppm), jumps of 64+ {jumps}, rms {rms:.1f}", flush=True)


if __name__ == "__main__":
    main()
