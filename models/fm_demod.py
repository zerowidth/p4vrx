"""Fixed-point model of the Nano's FM demodulator, checked against a float reference and the P4's demod.

The Nano takes the C5's 7+7-bit I/Q at 40 MS/s, finds each sample's phase by CORDIC, differences it at
40 MS/s, low-passes and keeps one sample in three: frequency at 13.33 MS/s as a 14-bit word. The P4 today
looks up an 8-bit phase per sample at 13.33 MS/s and differences that, with despeck for wraps.

    uv run --with numpy python models/fm_demod.py captures/iq-R4-20261116-020228.u16 [...]

Captures are iqcap.py's at 40 MS/s: little-endian words, I in the low byte and Q in the high, each 2v+1.
"""

import sys

import numpy as np

FS = 40e6
DECIMATE = 3
ITERATIONS = 10
# Phase in units of a full turn over 2^PHASE_BITS.
PHASE_BITS = 12
OUT_BITS = 14
# Output LSB in Hz: a 40 MS/s phase step's LSB, over the gain the FIR adds (2^FIR_SHIFT ... kept as 4x).
OUT_FRAC = 2
FIR_SHIFT = 10


def lanes(path):
    w = np.fromfile(path, dtype="<u2")
    i = (w & 0xFF).astype(np.uint8).view(np.int8).astype(np.int32)
    q = (w >> 8).astype(np.uint8).view(np.int8).astype(np.int32)
    return i, q


