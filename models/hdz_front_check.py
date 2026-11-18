"""hdz_front.v against models/hdz_front.py: the same capture samples through both, every non-idle bus word compared.

    uv run --with numpy python models/hdz_front_check.py CAPTURE_PREFIX [samples] [--verilator]
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import hdz_front as m  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RTL = ["fpga/rtl/hdz_front.v", "fpga/rtl/hdz_fft.v", "fpga/rtl/hdz_r22.v", "fpga/rtl/ce_ring.v"]
TABLES = ["hdz_resample.hex", "hdz_nco.hex", "hdz_gain_c5narrow.hex", "hdz_tw1024.hex", "hdz_tw256.hex",
          "hdz_tw64.hex", "hdz_tw16.hex"]


def main(prefix, samples=400_000):
    i, q = m.lanes(prefix.removesuffix(".bin"), samples)
    words, info = m.front(i, q, gain=m.load_gain())
    raw = ((i & 0xFF) | ((q & 0xFF) << 8)).astype(np.uint16)
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "hdz_front_in.txt"), "w") as f:
            f.write("".join(f"{v:04x}\n" for v in raw))
        for t in TABLES:
            os.makedirs(os.path.join(d, "fpga/rtl"), exist_ok=True)
            os.symlink(os.path.join(ROOT, "fpga/rtl", t), os.path.join(d, "fpga/rtl", t))
        subprocess.run(["iverilog", "-g2012", "-o", os.path.join(d, "tb"), os.path.join(ROOT, "fpga/sim/hdz_front_tb.v")]
                       + [os.path.join(ROOT, p) for p in RTL], check=True)
        r = subprocess.run(["vvp", "-n", "tb"], cwd=d, check=True, capture_output=True, text=True)
        print("\n".join(l for l in r.stdout.strip().splitlines() if "finish" not in l))
        out = np.array([int(l, 16) for l in open(os.path.join(d, "hdz_front_out.txt")).read().split()], np.uint16)
    os.makedirs(os.path.join(ROOT, "captures/hdzf"), exist_ok=True)
    out.tofile(os.path.join(ROOT, "captures/hdzf/rtl.u16"))
    words.tofile(os.path.join(ROOT, "captures/hdzf/model.u16"))
    n = min(len(out), len(words))
    diff = np.flatnonzero(out[:n] != words[:n])
    per = 5 + m.N
    print(f"model {len(words)} words ({info['symbols']} symbols), rtl {len(out)} words; "
          f"{n // per} symbols compared, peaks {info['peaks'][:4]}, thetas {info['thetas'][:3]}")
    if len(diff):
        k = diff[0]
        print(f"first difference at word {k} (symbol {k // per}, word {k % per}): rtl {out[k]:04x} model {words[k]:04x};"
              f" {len(diff)} words differ")
        return 1
    print("bit for bit")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], *(int(a) for a in sys.argv[2:3])))
