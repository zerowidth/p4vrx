"""How well a decode's slots agree with a reference decode of the same capture: each frame of A against the frame of
B it agrees with most, over slots on bins neither flags as noisy.

    python tools/hdzagree.py A_DIR B_DIR      (directories of hdzrx_host dumps, fN.slots.bin)
"""
import pathlib
import sys

import numpy as np


def frames(d):
    out = []
    for p in sorted(pathlib.Path(d).glob("f*.slots.bin"), key=lambda p: int(p.name[1:].split(".")[0])):
        out.append(np.fromfile(p, np.uint8))
    return out


def main(a, b):
    fa, fb = frames(a), frames(b)
    for i, x in enumerate(fa):
        best = None
        for j, y in enumerate(fb):
            n = min(len(x), len(y))
            ok = ((x[:n] | y[:n]) & 0x30) == 0
            agree = np.mean((x[:n] & 15)[ok] == (y[:n] & 15)[ok]) if ok.any() else 0
            if best is None or agree > best[0]:
                best = (agree, j, ok.mean())
        print(f"A f{i} ~ B f{best[1]}: {100 * best[0]:.2f}% of slots agree, over {100 * best[2]:.0f}% of slots", flush=True)


if __name__ == "__main__":
    main(*sys.argv[1:3])
