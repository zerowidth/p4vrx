"""Run the HDZero burst decoder on the P4 and check it against hdzrx: upload a capture (or grab live), `hdz run` with
dumps, save them as tools/hdzcompare.py reads them, then compare.

    uv run --with pyserial --with-requirements ../hdzero-captures/requirements.txt python tools/hdzrun.py PREFIX
        [--frames 1] [--full] [--no-compare]
    ... tools/hdzrun.py --grab [--samples 1400000]        (live: grab on the P4, then fetch the I/Q too)

Output goes to captures/hdzc/<name>-p4/ (frames as PNG there too). --full also dumps the 27 MS/s samples and the
equalised symbols (11 MB, about a minute)."""
import argparse
import base64
import json
import os
import pathlib
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from hdzwatch import Console, grab as watch_grab  # noqa: E402

HERE = pathlib.Path(__file__).resolve().parent.parent
WINDOW = 1_400_000


def upload(con, prefix):
    meta = json.load(open(prefix + ".json"))
    w = np.fromfile(prefix + ".bin", dtype="<u2")
    if len(w) > 1_400_000:
        w = w[meta.get("unsettled_leading_samples", 0):]
    w = w[:WINDOW]
    con.s.write(f"hdz upload {len(w)}\r".encode())
    ready = con.lines(time.time() + 3, rb"^(ok upload ready|err)")
    if not ready or not ready[-1].startswith(b"ok upload ready"):
        sys.exit(f"upload refused: {ready}")
    t0 = time.time()
    data = w.astype("<u2").tobytes()
    for off in range(0, len(data), 16384):
        con.s.write(data[off:off + 16384])
    done = con.lines(time.time() + 30, rb"^(ok upload \d|err)")
    print(f"{done[-1].decode(errors='replace') if done else 'no reply'} ({len(data) / (time.time() - t0) / 1e3:.0f} KB/s)", flush=True)
    con.lines(time.time() + 0.3, rb"^> ?$")


def run(con, out, frames, dumps):
    con.s.write(f"hdz run frames={frames} dump={','.join(dumps)}\r".encode())
    end = time.time() + 120
    buf, name, size, body, frames_json, info = b"", None, 0, [], [], None
    while time.time() < end:
        buf += con.s.read(65536)
        *done, buf = buf.split(b"\n")
        for line in done:
            line = line.strip(b"\r")
            if name:
                if line == b"dump end":
                    raw = base64.b64decode(b"".join(body))
                    (out / f"{name}.bin").write_bytes(raw)
                    print(f"  dump {name}: {len(raw)} of {size} bytes", flush=True)
                    name = None
                else:
                    body.append(line)
            elif line.startswith(b"dump "):
                _, name, size = line.decode().split()
                name = name.strip()
                size, body = int(size), []
            elif line.startswith(b"hdz frame "):
                frames_json.append(json.loads(line[10:]))
                print(line.decode(), flush=True)
            elif line.startswith(b"hdz info "):
                info = json.loads(line[9:])
            elif line.startswith((b"ok hdz run", b"err")):
                print(line.decode(), flush=True)
                (out / "frames.json").write_text(json.dumps(frames_json) + "\n")
                if info:
                    (out / "info.json").write_text(json.dumps(info) + "\n")
                    print(f"P4 timing, ms: {info['ms']}, PSRAM free before {info['psram_free'] / 1e6:.1f} MB", flush=True)
                con.lines(time.time() + 0.3, rb"^> ?$")
                return info
    sys.exit("hdz run: no reply in 120 s")


def save_png(out, fi):
    """The P4's planes through hdzrx's display stage."""
    hdz = pathlib.Path(os.environ.get("HDZERO_CAPTURES", HERE.parent / "hdzero-captures"))
    sys.path.insert(0, str(hdz))
    from hdzrx import codec
    from PIL import Image
    planes = [np.fromfile(out / f"f{fi}.{p}.bin", np.int16).astype(float) for p in ("Y", "Cb", "Cr")]
    if any(len(p) == 0 for p in planes):
        return
    Y, Cb, Cr = planes[0].reshape(720, 960), planes[1].reshape(720, 480), planes[2].reshape(720, 480)
    Image.fromarray(codec.to_rgb(Y, Cb, Cr)).save(out / f"f{fi}.png")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="?")
    ap.add_argument("--grab", action="store_true")
    ap.add_argument("--samples", type=int, default=WINDOW)
    ap.add_argument("--frames", type=int, default=1)
    ap.add_argument("--full", action="store_true")
    ap.add_argument("--no-compare", action="store_true")
    a = ap.parse_args()
    con = Console()
    if a.grab:
        prefix = watch_grab(con, a.samples, HERE / "captures/hdz", "hdzrun")
        if not prefix:
            sys.exit(1)
        prefix = str(prefix)
    else:
        prefix = str(pathlib.Path(a.prefix.removesuffix(".bin")).resolve())
        upload(con, prefix)
    out = HERE / "captures/hdzc" / (os.path.basename(prefix) + "-p4")
    out.mkdir(parents=True, exist_ok=True)
    dumps = [f"f{i}.{n}" for i in range(a.frames) for n in ("H", "trim", "slots", "Y", "Cb", "Cr")]
    if a.full:
        dumps += ["y"] + [f"f{i}.E" for i in range(a.frames)]
    info = run(con, out, a.frames, dumps)
    for fi in range(info.get("frames", 0) if info else 0):
        save_png(out, fi)
    print(f"-> {out}/", flush=True)
    if not a.no_compare and info:
        subprocess.run([sys.executable, str(HERE / "tools/hdzcompare.py"), prefix, "--c-dir", str(out), "--frames", str(a.frames)])


if __name__ == "__main__":
    main()
