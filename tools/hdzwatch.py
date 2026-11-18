"""Capture HDZero I/Q whenever the P4 detects it: watch the console for `event hdzero=1`, then `iq grab` and `iq dump`
into captures/hdz/, in hdzero-captures' format (PREFIX.bin + PREFIX.json), so its decode.py reads them as they are.

    uv run --with pyserial python tools/hdzwatch.py [--samples 4000000] [--every 15] [--per-detection 4] [--now]
        [--decode] [--out captures/hdz] [--filter ab|narrow|wide]

--now grabs once straight away and exits, detected or not. --fake SECONDS tests the plumbing without a transmitter: the
P4 reports HDZero (`hdz fake 1`) for that long, then follows its detector again, and the watcher stops after. --decode runs tools/hdzdecode.py on each new capture in
the background. --filter picks the C5's receive filter: narrow (802.11p, analog's), wide (twice the width, for
HDZero's band edges, during the grab only), or ab (the default) to alternate. Stop with Ctrl-C."""
import argparse
import base64
import json
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from p4cmd import open_console  # noqa: E402

B64 = re.compile(rb"^[A-Za-z0-9+/=]+$")


class Console:
    def __init__(self):
        self.s = open_console()
        self.s.timeout = 0.2
        self.events = []

    def lines(self, until, stop=None):
        """Lines until `until` (a time) or a line matching `stop`; event lines are kept aside wherever they come."""
        buf, out = b"", []
        while time.time() < until:
            buf += self.s.read(65536)
            *done, buf = buf.split(b"\n")
            for line in done:
                line = line.strip(b"\r")
                if line.startswith(b"event "):
                    self.events.append(line.decode(errors="replace"))
                    continue
                out.append(line)
                if stop and re.search(stop, line):
                    return out
        return out

    def cmd(self, c, wait=3.0):
        self.s.write((c + "\r").encode())
        return [l.decode(errors="replace") for l in self.lines(time.time() + wait, rb"^> ?$")]


def keyvals(line):
    return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)


def grab(con, samples, out_dir, why, wide=False):
    t0 = time.time()
    reply = [l for l in con.cmd(f"iq grab {samples}" + (" wide" if wide else ""), 15) if l.startswith(("ok grab", "err"))]
    if not reply or not reply[0].startswith("ok grab"):
        print(f"grab failed: {reply}", flush=True)
        return None
    info = keyvals(reply[0])
    con.s.write(b"iq dump\r")
    body = con.lines(time.time() + 30 + samples / 50_000, rb"iq dump end")
    data = b"".join(l for l in body if B64.match(l) and not l.startswith(b"iq"))
    raw = base64.b64decode(data)
    con.lines(time.time() + 0.5, rb"^> ?$")
    if len(raw) != 2 * int(info["samples"]):
        print(f"dump short: {len(raw)} bytes of {2 * int(info['samples'])}", flush=True)
        return None
    stamp = time.strftime("%Y%m%d-%H%M%S")
    prefix = out_dir / f"iq-{info.get('ch', 'ch')}-{stamp}"
    prefix.with_suffix(".bin").write_bytes(raw)
    meta = {
        "channel": info.get("ch"),
        "tuned_mhz": int(info.get("mhz", 0)),
        "sample_rate_hz": float(info.get("rate", 40e6)),
        "samples": int(info["samples"]),
        "unsettled_leading_samples": 0,
        "gain": int(info.get("gain", 0)),
        "format": "u16le words: I = int8(word & 0xff), Q = int8(word >> 8), each 2v+1",
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "source": "p4vrx iq grab (Nano raw 40 MS/s)",
        "hdzero": int(info.get("hdzero", 0)),
        "score": float(info.get("score", 0)),
        "why": why,
        "filter": info.get("filter", "narrow"),
        "grab": reply[0],
    }
    prefix.with_suffix(".json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"{prefix}.bin: {meta['samples']} samples, {meta['channel']} gain {meta['gain']}, {meta['filter']} filter, score {meta['score']:.2f}, "
          f"{time.time() - t0:.0f} s", flush=True)
    return prefix


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=4_000_000)
    ap.add_argument("--every", type=float, default=15, help="seconds between grabs while detected")
    ap.add_argument("--per-detection", type=int, default=4)
    ap.add_argument("--now", action="store_true")
    ap.add_argument("--decode", action="store_true")
    ap.add_argument("--out", default="captures/hdz")
    ap.add_argument("--fake", type=float, default=0)
    ap.add_argument("--filter", choices=["ab", "narrow", "wide"], default="ab",
                    help="the C5's receive filter for each grab; ab alternates, so each detection gives both")
    a = ap.parse_args()
    out_dir = pathlib.Path(a.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    con = Console()

    n_grabs = [0]

    def wide():
        n_grabs[0] += 1
        return a.filter == "wide" or (a.filter == "ab" and n_grabs[0] % 2 == 0)

    def captured(prefix):
        if prefix and a.decode:
            subprocess.Popen([sys.executable, str(pathlib.Path(__file__).parent / "hdzdecode.py"), str(prefix)])

    if a.now:
        captured(grab(con, a.samples, out_dir, "now", wide()))
        return
    fake_until = time.time() + a.fake if a.fake else None
    if fake_until:
        con.cmd("hdz fake 1")
    state = keyvals(next((l for l in con.cmd("hdz") if l.startswith("hdzero=")), "hdzero=0"))
    on = state.get("hdzero") == "1"
    taken, last = 0, 0.0
    print(f"watching: hdzero={int(on)} score={state.get('score')} ch={state.get('ch')}", flush=True)
    while True:
        if fake_until and time.time() > fake_until:
            con.cmd("hdz fake off")
            con.lines(time.time() + 1)
            print("\n".join(con.events), flush=True)
            return
        # Events arrive unasked; a poll every couple of seconds covers one missed while the console was busy.
        con.lines(time.time() + 2)
        for e in con.events:
            print(e, flush=True)
            on = keyvals(e).get("hdzero") == "1"
            if on:
                taken = 0
        con.events.clear()
        poll = next((l for l in con.cmd("hdz", 2) if l.startswith("hdzero=")), None)
        if poll:
            was, on = on, keyvals(poll).get("hdzero") == "1"
            if on and not was:
                taken = 0
        if on and taken < a.per_detection and time.time() - last >= a.every:
            prefix = grab(con, a.samples, out_dir, "event", wide())
            last = time.time()
            if prefix:
                taken += 1
                captured(prefix)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
