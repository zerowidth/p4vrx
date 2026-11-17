"""Runs fpga/sim/fm_demod_tb.v on a capture and checks its output against models/fm_demod.py bit for bit.

    uv run --with numpy python models/fm_demod_check.py captures/iq-R4-20261116-020228.u16 [samples]
"""

import subprocess
import sys

import numpy as np

import fm_demod as m


def main(path, n=60000):
    w = np.fromfile(path, dtype="<u2")[:n]
    i, q = m.lanes(path)
    dc_i, dc_q = int(round(i.mean())), int(round(q.mean()))
    i, q = i[:n], q[:n]
    want = m.nano(i, q, dc_i, dc_q, m.fir_taps())
    np.savetxt("/tmp/fm_in.hex", w, fmt="%04x")
    subprocess.run(["iverilog", "-o", "/tmp/fm_tb", "fpga/sim/fm_demod_tb.v", "fpga/rtl/fm_demod.v"], check=True)
    subprocess.run(["vvp", "-n", "/tmp/fm_tb", f"+n={n}", f"+dci={dc_i}", f"+dcq={dc_q}"], check=True, stdout=subprocess.DEVNULL)
    # Unknown until the pipeline fills: those outputs are skipped below anyway.
    got = np.array([int(v) if v.strip().lstrip("-").isdigit() else 0 for v in open("/tmp/fm_out.txt")], dtype=np.int64)
    # The RTL's pipeline delay and decimation phase are its own: find where its stream matches the model's.
    for start in range(60):
        g = got[start:start + len(want) - 20]
        for offset in range(3):
            ref = m.nano(i[offset:], q[offset:], dc_i, dc_q, m.fir_taps())[: len(g)]
            if len(ref) == len(g) and np.array_equal(g[100:], ref[100:]):
                print(f"{path}: {len(g) - 100} outputs match the model bit for bit (RTL lag {start}, phase {offset})")
                return True
    best = got[: len(want)]
    print(f"{path}: no match; first outputs RTL {best[20:30].tolist()} model {want[:10].tolist()}")
    return False


if __name__ == "__main__":
    ok = main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 60000)
    sys.exit(0 if ok else 1)
