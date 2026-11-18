# HDZero PHY in the Nano

Move the HDZero receiver's PHY from the P4 (`hdzrx/`, the C port of hdzrx) into the Nano, model first, each block bit for bit with its model in simulation. Stage A puts the front end in the Nano and sends per-symbol spectra to the P4; stage B adds equalise, demap and descramble so slots go out instead.

Status, 2026-11-18: stage A modelled (`models/hdz_front.py`) and in RTL (`fpga/rtl/hdz_front.v`, `hdz_fft.v`, `hdz_r22.v`), bit for bit with the model in iverilog (`models/hdz_front_check.py`, `models/hdz_fft_check.py`); `fm_top` build 5 puts it on the bus (control bit 6). The C receiver decodes its stream on the Mac and the P4 (`hz_receive_symbols`; `hdz decode src=nano`). Stage B prototyped in C.

## Stage A: front end

All on the 40 MHz clock, one 40 MS/s I/Q sample a clock from the raw lanes; 27 MS/s parts run on a clock enable.

1. DC: each sample less the mean of the 2,048 around it, as hdzrx's local mean; a 1,024-sample delay makes it causal. 9-bit out.
2. Resample 40 to 27 MS/s: a polyphase FIR of 8 taps and 27 phases (output m from inputs around 40m/27, phase (40m) mod 27), 9-bit coefficients, each phase summing to 256. Kaiser-windowed sinc, cutoff 12.5 MHz. 16 MULT9.
3. CP timing: y[n-1024] conj(y[n]) on 8-bit samples, summed over 32, magnitude as max + min/2, folded over 1,056 positions for 64 symbols (2.5 ms). The peak sets the symbol start. Corrections of 16 samples or less move the next symbol's window; larger ones resync.
4. Fractional CFO: the block after a peak sums the correlation at the peak, a CORDIC gives its angle in 2^-16 turns, and the NCO steps that over 1,024 samples. Whole bins are left to the receiver, as an index shift.
5. NCO: a 32-bit phase accumulator through a 1,024-entry, 12-bit sin/cos table.
6. FFT: 1,024 points, radix-2² decimation in frequency (the R2²SDF's arithmetic), 18-bit, each of the ten butterflies halving, Q15 twiddles with rounding. The window starts 8 samples into the CP. Bins come out bit-reversed.
7. Output scaling: bin k times G[k] A >> 13, G a per-bin gain table (Q4, `fpga/rtl/hdz_gain_c5narrow.hex`, measured through the C5's 802.11p filter, up to 8×) that lifts the band edges before quantising, A an AGC (Q5) that brings the mean |v| to 14 over each 64 symbols. Saturated to ±63.

### Bus format

16-bit words on the raw I/Q bus (PCLK 40 MHz), each lane value v a 7-bit byte read as 2v+1 by the P4, as raw I/Q. A bin never takes v = -64, so:

- (-64, -64) starts a symbol. Then three header words, each 14 bits as two 7-bit values (I bits 0-6, Q bits 7-13): {cfo_valid, resync, symbol count (12 bits)}; the window's offset in samples from where it would be without timing corrections since the last resync (signed), whose phase ramp across the bins the receiver turns back out; the AGC's A, which the receiver divides back out.
- Then 1,024 bins in bit-reversed order.
- (-64, 0) is idle.

1,028 words a symbol at 25.6k symbols a second is 26.3M words a second, 66% of the bus.

## Results of the model

The C receiver on the model's stream against it on the samples (float front end), first 4M samples of each Nov 4 capture, decision error (median squared distance to the nearest 16-QAM point) on bins |k| < 300 and 300-444:

| Capture | Float front end | Model front end |
| --- | --- | --- |
| 191225 | -20.0 to -20.4 dB, -15.7 to -17.5 dB (one frame -17.5/-13.5) | -19.0 to -19.8 dB, -14.3 to -16.0 dB (one frame -15.9/-12.1) |
| 191250 | -15.7 to -18.2 dB, -11.1 to -14.0 dB | -15.6 to -17.8 dB, -11.0 to -13.8 dB |

- Every frame of 191206, 191225 and 191250 finds its three special symbols. 191237 is weak in both (hdzrx finds 0-2 specials a frame there).
- The fixed-point FFT is within 0.13% of a float FFT; swapping in a float FFT or scipy's resampler changes nothing measurable, and moving the window (8, 4, 0, -4 samples back) shows 8 is best. The remaining 0.2-1 dB is in the timing and CFO tracking or the 7-bit output, not found yet.
- Without the gain table, the outer bins lost 4-6 dB to quantisation (their level is 15-20 dB down). Without the AGC, a fixed scale clipped 28-53% of bins on the louder captures.
- A timing correction of one sample is a full turn of phase ramp across the band, which broke the training runs (one run lost, read as 1080p30) until the header carried the offset for the receiver to undo.
- Slots agree with the float decode on 80-95% of non-noisy slots per frame; agreement says which differ, decision error says which is better.

## Resources, estimated

| Block | MULT9 | MULT18 | Block RAM |
| --- | --- | --- | --- |
| DC | | | 1 |
| Resample | 16 | | 1 (coefficients) |
| CP correlator, fold | 4 | | 3 |
| NCO | | 4 | 2 |
| FFT | | 12-16 | 7 (delays 512, 256, 128; twiddles) |
| Output scale, AGC | 1 | 2 | 1 (gain table) |

About 16.5 DSP macros with `fm_top`'s 6.5 makes 23 of 24, which is tight; synthesis will tell. The FFT can use the three-multiplier complex product.

## Stage B: equalise, demap, descramble

hdzrx's back end looks at the whole frame before deciding a bit: the scattered-pilot offset by the boost over all 409 data symbols, and a per-bin trim from the 4th power of all of them. A streaming receiver can't hold the frame, so stage B was tried causally in the C receiver first (`hdzrx_host --causal 1|2`), the first frame bootstrapped as now:

- The pilot offset steps by 8 every symbol, training and X included, so once known it is predicted from the symbol count since the last frame.
- `--causal 1`: each frame takes the previous frame's trim and noisy bins.
- `--causal 2`: no trim; a per-bin tracker, one complex tap a bin, normalised LMS (mu 0.05) on 16-QAM decisions, symbol by symbol, its taps carried from frame to frame. The 0/90 degree choice comes from the training symbols, which arrive before the data; the 0/180 one is an inversion of HP bits that can be flagged at the frame's end and applied by the P4.

On 200 ms of 191206, 191225 and 191250 (11 frames each), decision error on |k| < 300, and the share of slots that agree with the batch decode on bins neither flags as noisy:

| | Batch (hdzrx) | Previous frame's trim | Per-bin LMS |
| --- | --- | --- | --- |
| 191206 | -16.4 to -20.2 dB | -14.6 to -19.5 dB | -16.4 to -21.4 dB |
| 191225 | -17.4 to -20.3 dB | -15.9 to -19.6 dB | -17.4 to -21.6 dB |
| 191250 | -14.9 to -17.2 dB | -14.5 to -17.2 dB | -14.8 to -17.2 dB |
| Agreement with batch, 191225 | | 74-97% | 88-96% |

The tracker's decision error is optimistic (it adapts toward its own decisions), but its slots also agree with the batch decode better than the previous frame's trim does. It is the plan for stage B: per bin a tap (1,024 complex words, one block RAM), per data bin a decision, an error and an update, which is one complex multiply-accumulate a bin at the symbol rate. The outer bins (300-444) on 191250 lose about 1 dB against batch, and the noisy-bin flags from the previous frame conceal more records; both are open.
