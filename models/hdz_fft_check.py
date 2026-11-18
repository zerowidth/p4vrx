"""hdz_fft.v against fft_r22: random symbols through fpga/sim/hdz_fft_tb.v, compared bit for bit.

    uv run --with numpy python models/hdz_fft_check.py [symbols]
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import hdz_front as m  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main(nsym=6):
    rng = np.random.default_rng(3)
    # realistic and edge-of-range symbols
    re = rng.normal(0, 3000, (nsym, 1024)).round().astype(np.int64)
    im = rng.normal(0, 3000, (nsym, 1024)).round().astype(np.int64)
    re[-1] = rng.integers(-(1 << 16), 1 << 16, 1024)
    im[-1] = rng.integers(-(1 << 16), 1 << 16, 1024)
    fr, fi = m.fft_r22(re, im)
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "hdz_fft_in.txt"), "w") as f:
            f.write(f"{nsym}\n")
            for s in range(nsym):
                for a, b in zip(re[s], im[s]):
                    f.write(f"{a} {b}\n")
        for t in ("hdz_tw1024.hex", "hdz_tw256.hex", "hdz_tw64.hex", "hdz_tw16.hex"):
            os.symlink(os.path.join(ROOT, "fpga/rtl", t), os.path.join(d, t))
        src = [os.path.join(ROOT, p) for p in ("fpga/sim/hdz_fft_tb.v", "fpga/rtl/hdz_fft.v", "fpga/rtl/hdz_r22.v",
                                               "fpga/rtl/ce_ring.v")]
        subprocess.run(["iverilog", "-g2012", "-o", os.path.join(d, "tb")] + src, check=True)
        subprocess.run(["vvp", "-n", "tb"], cwd=d, check=True, stdout=subprocess.DEVNULL)
        out = np.loadtxt(os.path.join(d, "hdz_fft_out.txt"), dtype=np.int64).reshape(-1, 1024, 2)
    bad = 0
    for s in range(nsym):
        ok = np.array_equal(out[s, :, 0], fr[s]) and np.array_equal(out[s, :, 1], fi[s])
        if not ok:
            k = np.flatnonzero((out[s, :, 0] != fr[s]) | (out[s, :, 1] != fi[s]))
            print(f"symbol {s}: {len(k)} bins differ, first at {k[0]}: rtl {out[s, k[0]]} model {fr[s, k[0]]} {fi[s, k[0]]}")
            bad += 1
    print(f"{nsym - bad} of {nsym} symbols bit for bit")
    return bad


if __name__ == "__main__":
    sys.exit(main(*(int(a) for a in sys.argv[1:])) and 1)
