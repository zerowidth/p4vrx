"""Decode HDZero captures on the Mac with hdzero-captures' receiver (hdzrx), and print one line each: CP strength,
carrier offset, frames, special symbols found, records concealed. Frames go to PREFIX-frames/ as PNGs.

    uv run python tools/hdzdecode.py captures/hdz/iq-R8-20261118-101500 [...]
    uv run python tools/hdzdecode.py captures/hdz          (every capture in the folder not decoded yet)

HDZERO_CAPTURES points at the hdzero-captures checkout (default ../hdzero-captures beside this repo)."""
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
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


def main(args):
    for arg in args or ["captures/hdz"]:
        p = pathlib.Path(arg)
        if p.is_dir():
            for b in sorted(p.glob("*.bin")):
                pre = b.with_suffix("")
                if not pathlib.Path(f"{pre}-frames/decode.log").exists():
                    decode(pre)
        else:
            decode(pathlib.Path(str(p).removesuffix(".bin")))


if __name__ == "__main__":
    main(sys.argv[1:])
