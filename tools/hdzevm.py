"""Decision error of equalised data symbols (hdzrx_host's fN.E dumps): mean squared distance to the nearest 16-QAM
point, in dB, over bins |k| < 300 and over the outer bins 300..444, leaving out pilots.

    python tools/hdzevm.py DIR/fN.E.bin [...]
"""
import sys

import numpy as np

A = 1 / np.sqrt(5)
LV = np.array([-2, -1, 1, 2]) * A
CONT = [-444, -400, -360, -312, -280, -240, -224, -200, -168, -136, -96, -56, 56, 104, 128, 160, 184, 216, 264, 280,
        312, 364, 404, 444]


def evm(E, bins):
    z = E[:, np.array(bins) + 512]
    er = np.min((z.real[..., None] - LV) ** 2, -1) + np.min((z.imag[..., None] - LV) ** 2, -1)
    return 10 * np.log10(np.median(er))


for p in sys.argv[1:]:
    E = np.fromfile(p, np.complex64).reshape(-1, 1024)
    inner = [b for b in range(-299, 300) if not -11 <= b <= 3 and b not in CONT and b % 4]
    outer = [b for b in list(range(300, 445)) + list(range(-444, -299)) if b not in CONT and b % 4]
    print(f"{p}: median decision error {evm(E, inner):.1f} dB inner, {evm(E, outer):.1f} dB outer", flush=True)
