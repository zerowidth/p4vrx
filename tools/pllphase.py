"""Sweep the Nano's resampling phase (passthru_pll_top) while the P4 decodes, score each phase by the decoder's
clicks per field and sync-tip noise, and leave the best set: pllphase.py [seconds per phase]."""
import pathlib
import re
import sys
import time

import serial

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from boards import port  # noqa: E402
from p4cmd import open_console  # noqa: E402


def main():
    dwell = float(sys.argv[1]) if len(sys.argv) > 1 else 3
    nano = serial.Serial(port(0x0403, 0x6010, "1"), 115200, timeout=0.5)
    p4 = open_console()

    def decode():
        p4.write(b"decode\r")
        out, end = b"", time.time() + 5
        while time.time() < end and not out.rstrip().endswith(b">"):
            out += p4.read(65536)
        return out.decode(errors="replace")

    def lock():
        nano.reset_input_buffer()
        nano.write(b"r")
        return nano.readline().decode(errors="replace").strip()

    results = []
    for k in range(16):
        nano.write(b"0123456789abcdef"[k:k + 1])
        time.sleep(1)
        decode()
        t0 = time.time()
        time.sleep(dwell)
        t = decode()
        g = lambda p: (re.search(p, t) or [None, None])[1]  # noqa: E731
        clicks = int(g(r"(\d+) lines with clicks") or 0) / ((time.time() - t0) * 59.94)
        tip = int(g(r"sync tip noise (\d+)") or 9999)
        hs = int(g(r"last field (\d+)/262") or 0)
        results.append((k, clicks, tip, hs))
        print(f"phase {k:2}: clicks/field {clicks:6.1f}, sync tip noise {tip:5} kHz, hsyncs {hs}, {lock()}", flush=True)
    best = min(results, key=lambda r: (r[3] < 200, r[2]))
    nano.write(b"0123456789abcdef"[best[0]:best[0] + 1])
    print(f"set phase {best[0]}: {lock()}")


if __name__ == "__main__":
    main()
