"""Check the C receiver (hdzrx/) stage by stage against hdzero-captures' hdzrx, and name the first stage that diverges.

    uv run --with-requirements ../hdzero-captures/requirements.txt python tools/hdzcompare.py PREFIX [--c-dir DIR]
        [--window 1400000] [--frames 2]

Without --c-dir it builds and runs the Mac build (build/hdzrx_host) into captures/hdzc/<name>/. With --c-dir it reads
dumps already there, e.g. from the P4 (`tools/hdzrun.py`). Both receivers see the same samples: the first --window
after the capture's unsettled ones."""
import argparse
import json
import os
import pathlib
import subprocess
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent.parent
HDZ = pathlib.Path(os.environ.get("HDZERO_CAPTURES", HERE.parent / "hdzero-captures"))
sys.path.insert(0, str(HDZ))
from hdzrx import codec, phy  # noqa: E402
from scipy.ndimage import uniform_filter1d  # noqa: E402

SRC = ["hdzrx/host/main.c", "hdzrx/phy.c", "hdzrx/codec.c", "hdzrx/fft.c"]


def build():
    exe = HERE / "build/hdzrx_host"
    srcs = [HERE / s for s in SRC + ["hdzrx/hdzrx.h"]]
    if not exe.exists() or max(s.stat().st_mtime for s in srcs) > exe.stat().st_mtime:
        exe.parent.mkdir(exist_ok=True)
        subprocess.run(["cc", "-O2", "-Wall", "-fcx-limited-range", "-o", str(exe)] + [str(HERE / s) for s in SRC] + ["-lm"],
                       check=True)
    return exe


def load_window(prefix, window):
    """hdzrx's load_capture on the window the C receiver takes."""
    meta = json.load(open(prefix + ".json"))
    w = np.fromfile(prefix + ".bin", dtype="<u2")
    if len(w) > 1_400_000:
        w = w[meta["unsettled_leading_samples"]:]
    w = w[:window]
    i = (w & 0xFF).astype(np.uint8).view(np.int8).astype(np.float32)
    q = (w >> 8).astype(np.uint8).view(np.int8).astype(np.float32)
    x = (i + 1j * q) / 2.0
    x = x - (uniform_filter1d(x.real, 2048) + 1j * uniform_filter1d(x.imag, 2048))
    return x.astype(np.complex64), meta


class Report:
    def __init__(self):
        self.first = None

    def row(self, stage, ok, detail):
        print(f"{'ok ' if ok else 'BAD'} {stage:<14} {detail}", flush=True)
        if not ok and self.first is None:
            self.first = stage


def rel(a, b):
    return float(np.linalg.norm(a - b) / max(np.linalg.norm(b), 1e-30))


