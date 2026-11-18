# HDZero burst decoder

`hdzrx/` is a C port of hdzero-captures' Python receiver (`hdzrx/phy.py`, `hdzrx/codec.py`) for 720p60 from the C5's 7-bit, 40 MS/s I/Q. It is plain C, so the same files build for the P4 (an ESP-IDF component) and for the Mac (`hdzrx/host/main.c`), where `tools/hdzcompare.py` checks each stage against the Python.

## Stages

Each is a function in `hdzrx/phy.c` or `hdzrx/codec.c` with the Python function's name, and each dumps its output by name (`y`, `coherence`, `fN.H`, `fN.trim`, `fN.E`, `fN.slots`, `fN.Y`, `fN.Cb`, `fN.Cr`).

1. Load and resample: raw / 2 less its 2,048-sample mean, then scipy's `resample_poly(x, 27, 40)` filter (801 taps, Kaiser 5.0) as a polyphase loop. One pass; the DC sums are integers.
2. CP sync: CP start mod 1,056 and CFO within half a bin, from a folded CP correlation.
3. Whole-bin CFO: one set of FFTs scores all seven candidates, since a whole-bin shift only moves the spectrum and turns each symbol by the same step.
4. Training runs: coherence of consecutive symbols on the active bins, runs of 10 or more over 0.6.
5. Per frame: channel from the training run (the sign walk from the continual pilots), equalise, scattered-pilot offset, per-bin trim, the 0/90/180/270 choice, specials, noisy bins, descrambled slot bits.
6. Codec: luma and chroma blocks, 8x8 IDCT, concealment from the record above, and the display stage (`hz_row_rgb`).

Later FPGA stages (timing, CFO, resampler, FFT) replace stages 1-4 behind the same per-symbol spectra.

## Memory

On the P4 a 1.4M-sample burst (35 ms, at least one whole frame and the next training run) needs the 27 MS/s samples (945k × 8 bytes, 7.6 MB), one frame's equalised symbols (409 × 1,024 × 8, 3.4 MB), the planes as int16 (2.8 MB) and the slots (340 KB).

## Results

Mac build against hdzrx on the four Nov 4 captures, first 1.4M samples after the unsettled ones (2026-11-18):

| Capture | Frames | Stages that differ | Concealed records | Mac time |
| --- | --- | --- | --- | --- |
| 191206 | 1 | none | 1,271 | 60 ms |
| 191225 | 2 | none; 1 of 335,063 slots differs in frame 0 | 916, 1,148 | 83 ms |
| 191237 | 1 | none; no special symbols found, as in hdzrx | 416 | 63 ms |
| 191250 | 1 | none (channel within 0.3%) | 990 | 63 ms |

- The resampler matches scipy to 5e-8 relative error, and start, CFO, runs, offsets, rotation, specials, noisy bins and concealment match exactly.
- The trim differs only on the 24 continual pilots, which are BPSK, so their 4th-power phase sits on a 90-degree tie. They carry no data, and the compare leaves them out.
- Planes match within 1 (the C rounds them to int16).
