"""Runs fpga/sim/cp_detect_tb.v on a capture and checks each decision against nano_fold bit for bit.

    uv run --with numpy python models/cp_detect_check.py FILE[:skip] [samples]
"""

import subprocess
import sys

import numpy as np

import cp_detect as m


def main(arg, n):
    path, _, skip = arg.partition(":")
    i, q = m.load(path, int(skip or 0), n)
    dc_i, dc_q = int(round(i.mean())), int(round(q.mean()))
    want = m.nano_fold(i.astype(np.int64), q.astype(np.int64), dc_i, dc_q)
    w = np.fromfile(path, dtype="<u2")[int(skip or 0) : int(skip or 0) + n]
    np.savetxt("/tmp/cp_in.hex", w, fmt="%04x")
    subprocess.run(["iverilog", "-o", "/tmp/cp_tb", "fpga/sim/cp_detect_tb.v", "fpga/rtl/cp_detect.v", "fpga/rtl/sdp_ram.v"], check=True)
    subprocess.run(["vvp", "-n", "/tmp/cp_tb", f"+n={len(w)}", f"+dci={dc_i}", f"+dcq={dc_q}"], check=True, stdout=subprocess.DEVNULL)
    got = [tuple(int(v) for v in line.split()) for line in open("/tmp/cp_out.txt") if line.strip()]
    ok = got == [tuple(x) for x in want]
    print(f"{path.split('/')[-1]}: {len(got)} decisions from the RTL, {len(want)} from the model, "
          f"{'bit for bit' if ok else 'DIFFERENT'}; ratios {[round(p * 1565 / t, 2) for p, t, _ in got]}")
    if not ok:
        print("rtl  ", got[:3], "\nmodel", want[:3])
    return ok


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 320000) else 1)
