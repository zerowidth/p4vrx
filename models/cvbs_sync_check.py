"""Runs fpga/sim/cvbs_sync_tb.v on a capture's phase steps and checks its lines and fields against cvbs_sync.Sync.

    uv run --with numpy python models/cvbs_sync_check.py captures/grab8M-R4.u16 [samples]
"""

import subprocess
import sys

import numpy as np

import cvbs_sync as c


def main(path, n):
    hist = c.steps_from_capture(path)[:n]
    n = len(hist) // 4 * 4
    hist = hist[:n]
    s = c.Sync(hist).run()
    np.savetxt("/tmp/sync_in.hex", hist & 0xFF, fmt="%02x")
    subprocess.run(["iverilog", "-o", "/tmp/sync_tb", "fpga/sim/cvbs_sync_tb.v", "fpga/rtl/cvbs_sync.v", "fpga/rtl/sdp_ram.v"], check=True)
    out = subprocess.run(["vvp", "-n", "/tmp/sync_tb", f"+n={n}"], check=True, capture_output=True, text=True).stdout
    if out.strip():
        print(out.strip())
    got_l, got_f = [], []
    for line in open("/tmp/sync_out.txt"):
        f = line.split()
        if f[0] == "L":
            got_l.append(tuple(int(x) for x in f[1:]))
        elif f[0] == "F":
            got_f.append(tuple(int(x) for x in f[1:]))
    want_l = [(a & 0xFFFFFFFF, b, int(h), v, p) for a, b, h, v, p in s.lines]
    want_f = [(h, int(bool(v)), per) for h, v, _, per in s.field_log]
    first = next((k for k, (a, b) in enumerate(zip(got_l, want_l)) if a != b), None)
    ok = got_l == want_l and got_f == want_f
    print(f"{n} samples: RTL {len(got_l)} lines {len(got_f)} fields, model {len(want_l)} lines {len(want_f)} fields, "
          f"{'bit for bit' if ok else 'DIFFERENT'}")
    if first is not None:
        print(f"first different line {first}: RTL {got_l[first]} model {want_l[first]}")
        print("before:", want_l[max(0, first - 2):first])
    if got_f != want_f:
        print("fields RTL", got_f[:6], "\nfields model", want_f[:6])
    return ok


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 400000) else 1)
