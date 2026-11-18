"""Fixed-point model of the Nano's HDZero front end: 40 MS/s I/Q in, per-symbol 1024-point spectra out.

Blocks, in the order the RTL runs them, all integer:

1. DC: each sample less the mean of the 2,048 around it (a 1,024-sample delay line makes it causal).
2. Resample 40 -> 27 MS/s: a 27-phase polyphase FIR of TAPS taps, 9-bit data and coefficients (MULT9).
3. CP timing: y[n-1024] conj(y[n]) summed over 32 samples, its magnitude folded over 1,056 positions for 64
   symbols; the peak gives the symbol start. The next block sums the correlation at the peak, and a CORDIC's
   angle of it gives the fractional CFO, as an NCO step. Whole bins are left to the receiver (an index shift).
4. NCO: the samples turned by a phase accumulator through a 1,024-entry sin/cos table.
5. FFT: radix-2^2 decimation in frequency (the R2^2SDF's arithmetic), 18-bit, each butterfly halved; the bins
   come out in bit-reversed order, as the pipeline emits them, saturated to 7 bits after a final shift.

The output is what the P4 captures from the bus: 16-bit words, I in the low byte and Q in the high, each 2v+1
from the 7-bit lane value v (as raw I/Q). v = -64 never occurs in a bin, so it marks: (-64, -64) a symbol header,
whose next words carry {cfo_valid, resync, symbol count (12 bits)}, the window's offset in samples from where
it would be without timing corrections since the last resync (14 bits, signed), so the receiver can turn the
offset's phase ramp back out, and the AGC's gain (Q5) to divide back out; (-64, 0) is idle.

    uv run --with numpy python models/hdz_front.py CAPTURE_PREFIX OUT.u16 [--samples N] [--gain default|self|FILE.hex]

--gain default is fpga/rtl/hdz_gain_c5narrow.hex, measured on 191225 with the C5's 802.11p filter; self measures the
capture's own; without --gain every bin gets 1. --taps, --shift, --agc-target and --fft-back change the design for
experiments.
"""

import json
import sys

import numpy as np

N = 1024
CP = 32
SYM = N + CP
FFT_BACK = 8
UP, DOWN = 27, 40
TAPS = 8
COEF_BITS = 9
DC_HALF = 1024
BLOCK = 64
WINDOW = 32
NCO_BITS = 32
LUT_BITS = 10
LUT_SCALE = 2047
TW_BITS = 15
UPDATE_DELAY = 64
SMALL_STEP = 16
OUT_SHIFT = 4
AGC_START = 32
AGC_SYMBOLS = 64
AGC_TARGET = 14
MARK = -64


def lanes(prefix, samples=1_400_000):
    meta = json.load(open(prefix + ".json"))
    w = np.fromfile(prefix + ".bin", dtype="<u2")
    if len(w) > 1_400_000:
        w = w[meta.get("unsettled_leading_samples", 0):]
    w = w[:samples]
    i = (w & 0xFF).astype(np.uint8).view(np.int8).astype(np.int64)
    q = (w >> 8).astype(np.uint8).view(np.int8).astype(np.int64)
    return i, q


def dc(x):
    """(2048 x[n] - sum of x over [n-1024, n+1023]) >> 11, saturated to 9 bits; zeros before the start."""
    xp = np.concatenate([np.zeros(DC_HALF, np.int64), x])
    c = np.concatenate([[0], np.cumsum(xp)])
    n = np.arange(len(x) - (DC_HALF - 1))
    s = c[n + 2 * DC_HALF] - c[n]
    v = (2048 * x[: len(n)] - s) >> 11
    return np.clip(v, -256, 255)