def cload(d, name, dtype):
    p = d / f"{name}.bin"
    return np.fromfile(p, dtype=dtype) if p.exists() else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--c-dir")
    ap.add_argument("--window", type=int, default=1_400_000)
    ap.add_argument("--frames", type=int, default=2)
    a = ap.parse_args()
    prefix = str(pathlib.Path(a.prefix.removesuffix(".bin")).resolve())
    name = os.path.basename(prefix)
    if a.c_dir:
        cdir = pathlib.Path(a.c_dir)
    else:
        cdir = HERE / "captures/hdzc" / name
        cdir.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(build()), prefix, str(cdir), "--window", str(a.window), "--frames", str(a.frames), "--force"],
                       check=True)
    info = json.load(open(cdir / "info.json"))
    frames_c = json.load(open(cdir / "frames.json")) if (cdir / "frames.json").exists() else []
    rep = Report()

    x, meta = load_window(prefix, a.window)
    y = phy.resample(x, meta["sample_rate_hz"])
    yc = cload(cdir, "y", np.complex64)
    if yc is not None:
        n = min(len(y), len(yc))
        rep.row("resample", len(y) == len(yc) and rel(yc[:n], y[:n]) < 1e-4,
                f"{len(yc)} vs {len(y)} samples, relative error {rel(yc[:n], y[:n]):.2e}")
    start, cfo, ratio = phy.cp_sync(y)
    rep.row("cp_sync", start == info["start"], f"start {info['start']} vs {start}, CP peak/mean {info['cp_ratio']:.2f} vs {ratio:.2f}")
    cfo, kbin = phy.whole_bin_cfo(y, start, cfo)
    rep.row("cfo", kbin == info["whole_bins"] and abs(cfo - info["cfo_hz"]) < 50,
            f"{info['cfo_hz']:.0f} Hz ({info['whole_bins']:+d} bins) vs {cfo:.0f} Hz ({kbin:+d})")
    S = phy.fft_symbols(y, start, cfo)
    runs = phy.training_runs(S)
    rep.row("runs", [list(r) for r in runs] == info["runs"], f"{info['runs']} vs {[list(r) for r in runs]}")

    act = phy.ACTIVE + 512
    # data bins only: the continual pilots are BPSK, so their 4th-power trim sits on a 90-degree tie and is never used
    data = np.array([b + 512 for b in phy.ACTIVE if b not in set(phy.CONT.tolist()) and not -11 <= b <= 3])
    for fi, ((ra, rb), nxt) in enumerate(zip(runs, runs[1:] + [None])):
        if fi >= a.frames or rb + phy.N_DATA > len(S):
            break
        H = phy.channel_from_training(S[ra + 1:rb - 1])
        Hc = cload(cdir, f"f{fi}.H", np.complex64)
        if Hc is not None:
            rep.row(f"f{fi} channel", rel(Hc[act], H[act]) < 1e-2, f"relative error {rel(Hc[act], H[act]):.2e} on active bins")
        E = np.array([phy.equalize(S[t], H) for t in range(rb, rb + phy.N_DATA)])
        o0 = phy.scattered_offset_d0(E, phy.N_DATA)
        offsets = [(o0 - 8 * j) % 32 for j in range(phy.N_DATA)]
        Teq = np.array([phy.equalize(S[t], H) for t in range(ra + 1, rb - 1)])
        trim = phy.per_bin_trim(E, offsets, Teq)
        trim = trim / np.sqrt(np.mean(np.abs((E * trim)[:, 312:712]) ** 2))
        E = E * trim
        fc = frames_c[fi] if fi < len(frames_c) else {}
        rep.row(f"f{fi} offset", fc.get("o0") == o0, f"o0 {fc.get('o0')} vs {o0}")
        tc = cload(cdir, f"f{fi}.trim", np.complex64)
        if tc is not None:
            rep.row(f"f{fi} trim", rel(tc[data], trim[0, data]) < 1e-3, f"relative error {rel(tc[data], trim[0, data]):.2e} on data bins")
        Ec = cload(cdir, f"f{fi}.E", np.complex64)
        if Ec is not None:
            Ec = Ec.reshape(phy.N_DATA, phy.N)
            rep.row(f"f{fi} equalised", rel(Ec[:, data], E[:, data]) < 1e-3, f"relative error {rel(Ec[:, data], E[:, data]):.2e} on data bins")
        x_sym = x_off = None
        if nxt is not None:
            xi = nxt[0] - 2
            x_sym = phy.equalize(S[xi], H) * trim[0]
            x_off = (o0 - 8 * (xi - rb)) % 32
        fr = phy.Frame(E, offsets, x_sym, x_off)
        Tz = (Teq * trim)[:, phy.ACTIVE + 512].reshape(-1)
        q0 = 0 if np.mean(np.abs(Tz.real)) > np.mean(np.abs(Tz.imag)) else 1
        best = None
        for q in (q0, q0 + 2):
            fr.rot = 1j ** q
            bits = np.concatenate([phy.slot_bits(fr.points(E[j], offsets[j])) for j in range(5, 60)])
            zero = 1 - bits[:, :2].mean()
            if best is None or zero > best[0]:
                best = (zero, q)
        fr.rot = 1j ** best[1]
        rep.row(f"f{fi} rotation", fc.get("rot") == best[1] % 4, f"{fc.get('rot')} vs {best[1] % 4}")
        stream, noisy, specials = fr.stream()
        rep.row(f"f{fi} specials", fc.get("specials") == specials, f"{fc.get('specials')} vs {specials}")
        sc = cload(cdir, f"f{fi}.slots", np.uint8)
        if sc is not None:
            if len(sc) != len(stream):
                rep.row(f"f{fi} slots", False, f"{len(sc)} vs {len(stream)} slots")
            else:
                bits_c = np.stack([(sc >> k) & 1 for k in range(4)], 1)
                diff = np.flatnonzero((bits_c != stream).any(1))
                nz = np.flatnonzero(((sc >> 4) & 1).astype(bool) != noisy)
                rep.row(f"f{fi} slots", len(diff) <= 1e-4 * len(sc),
                        f"{len(diff)} of {len(sc)} slots differ" + (f", first at {diff[0]}" if len(diff) else ""))
                rep.row(f"f{fi} noisy bins", len(nz) == 0, f"{len(nz)} slots flagged differently")
        Y, Cb, Cr, missing = codec.decode_picture(stream, noisy)
        rep.row(f"f{fi} concealed", fc.get("concealed") == int(missing.sum()), f"{fc.get('concealed')} vs {int(missing.sum())}")
        for pn, ref, shape in (("Y", Y, (720, 960)), ("Cb", Cb, (720, 480)), ("Cr", Cr, (720, 480))):
            pc = cload(cdir, f"f{fi}.{pn}", np.int16)
            if pc is None:
                continue
            d = np.abs(pc.reshape(shape) - np.round(ref))
            bad = d > 1
            rep.row(f"f{fi} {pn}", bad.mean() < 1e-3, f"max difference {d.max():.0f}, {bad.sum()} pixels off by more than 1")
    print(f"first divergence: {rep.first or 'none'}")
    print(f"C timing, ms: {info.get('ms')}")
    return 1 if rep.first else 0


if __name__ == "__main__":
    sys.exit(main())
