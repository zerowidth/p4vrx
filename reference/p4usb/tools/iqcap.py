#!/usr/bin/env python3
"""Capture raw I/Q from the P4's console and save it on this machine.

    iqcap.py [-c CHANNEL] [-e EVERY] [-n SAMPLES] [-k CAPTURES] [-o PREFIX] [--port DEV]

Each capture is one burst of up to 8M samples (200 ms at 40 MS/s) into the P4's PSRAM, which is then
downloaded. The DMA outruns USB, and the PSRAM buffer limits the burst.

Files, for PREFIX (default iq-<time>):

    PREFIX.<k>.bin   little-endian 16-bit words, I in the low byte and Q in the high byte, both signed
                     and holding 2v+1 (the C5 sends 7 bits; lines 0 and 8 are constant 1). Read as
                     interleaved int8 I,Q (GNU Radio's complex int8, `-t cs8`) it is also an SDR file.
    PREFIX.<k>.json  channel, MHz, sample rate, gain, clock edge and the console's own capture stats

The sample rate is 80 MS/s / EVERY: 2 (40 MS/s, the default) keeps the full video bandwidth, 6 is the
decoder's 13.33 MS/s. The first 64 samples of a capture (the first 65536 of a burst over 1.4M) are not settled; the .json says so.
Without -c the receiver stays on the channel it is on. The decoder stops while capturing and, if it was
running, starts again on the same channel afterwards (--no-restore leaves it off).

Needs only the standard library.
"""

import argparse
import fcntl
import glob
import json
import os
import re
import select
import struct
import subprocess
import sys
import termios
import time
import tty

BUS_HZ = 80_000_000
SKIP = 64
# Bursts over 1.4M samples lose up to 128 samples somewhere in their first 55K, per `iq counter` on the C5's counter.
SKIP_LONG = 65536
MAX_SAMPLES = 8_000_000


def find_port(explicit):
    if explicit:
        return explicit
    # macOS names the CDC port after the USB serial number, which ioreg shows for VID 303a, PID 8000.
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True).stdout
    for block in out.split("+-o ")[1:]:
        if '"idVendor" = 12346' in block and '"idProduct" = 32768' in block:
            m = re.search(r'"USB Serial Number" = "([^"]+)"', block)
            ports = glob.glob(f"/dev/cu.usbmodem{m.group(1)}*") if m else []
            if ports:
                return ports[0]
    sys.exit("no P4 console (303a:8000) found; pass --port")


class Console:
    """A raw tty with just the calls the capture needs, so pyserial isn't required."""

    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        # DTR on so the P4 writes to us; RTS off, since RTS with DTR low looks like an esptool reset.
        fcntl.ioctl(self.fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_RTS))
        fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))

    def read(self, n, timeout=0.2):
        if not select.select([self.fd], [], [], timeout)[0]:
            return b""
        try:
            return os.read(self.fd, n)
        except BlockingIOError:
            return b""

    def write(self, data):
        while data:
            select.select([], [self.fd], [])
            try:
                data = data[os.write(self.fd, data) :]
            except BlockingIOError:
                pass

    def reset_input_buffer(self):
        termios.tcflush(self.fd, termios.TCIFLUSH)


def open_console(path):
    return Console(path)


def run(s, cmd, timeout):
    """Sends one console line and returns everything up to the next prompt."""
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    out = b""
    end = time.time() + timeout
    while not out.endswith(b"\n> ") and time.time() < end:
        out += s.read(1 << 16)
    if not out.endswith(b"\n> "):
        sys.exit(f"'{cmd}': no prompt after {timeout}s, got {out[-200:]!r}")
    return out.decode(errors="replace")