def coefficients(taps=TAPS):
    """27 phases x taps, 9-bit, each phase summing to 256: Kaiser-windowed sinc, cutoff 12.5 MHz at 40 MS/s."""
    fc = 12.5 / 40
    h = np.zeros((UP, taps), np.int64)
    for p in range(UP):
        t = np.arange(taps) - (taps // 2 - 1) - p / UP
        w = np.kaiser(taps + 1, 5.0)
        win = np.interp(t, np.arange(taps + 1) - taps / 2, w)
        f = 2 * fc * np.sinc(2 * fc * t) * win
        f = f / f.sum() * 256
        r = np.round(f).astype(np.int64)
        r[np.argmax(np.abs(f))] += 256 - r.sum()
        h[p] = np.clip(r, -256, 255)
    return h


def resample(xd, h):
    """Output m from inputs i0 - taps/2 + 1 .. i0 + taps/2, i0 = floor(40 m / 27), phase (40 m) mod 27; Q4."""
    taps = h.shape[1]
    n_out = (len(xd) - taps) * UP // DOWN
    m = np.arange(n_out)
    i0 = (DOWN * m) // UP
    p = (DOWN * m) % UP
    m = m[i0 + taps // 2 < len(xd)]
    i0, p = i0[: len(m)], p[: len(m)]
    acc = np.zeros(len(m), np.int64)
    for k in range(taps):
        idx = i0 + k - (taps // 2 - 1)
        acc += h[p, k] * np.where(idx >= 0, xd[np.clip(idx, 0, len(xd) - 1)], 0)
    return acc >> 4


def sat(v, bits):
    lim = 1 << (bits - 1)
    return np.clip(v, -lim, lim - 1)


def mag(re, im):
    a, b = np.abs(re), np.abs(im)
    return np.maximum(a, b) + (np.minimum(a, b) >> 1)


ATAN = [int(round(np.arctan(2.0 ** -k) / (2 * np.pi) * (1 << 16))) for k in range(14)]


def cordic_angle(x, y):
    """Angle of (x, y) in 1/65536 turns (16-bit), vectoring CORDIC on integers after scaling to 24 bits."""
    x, y = int(x), int(y)
    while max(abs(x), abs(y)) >= (1 << 23):
        x, y = x >> 1, y >> 1
    z = 0
    if x < 0:
        x, y, z = -x, -y, 1 << 15
    for k in range(14):
        if y >= 0:
            x, y, z = x + (y >> k), y - (x >> k), z + ATAN[k]
        else:
            x, y, z = x - (y >> k), y + (x >> k), z - ATAN[k]
    return z & 0xFFFF


def lut():
    k = np.arange(1 << LUT_BITS)
    a = 2 * np.pi * k / (1 << LUT_BITS)
    return np.round(LUT_SCALE * np.cos(a)).astype(np.int64), np.round(LUT_SCALE * np.sin(a)).astype(np.int64)


def twiddle(n_size, e):
    """W_n^e = exp(-2 pi j e / n) in Q15."""
    a = -2 * np.pi * e / n_size
    return (np.round(np.cos(a) * ((1 << TW_BITS) - 1)).astype(np.int64),
            np.round(np.sin(a) * ((1 << TW_BITS) - 1)).astype(np.int64))


def fft_r22(re, im):
    """Radix-2^2 DIF on integer arrays (last axis), halving after each butterfly; returns bit-reversed order."""
    re, im = re.copy(), im.copy()
    n = re.shape[-1]
    size = n
    while size >= 4:
        q = size // 4
        shape = re.shape[:-1] + (n // size, 4, q)
        r = re.reshape(shape)
        i = im.reshape(shape)
        x0r, x1r, x2r, x3r = r[..., 0, :], r[..., 1, :], r[..., 2, :], r[..., 3, :]
        x0i, x1i, x2i, x3i = i[..., 0, :], i[..., 1, :], i[..., 2, :], i[..., 3, :]
        # BF2I: span size/2
        u0r, u0i = (x0r + x2r) >> 1, (x0i + x2i) >> 1
        u1r, u1i = (x0r - x2r) >> 1, (x0i - x2i) >> 1
        v0r, v0i = (x1r + x3r) >> 1, (x1i + x3i) >> 1
        v1r, v1i = (x1r - x3r) >> 1, (x1i - x3i) >> 1
        # BF2II: span size/4, with -j on the k1 = 1 branch
        h = np.empty(shape, np.int64), np.empty(shape, np.int64)
        outs = [((u0r + v0r) >> 1, (u0i + v0i) >> 1),     # k1=0, k2=0
                ((u1r + v1i) >> 1, (u1i - v1r) >> 1),     # k1=1, k2=0: u1 + (-j) v1
                ((u0r - v0r) >> 1, (u0i - v0i) >> 1),     # k1=0, k2=1
                ((u1r - v1i) >> 1, (u1i + v1r) >> 1)]     # k1=1, k2=1: u1 + j v1
        n3 = np.arange(q)
        # quarter 2 k1 + k2 of the output, as the pipeline emits it (bit-reversed order overall)
        for src, (k1, k2) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
            ar, ai = outs[src]
            slot = 2 * k1 + k2
            e = n3 * (k1 + 2 * k2)
            if size > 4:
                wr, wi = twiddle(size, e)
                br = (ar * wr - ai * wi + (1 << (TW_BITS - 1))) >> TW_BITS
                bi = (ar * wi + ai * wr + (1 << (TW_BITS - 1))) >> TW_BITS
            else:
                br, bi = ar, ai
            h[0][..., slot, :] = br
            h[1][..., slot, :] = bi
        re, im = h[0].reshape(re.shape), h[1].reshape(im.shape)
        size = q
    if size == 2:
        shape = re.shape[:-1] + (n // 2, 2, 1)
        r, i = re.reshape(shape), im.reshape(shape)
        re = np.stack([(r[..., 0, :] + r[..., 1, :]) >> 1, (r[..., 0, :] - r[..., 1, :]) >> 1], -2).reshape(re.shape)
        im = np.stack([(i[..., 0, :] + i[..., 1, :]) >> 1, (i[..., 0, :] - i[..., 1, :]) >> 1], -2).reshape(im.shape)
    return re, im


def bin_order(n=N):
    """Natural bin index at each output position, measured: a tone in bin k lands where its energy is."""
    t = np.arange(n)
    order = np.zeros(n, np.int64)
    re = np.round(4000 * np.cos(2 * np.pi * np.outer(np.arange(n), t) / n)).astype(np.int64)
    im = np.round(4000 * np.sin(2 * np.pi * np.outer(np.arange(n), t) / n)).astype(np.int64)
    fr, fi = fft_r22(re, im)
    order[np.argmax(fr ** 2 + fi ** 2, axis=1)] = np.arange(n)
    return order


def front(i, q, taps=TAPS, shift=OUT_SHIFT, trace=None, float_fft=False, float_resample=False, gain=None):
    xd_i, xd_q = dc(i), dc(q)
    h = coefficients(taps)
    if float_resample:
        from scipy.signal import resample_poly
        yr = np.round(resample_poly(xd_i.astype(float), UP, DOWN) * 16).astype(np.int64)
        yi = np.round(resample_poly(xd_q.astype(float), UP, DOWN) * 16).astype(np.int64)
    else:
        yr, yi = resample(xd_i, h), resample(xd_q, h)
    L = len(yr)
    y8r, y8i = sat(yr >> 4, 8), sat(yi >> 4, 8)
    pr = np.zeros(L, np.int64)
    pi = np.zeros(L, np.int64)
    a_r, a_i, b_r, b_i = y8r[:-N], y8i[:-N], y8r[N:], y8i[N:]
    pr[N:] = a_r * b_r + a_i * b_i
    pi[N:] = a_i * b_r - a_r * b_i
    cr, ci = np.concatenate([[0], np.cumsum(pr)]), np.concatenate([[0], np.cumsum(pi)])
    sr = np.zeros(L, np.int64)
    si = np.zeros(L, np.int64)
    n = np.arange(N + WINDOW - 1, L)
    sr[n] = cr[n + 1] - cr[n + 1 - WINDOW]
    si[n] = ci[n + 1] - ci[n + 1 - WINDOW]
    m = mag(sr, si)

    first = N + WINDOW - 1
    blen = BLOCK * SYM
    nblocks = (L - first) // blen
    peaks, steps = [], []
    for b in range(nblocks):
        nb = first + b * blen
        fold = m[nb: nb + blen].reshape(BLOCK, SYM).sum(0)
        j = int(np.argmax(fold))
        peaks.append((nb + blen, (nb + j + 1) % SYM, j))
        if b > 0:
            pj = peaks[b - 1][2]
            idx = nb + pj + SYM * np.arange(BLOCK)
            theta = cordic_angle(sr[idx].sum(), si[idx].sum())
            steps.append((nb + blen, theta))
    # NCO: step in 2^-32 turns per sample = theta (2^-16 turns over 1024 samples) << 6
    step = np.zeros(L, np.int64)
    for end, theta in steps:
        th = theta - (1 << 16) if theta >= (1 << 15) else theta
        step[end + UPDATE_DELAY:] = th << 6
    acc = np.concatenate([[0], np.cumsum(step)[:-1]]) & ((1 << NCO_BITS) - 1)
    idx = acc >> (NCO_BITS - LUT_BITS)
    cs, sn = lut()
    c, s = cs[idx], sn[idx]
    half = 1 << 10
    zr = (yr * c - yi * s + half) >> 11
    zi = (yr * s + yi * c + half) >> 11

    # Symbol schedule
    starts = []
    if peaks:
        k = 0
        phase = peaks[0][1]
        t = peaks[0][0] + UPDATE_DELAY
        t += (phase - t) % SYM
        while t + CP - FFT_BACK + N <= L:
            # the newest estimate known before this symbol starts
            while k + 1 < len(peaks) and peaks[k + 1][0] + UPDATE_DELAY <= t:
                k += 1
            new = peaks[k][1]
            d = (new - t) % SYM
            d = d - SYM if d >= SYM // 2 else d
            resync = False
            if abs(d) > SMALL_STEP:
                t += d % SYM
                resync = True
            elif d != 0:
                t += d
            if t + CP - FFT_BACK + N > L:
                break
            cfo_ok = any(end + UPDATE_DELAY <= t for end, _ in steps)
            starts.append((t, cfo_ok, resync))
            t += SYM
    w0 = np.array([s for s, _, _ in starts], np.int64)
    if len(w0) == 0:
        return np.zeros(0, np.uint16), {}
    win = w0[:, None] + CP - FFT_BACK + np.arange(N)[None, :]
    if float_fft:
        f = np.fft.fft((zr[win] << 2) + 1j * (zi[win] << 2), axis=1)[:, bin_order()] / N
        fr, fi = np.floor(f.real).astype(np.int64), np.floor(f.imag).astype(np.int64)
    else:
        fr, fi = fft_r22(zr[win] << 2, zi[win] << 2)
    # Output scaling: bin k of a symbol is f * G[k] * A >> (9 + OUT_SHIFT), G the per-bin gain table (Q4, 8 bits),
    # A the AGC (Q5, 8 bits). The AGC sums |v_re| + |v_im| over 64 output symbols, before saturation, and sets A for
    # the symbols from the 66th on so the mean comes to AGC_TARGET.
    g = (gain if gain is not None else np.full(N, 16, np.int64))[bin_order()]
    agc = AGC_START
    agcs = np.zeros(len(starts), np.int64)
    vr = np.zeros(fr.shape, np.int64)
    vi = np.zeros(fi.shape, np.int64)
    acc, pending = 0, None
    for k in range(len(starts)):
        if pending is not None and k == pending[0]:
            agc = pending[1]
            pending = None
        agcs[k] = agc
        p = g * agc
        ar = (fr[k] * p) >> (9 + shift)
        ai = (fi[k] * p) >> (9 + shift)
        acc += int(np.abs(ar).sum() + np.abs(ai).sum())
        vr[k], vi[k] = ar, ai
        if k % AGC_SYMBOLS == AGC_SYMBOLS - 1:
            new = (agc * AGC_TARGET * AGC_SYMBOLS * 2 * N) // max(acc, 1)
            pending = (k + 2, int(min(max(new, 1), 255)))
            acc = 0
    clip = float(np.mean((np.abs(vr) > 63) | (np.abs(vi) > 63)))
    rms = float(np.sqrt(np.mean(vr.astype(float) ** 2 + vi.astype(float) ** 2) / 2))
    vr, vi = np.clip(vr, -63, 63), np.clip(vi, -63, 63)
    words = []
    base = 0
    for k, (t, cfo_ok, resync) in enumerate(starts):
        if k == 0 or resync:
            base = t - SYM * k
        off = (t - SYM * k - base) & 0x3FFF
        words.append(word(MARK, MARK))
        hdr = (int(cfo_ok) << 13) | (int(resync) << 12) | (k & 0xFFF)
        words.append(hdr_word(hdr))
        words.append(hdr_word(off))
        words.append(hdr_word(int(agcs[k])))
        words.extend(word(vr[k], vi[k]).tolist())
    info = {"samples_27": L, "blocks": nblocks, "peaks": [p[1] for p in peaks], "thetas": [t for _, t in steps],
            "symbols": len(starts), "clip": clip, "bin_rms": rms, "agc": sorted(set(agcs.tolist()))[:12]}
    if trace is not None:
        trace.update(xd_i=xd_i, xd_q=xd_q, yr=yr, yi=yi, zr=zr, zi=zi, starts=starts, fr=fr, fi=fi)
    return np.array(words, np.uint16), info


def gain_table(i, q, taps=TAPS, max_gain=8.0):
    """A per-bin gain (Q4, 8-bit) that flattens the average spectrum of a capture: 16 / its level relative to the
    median of bins |k| < 200, at most max_gain."""
    tr = {}
    front(i, q, taps, 0, trace=tr)
    fr, fi = tr["fr"], tr["fi"]
    p = np.zeros(N)
    p[bin_order()] = np.mean(fr.astype(float) ** 2 + fi.astype(float) ** 2, 0)
    k = (np.arange(N) + N // 2) % N - N // 2
    ref = np.median(p[(np.abs(k) < 200) & (np.abs(k) > 12)])
    g = np.sqrt(ref / np.maximum(p, 1e-9))
    g = np.clip(g, 1.0, max_gain)
    return np.clip(np.round(16 * g), 16, 255).astype(np.int64)


GAIN_HEX = __file__.rsplit("/", 2)[0] + "/fpga/rtl/hdz_gain_c5narrow.hex"


def load_gain(path=GAIN_HEX):
    """A gain table as the RTL's $readmemh reads it: 1,024 lines of 2 hex digits, natural bin order (bin 0 first)."""
    return np.array([int(l, 16) for l in open(path).read().split()], np.int64)


def word(vr, vi):
    """Lane values to the u16 word the P4 captures: each 7-bit value v becomes the byte 2v+1."""
    vr, vi = np.asarray(vr), np.asarray(vi)
    return ((((2 * vr + 1) & 0xFF) | (((2 * vi + 1) & 0xFF) << 8))).astype(np.uint16)


def hdr_word(h):
    """14 header bits as two 7-bit lane values, I = bits 0-6, Q = bits 7-13."""
    lo, hi = h & 0x7F, (h >> 7) & 0x7F
    lo = lo - 128 if lo >= 64 else lo
    hi = hi - 128 if hi >= 64 else hi
    return int(word(lo, hi))


def main(args):
    prefix = args[0].removesuffix(".bin")
    out = args[1]
    opt = dict(zip(args[2::2], args[3::2]))
    i, q = lanes(prefix, int(opt.get("--samples", 1_400_000)))
    gain = None
    global AGC_TARGET, FFT_BACK
    AGC_TARGET = int(opt.get("--agc-target", AGC_TARGET))
    FFT_BACK = int(opt.get("--fft-back", FFT_BACK))
    if "--gain" in opt:
        src = opt["--gain"]
        if src == "default":
            gain = load_gain()
        elif src == "self":
            gain = gain_table(i, q, int(opt.get("--taps", TAPS)), float(opt.get("--max-gain", 8)))
        else:
            gain = load_gain(src)
        if "--save-gain" in opt:
            open(opt["--save-gain"], "w").write("".join(f"{int(v):02x}\n" for v in gain))
    words, info = front(i, q, int(opt.get("--taps", TAPS)), int(opt.get("--shift", OUT_SHIFT)),
                        float_fft=opt.get("--float-fft") == "1", float_resample=opt.get("--float-resample") == "1",
                        gain=gain)
    words.tofile(out)
    json.dump(info, open(out + ".json", "w"))
    print(json.dumps({k: v for k, v in info.items() if k not in ("peaks", "thetas")}), info["peaks"][:6], info["thetas"][:6])


if __name__ == "__main__":
    main(sys.argv[1:])
