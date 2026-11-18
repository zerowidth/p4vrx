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

## Modes and reference behaviour

- Modes from hdzero-reference's `modes.py`: 720p60, 540p90, 1080p30 and 960x720p60, all on the same PHY, with their own data-symbol count, regular specials, block sizes and picture size. Each frame's mode is detected as the reference does: by the distance to the next training run (within 3 symbols), with the run length telling 720p60 (12-14) from 960x720p60 (52), or by the run length alone. `mode=` (P4) or `--mode` (Mac) forces one. 540p60n runs at 17 MS/s and needs another front end, so it isn't here.
- Only 720p60 has been decoded from real signals; the other modes follow the reference's tables untested. Forced onto the Nov 4 captures, each runs clean under AddressSanitizer.
- 1080p30 needs about 2.6M samples at 40 MS/s for one frame and the next run, more than `hdz decode`'s 1.4M default, and its buffers don't fit the P4 beside a long capture.
- The gap rule (SPEC 7.2) is on: in a frame with more specials than its mode's regular count, each extra one leaves a symbol's worth of slots out before X, and the records they would have filled are concealed. `gap=0` / `--no-gap` turns it off, as hdzrx has it. The Nov 4 captures have no such frame.
- `runs=ref` / `--ref-runs` uses the reference's training-run rule (over 0.7 with gaps of 2 bridged, end trimmed to 0.85 then extended while over 0.5, at least 8) instead of hdzrx's (over 0.6, 10 in a row). On 100 ms of each Nov 4 capture both find the same runs; the reference's should hold through a dropout.
- The P4 differs from the Mac build on weak bins: on 191250, 3,344 of 335,062 slots (1%) and 1,004 concealed records against 990. The per-bin trim's fine phase comes from the 4th power of the data, which is near zero on a few band-edge bins (-418..-395, +293, +316), so rounding and libm differences move it by up to 16 degrees. 191225 matches exactly. A decision-directed refinement of each bin's gain and phase was tried and concealed more records on all four captures (the noisy-bin limit tightens as the good bins improve), so it isn't in.

## On the P4

- `hdz upload N` takes N raw I/Q words over the USB console into the capture buffer (4.5 MB/s), `hdz run [frames=N] [dump=...]` decodes the capture buffer, and `hdz decode [samples]` grabs from the Nano first. Results come back as `hdz frame {json}` and `hdz info {json}` lines, dumps as `dump NAME BYTES`, base64, `dump end`.
- `tools/hdzrun.py PREFIX` uploads a capture's first 1.4M samples, runs it, saves the dumps and PNGs to `captures/hdzc/<name>-p4/` and runs the compare on them; `--grab` does it live.
- The 27 MS/s samples go in the capture buffer after the burst, so the decoder's own PSRAM is about 7 MB (11.1 MB was free).
- Its 150 KB of tables and scratch are allocated for each burst and freed after it: as static arrays in internal RAM they left the analog decoder no room for its DMA ring (`decode: no memory for the ring`). The P4 tries internal RAM first, but with the ring allocated the largest free block is 48 KB, so they land in PSRAM, which costs nothing measurable.
- The first frame goes to the LCD (`lcd_still`), 16:9 letterboxed in the 4:3 picture, held until `hdz show off`. The PPA reads U on even rows of the packed 4:2:0.
- `hdz decode` keeps the analog decoder paused through the decode. One frame of 191225 took 4.9 s with analog decoding stopped and 10.2 s alongside it (the analog decoder then logged 11 overruns, against 1).

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

On the P4 (2026-11-18, analog decoder running alongside), 191225 uploaded: both frames match hdzrx at every stage, as on the Mac. 14.0 s for two frames:

| Stage | ms |
| --- | --- |
| resample | 1,235 |
| CP sync | 529 |
| whole-bin CFO (398 FFTs) | 1,107 |
| training runs (893 FFTs) | 2,483 |
| channel, equalise, trim (2 frames) | 4,936 |
| specials, noisy bins, slots (2 frames) | 854 |
| codec (2 frames) | 1,019 |

With exp(jx) from a 4,096-entry table (within 3e-7), the window correction precomputed and the pilot interpolation's segments worked out once, the same run took 8.6 s and still matched at every stage: resample 1,226, CP sync 546, whole-bin CFO 280, training runs 642, equalise 2,220, slots 847, codec 1,021 ms. The rest of the 8.6 s is the dumps going out over USB during the run.