def fir_taps(n=15, cutoff=5.5e6):
    """Integer low-pass taps summing to 2^FIR_SHIFT, symmetric."""
    k = np.arange(n) - (n - 1) / 2
    h = np.sinc(2 * cutoff / FS * k) * np.hamming(n)
    h /= h.sum()
    t = np.round(h * (1 << FIR_SHIFT)).astype(np.int64)
    t[n // 2] += (1 << FIR_SHIFT) - t.sum()
    return t


# CORDIC angles atan(2^-k) in phase units, with PHASE_BITS + 2 guard bits.
GUARD = 2
ATAN = [int(round(np.arctan(2.0**-k) / (2 * np.pi) * (1 << (PHASE_BITS + GUARD)))) for k in range(ITERATIONS)]


def cordic_phase(i, q):
    """Vectoring CORDIC: the angle of (i, q) in units of 2^-PHASE_BITS turns, as the RTL computes it."""
    x = i.astype(np.int64) << 2
    y = q.astype(np.int64) << 2
    turn = 1 << (PHASE_BITS + GUARD)
    # Into the right half plane: a half turn when x < 0.
    z = np.where(x < 0, turn // 2, 0).astype(np.int64)
    x, y = np.where(x < 0, -x, x), np.where(x < 0, -y, y)
    for k in range(ITERATIONS):
        d = np.where(y >= 0, 1, -1)
        x, y = x + d * (y >> k), y - d * (x >> k)
        z = z + d * ATAN[k]
    return (z >> GUARD) & ((1 << PHASE_BITS) - 1)


def nano(i, q, dc_i, dc_q, taps):
    """The Nano's output words at 13.33 MS/s, signed, LSB = FS / 2^PHASE_BITS / 2^OUT_FRAC Hz."""
    ph = cordic_phase(i - dc_i, q - dc_q)
    m = 1 << PHASE_BITS
    d = (np.diff(ph) + m // 2) % m - m // 2
    f = np.convolve(d, taps)[len(taps) - 1 :: DECIMATE]
    out = f >> (FIR_SHIFT - OUT_FRAC)
    lim = 1 << (OUT_BITS - 1)
    return np.clip(out, -lim, lim - 1)


def nano_hz(words):
    return words * (FS / (1 << PHASE_BITS) / (1 << OUT_FRAC))


def p4(i, q, dc_i, dc_q):
    """Today's P4 path in Hz: an 8-bit phase per 13.33 MS/s sample, differenced, with despeck's rule."""
    i, q = i[::DECIMATE], q[::DECIMATE]
    ph = np.round(np.arctan2(q - dc_q, i - dc_i) / (2 * np.pi) * 256).astype(np.int64) & 0xFF
    d = ((np.diff(ph) + 128) % 256 - 128).astype(np.float64)
    # A sample half a turn or more from the mean of its neighbours wrapped: it goes to the end of the range.
    mean = (np.roll(d, 1) + np.roll(d, -1)) / 2
    wrapped = np.abs(d - mean) >= 128
    d[wrapped] = np.where(d[wrapped] < mean[wrapped], 127, -128)
    return d * (FS / DECIMATE / 256)


def reference(i, q, dc_i, dc_q, cutoff=5.5e6, n=63):
    """Float: unwrapped phase at 40 MS/s, differenced, a long low-pass, one sample in three."""
    d = np.diff(np.unwrap(np.arctan2(q - dc_q, i - dc_i))) * FS / (2 * np.pi)
    k = np.arange(n) - (n - 1) / 2
    h = np.sinc(2 * cutoff / FS * k) * np.blackman(n)
    h /= h.sum()
    return np.convolve(d, h)[n - 1 :: DECIMATE]


def same_filter(i, q, dc_i, dc_q, taps):
    """Float phase through the Nano's own FIR: the Nano less this is its fixed-point loss alone."""
    d = np.diff(np.unwrap(np.arctan2(q - dc_q, i - dc_i))) * FS / (2 * np.pi)
    return np.convolve(d, taps / taps.sum())[len(taps) - 1 :: DECIMATE]


def align(a, b):
    """The lag of b against a that matches best, and both trimmed to it."""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    best = min(range(-12, 13), key=lambda s: np.mean((a[30:-30] - np.roll(b, s)[30:-30]) ** 2))
    b = np.roll(b, best)
    return a[30:-30], b[30:-30], best


def tip_noise(f):
    """RMS about a smoothed copy, over samples in the lowest 4% (sync tips), as the decoder's tip noise."""
    smooth = np.convolve(f, np.ones(9) / 9, mode="same")
    tips = f < np.percentile(f, 4)
    return np.sqrt(np.mean((f - smooth)[tips] ** 2))


def specks(f, ref):
    return int(np.sum(np.abs(f - ref) > 1.5e6))


def main(paths):
    taps = fir_taps()
    print(f"FIR {len(taps)} taps: {taps.tolist()}; CORDIC {ITERATIONS} iterations, {PHASE_BITS}-bit phase")
    print(f"output LSB {nano_hz(1) / 1e3:.2f} kHz, range +/-{nano_hz(1 << (OUT_BITS - 1)) / 1e6:.1f} MHz")
    for path in paths:
        i, q = lanes(path)
        dc_i, dc_q = int(round(i.mean())), int(round(q.mean()))
        ref = reference(i, q, dc_i, dc_q)
        n_hz = nano_hz(nano(i, q, dc_i, dc_q, taps).astype(np.float64))
        p_hz = p4(i, q, dc_i, dc_q)
        rn, nn, lag_n = align(ref, n_hz)
        rp, pp, lag_p = align(ref, p_hz)
        exact = same_filter(i, q, dc_i, dc_q, taps)
        loss = n_hz[: len(exact)] - exact[: len(n_hz)]
        radius = np.sqrt((i - dc_i) ** 2 + (q - dc_q) ** 2)
        print(f"\n{path.split('/')[-1]}: {len(i)} samples, I/Q offset {dc_i} {dc_q}, radius median {np.median(radius):.0f}")
        print(f"  nano fixed point vs the same filter in float: {np.sqrt(np.mean(loss[20:-20] ** 2)) / 1e3:.1f} kHz rms")
        for name, r, x, lag in (("nano", rn, nn, lag_n), ("p4", rp, pp, lag_p)):
            err = x - r
            print(f"  {name:4s} vs reference: error rms {np.sqrt(np.mean(err ** 2)) / 1e3:6.1f} kHz, "
                  f"specks {specks(x, r):4d}, tip noise {tip_noise(x) / 1e3:6.1f} kHz "
                  f"(reference {tip_noise(r) / 1e3:.1f}), distinct values {len(np.unique(np.round(x)))}, lag {lag}")


if __name__ == "__main__":
    main(sys.argv[1:])
