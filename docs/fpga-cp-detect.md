# HDZero detector in the Nano (phase 4, first step)

An always-on detector beside the FM demod that says whether the channel carries HDZero, from its OFDM cyclic prefix, so the receiver can switch modes by itself. Status: running in the Nano (`fpga/rtl/cp_detect.v` in `fm_top`, build 3, in its flash), bit for bit with `nano_fold` in `models/cp_detect.py` (`models/cp_detect_check.py`: 2-4 decisions each on two HDZero captures and an analog one). Wide-band lag only. On the P4: `hdz`, an `event` line on each change, the HDZERO badge, and `iq grab` for raw 40 MS/s I/Q.

## Using it

- `hdz` prints one line: `hdzero=0|1 score=… peak_bin=… decisions=… ch=…`. `info` includes it.
- When the state changes the console prints `event hdzero=1 score=3.84 ch=R8` (or `hdzero=0`), unasked, to whichever console last sent a command.
- The P4 reads the detector every 100 ms. Three reads in a row over 2.2 turn detection on (0.3 s), two under 1.6 turn it off.
- False triggers, 2026-11-17: 2.5 minutes each on live analog R4 (248 reads, median 1.22, max 1.34) and dead R2 at gain 77 (246 reads, median 1.20, max 2.98, two over 2.2, not in a row): no events. The isolated highs on R2 fit a burst of other traffic filling a few bins of one window.
- While detected, the strip shows the HDZERO badge and a LINK bar from the score (2.2-5 as 40-100%). There is no HDZero decode yet; the picture stays whatever the analog decoder makes of it.
- `iq grab [samples]` (default 1.4M, up to 8M) pauses the decoder's receive, has the Nano send its 40 MS/s lane samples with PCLK at 40 MHz (control bit 4), captures them, and restarts the receive with no retune or placement: 29 ms for 1M samples. Its reply carries `samples rate ch mhz gain hdzero score paused_ms`; `iq dump` then sends the samples, base64, as `tools/iqcap.py` reads them. A grab of analog R4 had its line every 2,542 samples, as at 40 MS/s.

## What it looks for

HDZero's symbols are a 1024-point FFT with a 32-sample prefix at 27 MS/s. At the C5's 40 MS/s the prefix repeats the symbol's last 47 samples 1517 samples later, once a symbol, every 1564.39 samples (39.11 µs). The narrowband mode's lag is about 2409.

- Correlate each sample with the one 1517 earlier, summed over 48: c[n] = Σ x[n−k]·x*[n−k−1517].
- Fold |c| by the symbol period into 1565 bins, over 64 symbols (2.5 ms), and compare the fold's peak with its mean. The prefix lines up in one bin; anything else spreads.
- Folding is what does the work. Flat stretches of analog FM (sync tips, blanking) look like a steady tone, which correlates with itself at any lag: |c| normalised reaches 0.999 on analog, and a per-symbol peak-over-mean without folding scored one analog capture 20-28, above HDZero's 9-15. Those peaks don't recur every 1564.39 samples, so the fold flattens them.

## Model results

`models/cp_detect.py` on the four 2026-11-04 HDZero captures (4M samples each from `hdzero-captures`, past the 65,536 unsettled ones), three 40 MS/s analog captures of R4, and dead R2 at gain 77 (2026-11-17):

| Capture | Float fold, 64 symbols | Fixed fold, 64 symbols | Fixed fold, 16 symbols |
| --- | --- | --- | --- |
| HDZero 191206 | 3.19-4.06 | 3.11-4.71 | 2.71-5.76 |
| HDZero 191225 | 3.30-4.02 | | |
| HDZero 191237 | 3.71-4.15 | 3.60-4.48 | 3.29-5.31 |
| HDZero 191250 | 3.40-4.04 | | |
| Analog R4 020228 | 1.16-1.28 | 1.15-1.30 | 1.12-1.91 |
| Analog R4 020646 | 1.22-1.38 | | |
| Analog R4 232445 | 1.32-1.34 | 1.31-1.34 | 1.15-2.12 |
| Analog R4 172254 | | 1.13 | 1.32-1.58 |
| Dead R2 | 1.17-1.25 | 1.12-1.36 | 1.19-1.71 |

With 64 symbols and a threshold of 2.2, every HDZero decision is above 3.1 and every other one below 1.4. Sixteen symbols leave 2.71 against 2.12, too close.

191206 is the capture that doesn't decode (deep fades); it still detects.

## Fixed point, as the Nano would run it

- The 7-bit lanes, less a 2048-sample running mean (the C5's offset moves with its gain).
- Products of 7-bit values, summed over 48: 21 bits a component. |c| as max + min/2 (within 12%), no division by the energy: the ratio cancels it.
- The bin from a 32-bit phase counter that steps 2^32 / 1564.39 a sample, times 1565, top bits. Drift between the transmitter's clock and the C5's at 50 ppm moves the prefix 5 samples over a 2.5 ms decision, inside its 47.
- After each decision, one pass over the bins for the peak and the sum, and the result as peak × 1565 / sum.

## Resources, GW2AR-18

| Block | Use |
| --- | --- |
| 1517-sample delay of 16-bit I/Q | 2 BSRAM |
| 48-sample moving sum of products | a 48-deep chain of 2 × 17 bits in flops (as BSRAM its address missed hold by 0.12 ns), multipliers |
| Two 1565-bin folds of 32 bits, ping-pong | 6 BSRAM |
| Phase counter, peak and sum pass, registers | a few hundred LUTs |

As built, `fm_top` with the detector: 10 of 46 BSRAM, 22% of flops, the 40 MHz domain closing at 71 MHz.

It runs on the FM demod's 40 MHz clock and the same lane samples, so it adds no PLL. The narrowband lag (2409) needs either a second delay (2 more BSRAM) or the two lags taking turns per decision.

## On the bus and the P4

- As built, `fm_top` registers 0x14-0x1f: the fold's peak and total (32 bits each), the peak's bin and the decision count (16 bits each), low bytes first; reading 0x14 holds them all for the reads that follow. The P4 does the ratio and the hysteresis.
- Later, on detection: the C5's gain to HDZero's fixed ~58, and the HDZero decoder.

## Verification

1. This model on more captures: the other analog transmitter, other channels, and HDZero at other gains and modes when there are captures of them.
2. RTL bit for bit with `fixed_fold` in iverilog, on the same files.
3. Hardware: the detector's ratio over I2C on analog R4 and dead channels (no HDZero transmitter here yet).

## Open

- No narrowband captures yet, so its lag is from the spec, not checked.
- A threshold for weak HDZero: the four captures are all of one transmitter on R8, at gains 58 and 70.