def download(s, timeout=60):
    s.reset_input_buffer()
    s.write(b"iq dump\n")
    buf = b""
    end = time.time() + timeout
    m = None
    while time.time() < end:
        buf += s.read(1 << 16)
        m = re.search(rb"-----BEGIN IQ (\d+)-----\n", buf)
        if m:
            break
    if not m:
        sys.exit(f"no capture header: {buf[-200:]!r}")
    n = int(m.group(1))
    data = bytearray(buf[m.end() :])
    t0 = time.time()
    while len(data) < n and time.time() < end:
        data += s.read(min(1 << 20, n - len(data)) or 1)
    if len(data) < n:
        sys.exit(f"capture cut short: {len(data)} of {n} bytes (the P4 drops a stalled console write)")
    # Eat the trailer and prompt so the next command starts clean.
    tail = b""
    while b"-----END IQ-----" not in tail and time.time() < end:
        tail += s.read(4096)
    return bytes(data[:n]), n / max(time.time() - t0, 1e-3)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-c", "--channel", help="retune first (R3, A2, 5800 ...); default: stay on the current channel")
    ap.add_argument("-e", "--every", type=int, default=2, help="keep one of every N 80 MS/s samples (default 2)")
    ap.add_argument("-n", "--samples", type=int, default=MAX_SAMPLES, help=f"samples per capture, at most {MAX_SAMPLES}")
    ap.add_argument("-k", "--captures", type=int, default=1, help="number of back-to-back captures (default 1)")
    ap.add_argument("-o", "--output", help="file prefix (default iq-<time>)")
    ap.add_argument("-d", "--dir", default=".", help="directory for the files (default .)")
    ap.add_argument("--no-restore", action="store_true", help="leave the decoder off afterwards")
    ap.add_argument("--port")
    a = ap.parse_args()
    if not 0 < a.samples <= MAX_SAMPLES:
        sys.exit(f"--samples must be 1..{MAX_SAMPLES}")
    os.makedirs(a.dir, exist_ok=True)
    prefix = os.path.join(a.dir, a.output or time.strftime("iq-%Y%m%d-%H%M%S"))

    s = open_console(find_port(a.port))
    run(s, "", 3)
    was_decoding = "running on" in run(s, "decode", 5)
    channel = a.channel
    if not channel:
        m = re.search(r"channel (\S+) (\d+)", run(s, "channel", 3))
        if not m:
            sys.exit("could not read the current channel; pass -c")
        channel = m.group(2)
    try:
        start = run(s, f"iq start {channel} {a.every}", 120)
        print(start.strip())
        if "iq:" in start and "using" not in start:
            sys.exit("iq start failed")
        gains = re.findall(r"gain (\d+)", start)
        mhz = re.search(r"ok tune(?:\s+\S+)?\s+(\d+)", start)
        rate = BUS_HZ / a.every
        for k in range(a.captures):
            stats = run(s, f"iq {a.samples}", 10).strip().splitlines()
            stats = next((l for l in stats if "samples:" in l or "iq:" in l), "")
            if "iq:" in stats:
                sys.exit(stats)
            raw, speed = download(s)
            base = f"{prefix}.{k}"
            open(base + ".bin", "wb").write(raw)
            meta = {
                "channel": channel,
                "tuned_mhz": int(mhz.group(1)) if mhz else None,
                "sample_rate_hz": rate,
                "samples": len(raw) // 2,
                "unsettled_leading_samples": SKIP_LONG if a.samples > 1_400_000 else SKIP,
                "gain": int(gains[-1]) if gains else None,
                "format": "u16le words: I = int8(word & 0xff), Q = int8(word >> 8), each 2v+1",
                "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                "console_stats": stats,
                "start_log": start.strip().splitlines(),
            }
            json.dump(meta, open(base + ".json", "w"), indent=2)
            print(f"{base}.bin: {len(raw) // 2} samples = {len(raw) / 2 / rate * 1e3:.1f} ms at {rate / 1e6:.2f} MS/s "
                  f"({speed / 1e3:.0f} kB/s download)")
            print(stats)

    finally:
        # decode on runs its own probe capture, so it waits until the last one is downloaded.
        if was_decoding and not a.no_restore:
            run(s, f"decode on {channel}", 30)

if __name__ == "__main__":
    main()
