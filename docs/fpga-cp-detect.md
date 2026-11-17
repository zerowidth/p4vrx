# HDZero detector in the Nano (phase 4, first step)

An always-on detector beside the FM demod that says whether the channel carries HDZero, from its OFDM cyclic prefix, so the receiver can switch modes by itself. Status: design and model (`models/cp_detect.py`); no RTL.

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
| 1517-sample delay of 14-bit I/Q | 21 kbit: 2 BSRAM |
| 48-sample moving sum of products | 3-4 multipliers (9×9), a 48-deep delay of 2 × 15 bits in registers or 1 BSRAM |
| 1565-bin fold, 24 bits each | 38 kbit: 3 BSRAM, read-modify-write a sample at 40 MHz |
| Phase counter, peak and sum pass, registers | a few hundred LUTs |

It runs on the FM demod's 40 MHz clock and the same lane samples, so it adds no PLL. The narrowband lag (2409) needs either a second delay (2 more BSRAM) or the two lags taking turns per decision.

## On the bus and the P4

- I2C registers: the last ratio (8.8 fixed point), a detected flag with hysteresis (two decisions over 2.2 to set, two under 1.6 to clear), the bin of the peak (the symbol timing, useful to the PHY later), and the lag in use.
- The P4 reads them every 100 ms or so. On detection it switches the C5's gain to HDZero's fixed ~58 and the UI to HDZero; the analog decoder keeps running until the switch.

## Verification

1. This model on more captures: the other analog transmitter, other channels, and HDZero at other gains and modes when there are captures of them.
2. RTL bit for bit with `fixed_fold` in iverilog, on the same files.
3. Hardware: the detector's ratio over I2C on analog R4 and dead channels (no HDZero transmitter here yet).

## Open

- No narrowband captures yet, so its lag is from the spec, not checked.
- A threshold for weak HDZero: the four captures are all of one transmitter on R8, at gains 58 and 70.
