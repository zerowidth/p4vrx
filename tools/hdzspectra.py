"""Grab the Nano's HDZero spectra stream (fm_top build 5, docs/hdzero-fpga.md) and check its structure, or check a
file of it: marks and headers, symbol counts in sequence, flags, timing offsets, AGC, bin levels. With --decode it
also runs the C receiver on it.

    uv run --with pyserial --with numpy python tools/hdzspectra.py [--words N] [--out captures/hdz] [--decode]
    uv run --with numpy python tools/hdzspectra.py --file FILE.u16
"""
import argparse
import base64
import pathlib
import subprocess
import sys
import time
import zlib

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
HERE = pathlib.Path(__file__).resolve().parent.parent
MARK = 0x8181
IDLE = 0x0181


def lanes(w):
    i = ((w & 0xFF).astype(np.uint8).view(np.int8).astype(int) - 1) >> 1
    q = ((w >> 8).astype(np.uint8).view(np.int8).astype(int) - 1) >> 1
    return i, q


def hdr(w):
    i, q = lanes(np.array([w], np.uint16))
    return int(i[0] & 0x3F) | int(q[0] & 0x3F) << 6


def check(w):
    w = np.asarray(w, np.uint16)
    idle = int(np.sum(w == IDLE))
    # idle words fall between bins wherever the bus outruns the FFT; none is a bin or header word
    w = w[w != IDLE]
    marks = np.flatnonzero(w == MARK)
    syms = []
    for m in marks:
        if m + 5 + 1024 > len(w):
            break
        count, flags, off, agc = (hdr(w[m + k]) for k in range(1, 5))
        bins = w[m + 5:m + 5 + 1024]
        bi, bq = lanes(bins)
        syms.append((count, flags & 1, flags >> 1 & 1, off, agc, float(np.sqrt(np.mean(bi ** 2 + bq ** 2) / 2)),
                     int(np.sum((np.abs(bi) == 63) | (np.abs(bq) == 63))), int(np.sum(bins == MARK))))
    marks = np.flatnonzero(w == MARK)
    spacing = np.diff(marks)
    print(f"symbols 1,029 words long: {int(np.sum(spacing == 1029))} of {len(spacing)}")
    print(f"{len(w)} words, {idle} idle ({100 * idle / max(len(w), 1):.0f}%), {len(marks)} marks, {len(syms)} whole symbols")
    if not syms:
        return syms
    counts = [s[0] for s in syms]
    gaps = sum(1 for a, b in zip(counts, counts[1:]) if b != (a + 1) & 0xFFF)
    print(f"counts {counts[0]}..{counts[-1]}, {gaps} breaks in sequence; cfo_valid {sum(s[1] for s in syms)}, "
          f"resyncs {sum(s[2] for s in syms)}; offsets {sorted(set(s[3] for s in syms))[:8]}; "
          f"agc {sorted(set(s[4] for s in syms))[:8]}")
    rms = [s[5] for s in syms]
    print(f"bin rms {min(rms):.1f}..{max(rms):.1f} (median {np.median(rms):.1f}); bins at +-63 {sum(s[6] for s in syms)}; "
          f"marks inside bins {sum(s[7] for s in syms)} (should be 0)")
    return syms


def grab(words, edge=""):
    from hdzwatch import Console, B64, keyvals
    con = Console()
    reply = [l for l in con.cmd(f"iq grab {words} spectra hold {edge}", 15) if l.startswith(("ok grab", "err"))]
    print(reply[0] if reply else "no reply", flush=True)
    if not reply or not reply[0].startswith("ok grab"):
        con.cmd("iq resume", 2)
        sys.exit(1)
    info = keyvals(reply[0])
    con.s.write(b"iq dump\r")
    body = con.lines(time.time() + 30 + words / 50_000, rb"iq dump end")
    raw = base64.b64decode(b"".join(l for l in body if B64.match(l) and not l.startswith(b"iq")))
    con.lines(time.time() + 0.5, rb"^> ?$")
    con.cmd("iq resume", 2)
    if f"{zlib.crc32(raw):08x}" != info.get("crc"):
        print("dump CRC differs from the grab's", flush=True)
    return np.frombuffer(raw, np.uint16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", type=int, default=1_400_000)
    ap.add_argument("--out", default="captures/hdz")
    ap.add_argument("--file")
    ap.add_argument("--decode", action="store_true")
    ap.add_argument("--edge", default="", help="pos or neg: the P4's sampling edge for this and later grabs")
    a = ap.parse_args()
    if a.file:
        path = pathlib.Path(a.file)
        w = np.fromfile(path, np.uint16)
    else:
        w = grab(a.words, a.edge)
        path = HERE / a.out / time.strftime("spectra-%Y%m%d-%H%M%S.u16")
        path.parent.mkdir(parents=True, exist_ok=True)
        w.tofile(path)
        print(f"-> {path}")
    check(w)
    if a.decode:
        from hdzdecode import build
        out = pathlib.Path(str(path).removesuffix(".u16") + "-c")
        out.mkdir(exist_ok=True)
        # hdzrx_host wants a capture prefix for its metadata; the words carry everything else
        stub = out / "stub"
        stub.with_suffix(".json").write_text('{"sample_rate_hz": 40000000.0, "unsettled_leading_samples": 0}\n')
        stub.with_suffix(".bin").write_bytes(b"\0" * 4)
        r = subprocess.run([str(build()), str(stub), str(out), "--words", str(path), "--dump", "none"],
                           capture_output=True, text=True)
        print(r.stdout.strip())


if __name__ == "__main__":
    main()
