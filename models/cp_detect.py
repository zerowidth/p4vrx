"""Cyclic-prefix detector for HDZero, on 40 MS/s captures: float, and the fixed point the Nano would run.

HDZero's OFDM has a 1024-point FFT and a 32-sample prefix at 27 MS/s: at 40 MS/s the prefix repeats the
symbol's tail 1517 samples later, over about 47 samples, once every 1564.39. Analog FM has nothing at that lag.

    uv run --with numpy python models/cp_detect.py FILE[:skip] ...

Files are u16le words, I in the low byte and Q in the high, each 2v+1 (iqcap.py, hdzero-captures).
"""

import sys

import numpy as np

LAG = 1517
WINDOW = 48
PERIOD = 1564.39
BLOCK = 1565
# Symbols per decision: 64 is 2.5 ms.
SYMBOLS = 64


def load(path, skip=0, n=4_000_000):
    w = np.fromfile(path, dtype="<u2")[skip : skip + n]
    i = (w & 0xFF).astype(np.uint8).view(np.int8).astype(np.int32)
    q = (w >> 8).astype(np.uint8).view(np.int8).astype(np.int32)
    return i, q


def moving(x, n):
    c = np.cumsum(x, dtype=np.int64 if x.dtype.kind in "iu" else None)
    return c[n:] - c[:-n]


def correlate(i, q, lag=LAG, window=WINDOW):
    """|sum x[n] x*[n-lag]| over the window, over the window's energy at both ends: 1 for a perfect repeat."""
    # A slow mean off, as the C5's offset moves with its gain.
    i = i - np.convolve(i, np.ones(2048) / 2048, mode="same").round().astype(np.int32)
    q = q - np.convolve(q, np.ones(2048) / 2048, mode="same").round().astype(np.int32)
    a_i, a_q, b_i, b_q = i[lag:], q[lag:], i[:-lag], q[:-lag]
    re = moving(a_i * b_i + a_q * b_q, window)
    im = moving(a_q * b_i - a_i * b_q, window)
    ea = moving(a_i * a_i + a_q * a_q, window)
    eb = moving(b_i * b_i + b_q * b_q, window)
    return np.hypot(re, im) / np.maximum(np.sqrt(ea.astype(float) * eb), 1), re, im


def fold_ratio(rho, period=PERIOD, symbols=SYMBOLS):
    """The folded profile's peak over its mean, per run of `symbols` symbols."""
    out = []
    span = int(period * symbols)
    for start in range(0, len(rho) - span, span):
        r = rho[start : start + span]
        bins = (np.arange(span) % period).astype(int)
        prof = np.bincount(bins, weights=r, minlength=int(period) + 1) / np.maximum(np.bincount(bins, minlength=int(period) + 1), 1)
        out.append(prof.max() / prof.mean())
    return np.array(out)


def block_ratio(c2, block=BLOCK, symbols=SYMBOLS):
    """Each block's peak |c|^2 over its mean, averaged over `symbols` blocks: no folding, no fractional period."""
    out = []
    n = len(c2) // block
    per = c2[: n * block].reshape(n, block)
    r = per.max(axis=1) / np.maximum(per.mean(axis=1), 1e-9)
    for start in range(0, n - symbols + 1, symbols):
        out.append(np.mean(r[start : start + symbols]))
    return np.array(out)


def fixed_fold(i, q, symbols=SYMBOLS, lag=LAG, window=WINDOW, period=PERIOD):
    """The Nano's version: 7-bit lanes less a 2048-sample mean, integer |c| as max + min/2, folded into 1565
    bins by a 16.16 phase counter that steps 1/period of a turn a sample, then the fold's peak over its mean."""
    i = (i - np.convolve(i, np.ones(2048) / 2048, mode="same").round().astype(np.int32)) >> 1
    q = (q - np.convolve(q, np.ones(2048) / 2048, mode="same").round().astype(np.int32)) >> 1
    a_i, a_q, b_i, b_q = i[lag:], q[lag:], i[:-lag], q[:-lag]
    re = np.abs(moving(a_i * b_i + a_q * b_q, window))
    im = np.abs(moving(a_q * b_i - a_i * b_q, window))
    mag = np.maximum(re, im) + np.minimum(re, im) // 2
    step = int(round(65536 * 65536 / period))
    bins = ((np.arange(len(mag), dtype=np.int64) * step) % (1 << 32) * 1565) >> 32
    out = []
    span = int(period * symbols)
    for start in range(0, len(mag) - span, span):
        acc = np.bincount(bins[start : start + span], weights=mag[start : start + span], minlength=1565)
        out.append(acc.max() * 1565 / max(acc.sum(), 1))
    return np.array(out)


def fixed(i, q, lag=LAG, window=WINDOW):
    """What the Nano would do with the 7-bit lanes: integer products and sums, |c| as max + min/2 (within
    12%), and the block ratio from |c| rather than |c|^2. No offset removal beyond a 2048-sample mean."""
    i = (i - np.convolve(i, np.ones(2048) / 2048, mode="same").round().astype(np.int32)) >> 1
    q = (q - np.convolve(q, np.ones(2048) / 2048, mode="same").round().astype(np.int32)) >> 1
    a_i, a_q, b_i, b_q = i[lag:], q[lag:], i[:-lag], q[:-lag]
    re = np.abs(moving(a_i * b_i + a_q * b_q, window))
    im = np.abs(moving(a_q * b_i - a_i * b_q, window))
    mag = np.maximum(re, im) + np.minimum(re, im) // 2
    return block_ratio(mag.astype(float))


def main(args):
    print(f"lag {LAG}, window {WINDOW}, decisions every {SYMBOLS} symbols ({SYMBOLS * PERIOD / 40e3:.1f} ms)")
    for arg in args:
        path, _, skip = arg.partition(":")
        i, q = load(path, int(skip or 0))
        rho, re, im = correlate(i, q)
        f = fold_ratio(rho)
        b = block_ratio(re.astype(float) ** 2 + im.astype(float) ** 2)
        x = fixed(i, q)
        ff = {n: fixed_fold(i, q, n) for n in (16, 64)}
        name = path.split("/")[-1]
        print(f"{name:32s} {len(i):8d} samples: rho median {np.median(rho):.3f}, 99.9% {np.percentile(rho, 99.9):.3f}; "
              f"fold peak/mean {f.min():.2f}-{f.max():.2f}; block |c|^2 {b.min():.2f}-{b.max():.2f}; "
              f"fixed fold 16 sym {ff[16].min():.2f}-{ff[16].max():.2f}, 64 sym {ff[64].min():.2f}-{ff[64].max():.2f}")


if __name__ == "__main__":
    main(sys.argv[1:])
