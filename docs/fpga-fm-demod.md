# FM demod in the Nano (phase 3a design)

The Nano takes the C5's I/Q at 40 MS/s, demodulates it, and sends the P4 frequency samples at 13.33 MS/s on the same 14-lane bus. The P4 keeps its decoder from the composite onward and drops its phase lookup, difference and despeck. Pass-through stays selectable, so the current path keeps working and the no-FPGA build is unchanged.

Status: fm is the P4's default whenever the Nano reports build 2 or later (`fm_top`); with `lockout_regs_top` (build 1), the image in the Nano's flash, it falls back to raw lanes. `decode source raw|fm` overrides it from the next start. `fm_top` runs from SRAM until it is written to flash. Against raw on R4 at a fixed gain of 57, three alternations of 10 s each: sync-tip noise 72-80 kHz against 110-135, and 0-2 clicked lines against 15-124. After PCLK went to 50% duty, 67-70 kHz against 135-138. The decoder's core 1 load is unchanged (54.6% against 55.1%).

Soak, 10 minutes in fm mode: lock throughout (253-254 hsyncs a field, line 847.387-847.392 samples), no PLL lock lost, core 1 at 53.5-55.4%, 4 ring overruns in all. Tip noise was 74-81 kHz until the signal weakened 4 minutes in (gain 54-57 rose to 61-65), then 101-186 kHz with up to 4,212 clicked lines in 30 s. Raw and fm alternated straight after, at that weaker signal: raw 159-188 kHz, fm 120-144.

- RTL (`fpga/rtl/fm_demod.v`) matches the model bit for bit in iverilog on three captures (`models/fm_demod_check.py`). `fm_top`: 14% of LUT4s, 7% of flops, 11 multipliers, the 40 MHz domain closing at 76 MHz.
- PLL, 2026-11-17: the lockout's clean clock ×3 held lock at 40,000 edges/ms for 60 s decoding R4, and 30 s on dead R2 at gain 77 with about 450 crosstalk edges/ms refused. The 2026-11-16 PLL bitstream, fed the raw C5 clock, lost lock.
- The PLL's dynamic phase made no difference: 16 steps all gave 167-183 kHz in the first, overrun-bound run. Either the eye is wide at 40 MS/s or the phase setting isn't taking; untested further.

## Why

- The P4 reads one sample in six of the C5's bus at 13.33 MS/s and turns each into an 8-bit phase. A step of that phase is 52 kHz, about 1.7 IRE, so luma comes in steps.
- A phase difference at 13.33 MS/s wraps at ±6.67 MHz. Strong deviation and noise wrap, which `despeck` patches up afterwards at about 9% of core 1.
- At 40 MS/s the difference can't wrap below ±20 MHz, and three samples can be filtered into each output rather than one kept.

## Input

- The C5's MODEM_DIAG bus changes at 40 MS/s with the 802.11p setting on, as c5rx runs it (80 MS/s without). Each of the 14 lanes carries one of the top 7 bits of I or Q, two's complement.
- The C5's clock stays at 13.33 MHz (`iq on 6`) and the Nano's lockout cleans it, as today. A second Nano PLL multiplies the clean clock by 3 to 40 MHz, locked to the C5's bus. The PLL bitstream of 2026-11-16 lost lock on the raw C5 clock because of its crosstalk edges; the lockout's output had +17 to 40 ppm of error and no extra edges, so it should hold a PLL. This is the first thing to prove on hardware.
- Where the 40 MHz edge lands against the bus is set with the PLL's dynamic phase (16 steps of 1.56 ns). The P4 chooses it over I2C as it places the C5's clock now, by the Nano's count of phase jumps (below) or by the decoder's tip noise.
- If the PLL won't hold, the fallback is a faster lockout that passes every edge of a 40 MHz C5 clock (25 ns apart), which needs a lockout clock well over 135 MHz.

## Demod

All at 40 MHz in the PLL's domain.

1. Offset: subtract the I and Q offsets, tracked by the Nano as the mean over 2^16 samples, or set by the P4 over I2C. I's offset grows with gain to about 30 of ±127.
2. Phase: a vectoring CORDIC, 10 iterations, 2 guard bits, gives a 12-bit phase (a turn is 4096). It is pipelined, one sample a clock. The magnitude comes out with it, for squelch and gain.
3. Frequency: the phase less the last one, modulo 4096: a 40 MS/s phase step, 9.77 kHz an LSB, ±20 MHz before it wraps.
4. Filter and decimate: a 15-tap symmetric low-pass, cut at 5.5 MHz, with integer taps summing to 1024. Only every third output is computed, so 5 multiplies a clock (8 if not shared).
5. Output: a 14-bit signed word, 2.44 kHz an LSB, ±20 MHz, at 13.33 MS/s.

Taps: −1 −6 −15 −11 36 133 236 280 236 133 36 −11 −15 −6 −1.

## On the bus

As built, the Nano sends a phase rather than a frequency: each output adds 3 × the 14-bit frequency to a 14-bit accumulator, whose top 8 bits are the decoder's 8-bit phase (1/256 turn at 13.33 MS/s, 52.08 kHz a step; the frequency's LSB is exactly 3/64 of that) and whose low 6 are its fraction. In fm mode the P4 maps PARLIO so D13..D6 land in each word's high byte, and its existing SIMD `bs_demod` and `despeck` take that byte as they take the BitScrambler's. A first version that converted 14-bit frequencies to steps in C took core 1 to 96.7% with 10,889 ring overruns. Summed and differenced, each step is within one unit and drift-free.

PCLK in fm mode is clk40 / 3, high for 37.5 ns from a falling-edge flop. With a 25 ns low pulse the decoder's line read 847.53-847.74 samples, against 847.39 raw; at 50% duty it reads 847.39.

