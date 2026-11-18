"""Decode HDZero captures on the Mac, with hdzero-captures' receiver (hdzrx) and with our C port of it (hdzrx/, every
mode in hdzero-reference, the whole capture), and print one line each: CP strength, carrier offset, frames, special
symbols found, records concealed. Frames go to PREFIX-frames/ (hdzrx) and PREFIX-c/ (C) as PNGs.

    uv run python tools/hdzdecode.py captures/hdz/iq-R8-20261118-101500 [...]
    uv run python tools/hdzdecode.py captures/hdz          (every capture in the folder not decoded yet)
    ... tools/hdzdecode.py --c|--both ...                   (the C port only, or both)

HDZERO_CAPTURES points at the hdzero-captures checkout (default ../hdzero-captures beside this repo)."""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(pathlib.Path(__file__).parent))
HDZ = pathlib.Path(os.environ.get("HDZERO_CAPTURES", HERE.parent / "hdzero-captures"))


def decode(prefix):
    prefix = pathlib.Path(prefix).resolve()
    out = pathlib.Path(f"{prefix}-frames")
    out.mkdir(exist_ok=True)
    uv = shutil.which("uv") or "uv"
    r = subprocess.run([uv, "run", "-q", "--with-requirements", str(HDZ / "requirements.txt"), "python", str(HDZ / "decode.py"),
                        str(prefix), "--out", str(out)], capture_output=True, text=True, cwd=HDZ)
    log = r.stdout + r.stderr
    (out / "decode.log").write_text(log)
    head = re.search(r"CP peak/mean ([0-9.]+), CFO (-?[0-9]+) Hz", log)
    frames = re.findall(r"frame \d+: specials at \[([^\]]*)\], (\d+) slots .*?, (\d+) records concealed", log)
    if not head:
        print(f"{prefix}: no sync ({log.strip().splitlines()[-1] if log.strip() else 'no output'})", flush=True)
        return
    with_specials = sum(1 for s, _, _ in frames if s.strip())
    try:
        filt = json.load(open(f"{prefix}.json")).get("filter", "narrow")
    except (OSError, ValueError):
        filt = "?"

    concealed = sorted(int(c) for _, _, c in frames)
    print(f"{prefix}: {filt} filter, CP {float(head.group(1)):.1f}x, CFO {int(head.group(2)) / 1e3:.1f} kHz, {len(frames)} frames, "
          f"{with_specials} with special symbols, concealed median {concealed[len(concealed) // 2] if concealed else '-'}"
          f" -> {out}/", flush=True)


def build():
    """build/hdzrx_host, rebuilt when a source is newer."""
    src = ["hdzrx/host/main.c", "hdzrx/phy.c", "hdzrx/codec.c", "hdzrx/fft.c"]
    exe = HERE / "build/hdzrx_host"
    newest = max((HERE / f).stat().st_mtime for f in src + ["hdzrx/hdzrx.h"])
    if not exe.exists() or newest > exe.stat().st_mtime:
        exe.parent.mkdir(exist_ok=True)
        subprocess.run(["cc", "-O2", "-fcx-limited-range", "-o", str(exe)] + [str(HERE / f) for f in src] + ["-lm"], check=True)
    return exe


def decode_c(prefix):
    """The C port over the whole capture, every mode; PNGs from its PPMs."""
    prefix = pathlib.Path(prefix).resolve()
    out = pathlib.Path(f"{prefix}-c")
    out.mkdir(exist_ok=True)
    n = pathlib.Path(f"{prefix}.bin").stat().st_size // 2
    r = subprocess.run([str(build()), str(prefix), str(out), "--window", str(n), "--dump", "none"], capture_output=True, text=True)
    (out / "decode.log").write_text(r.stdout + r.stderr)
    try:
        info = json.loads((out / "info.json").read_text())
        frames = json.loads((out / "frames.json").read_text())
    except (OSError, ValueError):
        print(f"{prefix} (C): failed: {(r.stdout + r.stderr).strip().splitlines()[-1:]}", flush=True)
        return
    for ppm in out.glob("f*.ppm"):
        # sips ships with macOS, so this needs nothing from the Python environment
        if subprocess.run(["sips", "-s", "format", "png", str(ppm), "--out", str(ppm.with_suffix(".png"))],
                          capture_output=True).returncode == 0:
            ppm.unlink()
    modes = sorted({f["mode"] for f in frames})
    conc = sorted(f["concealed"] for f in frames)
    print(f"{prefix} (C): CP {info['cp_ratio']:.1f}x, CFO {info['cfo_hz'] / 1e3:.1f} kHz, {len(frames)} frames "
          f"({', '.join(modes) or 'none'}), {sum(1 for f in frames if f['specials'])} with special symbols, "
          f"concealed median {conc[len(conc) // 2] if conc else '-'} -> {out}/", flush=True)


def main(args):
    use_c, use_py = "--c" in args or "--both" in args, "--c" not in args
    args = [a for a in args if a not in ("--c", "--both")]
    done = "decode.log"

    def run(pre):
        if use_py:
            decode(pre)
        if use_c:
            decode_c(pre)

    for arg in args or ["captures/hdz"]:
        p = pathlib.Path(arg)
        if p.is_dir():
            for b in sorted(p.glob("*.bin")):
                pre = b.with_suffix("")
                seen = (not use_py or pathlib.Path(f"{pre}-frames/{done}").exists()) and \
                    (not use_c or pathlib.Path(f"{pre}-c/{done}").exists())
                if not seen:
                    run(pre)
        else:
            run(pathlib.Path(str(p).removesuffix(".bin")))

if __name__ == "__main__":
    main(sys.argv[1:])