The design as first written:


- The same 14 lanes and PCLK. In fm mode the Nano makes PCLK as the 40 MHz clock divided by 3, high for two of its periods, and changes the word as PCLK falls: 25 ns before the P4 samples on the rise and 50 ns after. That holds whatever the PLL's phase, which only places the lane sampling. The rate stays 13.33 MS/s, locked to the C5.
- Control register 0x08 bit 2 chooses the demodulated words over the raw lanes. Pattern mode (bit 0) still wins, so the self-check is unchanged.
- New registers: the offsets in use, the mean magnitude per ms (the P4's gain loop reads I/Q RMS from the raw lanes today), the PLL's phase step and lock, and phase jumps per ms.

## The P4's side

- `decode source raw|fm`, raw by default. In fm mode the decoder sets the Nano's bit 2, skips the BitScrambler's phase lookup, takes each 16-bit PARLIO word as a signed 14-bit frequency, and scales it to the units its slicer and colour stages use now. `despeck` is skipped.
- Gain: in fm mode the gain loop reads the Nano's magnitude register rather than the raw lanes' RMS.
- Clock placement: the C5's clock is placed as now (the lockout needs a clean clock), and then the PLL's phase is placed.
- The self-check gains a demod check: the Nano's pattern mode with a known sequence through the demod. Without it the pattern mode bypasses the demod entirely.

## Resources, GW2AR-18

| Block | LUT4 | FF | DSP (18x18) | BSRAM | PLL |
| --- | --- | --- | --- | --- | --- |
| Today: lockout, registers, I2C | about 300 | about 250 | 0 | 0 | 1 |
| Offsets and CORDIC (10 stages of 12-14 bits) | about 450 | about 400 | 0 | 0 | |
| Difference, FIR, decimate | about 150 | about 250 | 5-8 | 0 | |
| 40 MHz PLL | | | | | 1 |
| Total, of 20,736 LUT4 / 15,552 FF / 48 DSP / 46 BSRAM / 2 PLL | under 5% | under 7% | under 17% | 0 | 2 of 2 |

Both PLLs are used; phase 3b's SDRAM and line memory need none. The 40 MHz domain's paths are short (CORDIC stages of one add each), well inside the 167-338 MHz nextpnr reports for today's design.

## Verification

1. Model: `models/fm_demod.py` runs the Nano's fixed-point pipeline and today's P4 path (8-bit phase at 13.33 MS/s with despeck's rule) on 40 MS/s captures. It compares both with a float reference: unwrapped phase at 40 MS/s, differenced, a 63-tap low-pass, one sample in three.
2. RTL in iverilog, fed the same capture words, must match the model bit for bit (`fpga/sim`).
3. Hardware: load into SRAM first. Prove the 40 MHz PLL holds lock on the lockout's clock (an I2C lock bit and its edges per ms), then A/B `decode source raw` against `fm` on the same transmitter: tip noise, click lines, colour, and core 1's load. Flash only after that.

## Model results

On four 40 MS/s captures of R4 (2026-11-15 and -16). Error is rms against the float reference, specks are samples more than 1.5 MHz from it, and tip noise is the decoder's measure over the lowest 4% of samples:

| Capture | Radius | Path | Error | Specks | Tip noise | Distinct values |
| --- | --- | --- | --- | --- | --- | --- |
| 020228 (524k) | 36 | Nano | 75.4 kHz | 0 | 158 kHz | 3510 |
| | | P4 | 183.6 kHz | 889 | 266 kHz | 205 |
| 020646 (524k) | 35 | Nano | 73.6 kHz | 0 | 160 kHz | 3460 |
| | | P4 | 182.0 kHz | 886 | 308 kHz | 214 |
| 232445 (262k) | 52 | Nano | 33.5 kHz | 0 | 136 kHz | 3154 |
| | | P4 | 43.6 kHz | 0 | 150 kHz | 164 |
| 172254 (131k) | 57 | Nano | 34.4 kHz | 0 | 163 kHz | 2766 |
| | | P4 | 45.4 kHz | 0 | 184 kHz | 163 |

- On the weaker captures the P4's path wraps about 890 times in 175,000 samples, even with despeck's rule; the Nano's never does.
- The Nano's fixed-point loss, against the same 15-tap filter in float, is 7.9 and 4.6 kHz rms on the weak and strong captures: under 0.3 IRE. Eight iterations or a 10-bit phase cost 7-10 kHz; 12 iterations gain nothing. A 14-bit phase with this output scale clips the sync tip.
- Some of the P4's error is noise above 5.5 MHz that the Nano filters out and the P4 keeps; its decoder filters some of it later.

## Open

- Gain, done: in fm mode the gain loop takes the radius from the Nano's mean magnitude (0x12-0x13, ×4.86) and clipping from a radius over 110. On R4 it went from 40 to 57 and from 75 to 56 within 3 s; raw chose 56.
- Channel scan, done: D5..D0 carry each sample's radius code (radius / 4.86 to 31) and a clip flag from 121, mapped into the low byte as the BitScrambler's info byte, so `sweep_take` reads them with a table of radius². Band R: live R4 57.4 dB (s.d. 0.3), empty channels 29-34 dB; raw gave 54.3 (s.d. 6.3) and 23-32.
- A capture of the FM words showed 158 outliers in 262,144 samples; of those one flipped bit explains, 61 of 64 are bit 9, so D9 may be marginal at this timing.

- The output scale against the decoder's units: 2.44 kHz is about 1/21 of today's 52 kHz step.
- Whether 80 MS/s (802.11p off, a wider filter) is worth it later; it needs an 80 MHz PLL output and the same PLL phase search.
