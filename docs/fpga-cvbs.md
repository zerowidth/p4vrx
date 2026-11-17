# CVBS decode in the Nano (phase 3b design)

Move the analog decoder from the P4 into the Nano in three stages, within the ceiling in [the budget](fpga-budget.md): 6,600 LUT4, 4,400 flops, 18 block RAMs, 4.5 DSP macros (18 MULT9), no PLL, and the SDRAM. At the end the Nano sends finished 720x480 frames, and the P4 does display, UI, UVC and the encoder.

Status: design. No model or RTL yet.

## Where it starts

The Nano already has the FM demod's 14-bit frequency at 13.33 MS/s (2.44 kHz an LSB), filtered and without wraps. The P4's decoder (`firmware/p4/main/decode.c` and its assembly, adapted from C5VRX p4usb, so GPL-3.0) does the rest, on both cores:

- Sync: a slicer whose thresholds follow each line's tip and porch levels, a line clock as a 16.16 period (847.39 samples a line), vsync from broad pulses in a window, field parity, click handling.
- Chroma: the subcarrier is exactly 189/704 of 13.33 MS/s, so a 704-entry table follows it with no drift; burst phase averaged over lines.
- Luma, deinterlace with the previous field, temporal noise reduction, and packing into the encoder's 4:2:0.
- Today that's core 1 at about 55% and core 0 at about 45% (the CPU budget).

RTL ported from that code stays GPL-3.0 with the C5VRX credit, as the repo's licence rules say.

## Stage 1: sync and line clock

Port the slicer, line clock, vsync and parity to the Nano on the 13.33 MS/s frequency stream, the same algorithm and constants as the P4's.

- Out: the composite, line-locked. Each line is resampled to 858 samples (BT.601's 13.5 MHz, 720 active) by a 4-tap cubic interpolator on the line clock's fraction: 4 MULT9, a coefficient table in 1 block RAM.
- On the bus: 14-bit samples at 13.5 MHz with the line's first sample and the field's first line flagged in the low bits.
- The P4 skips its sync and line clock and keeps its chroma, luma and deinterlace on lines it no longer has to find.
- Estimate: 1,500 LUT4, 1,200 flops, 2 block RAM, 1 DSP macro.

## Stage 2: chroma and luma

- Sampled line-locked at 858 a line, the subcarrier is 227.5 cycles a line: its phase per sample is fixed, flipping 180° each line, and only the burst sets the absolute phase. One table of 858 phases, 1 block RAM.
- Burst: correlate the burst window against the table each line and average the phase over lines, as the P4 does (weight 1/4).
- Demodulate U and V with two MULT9 against the table, low-pass to 1.3 MHz with shift-and-add FIRs, saturation and hue as two more MULT9.
- Luma: the composite less the remodulated chroma, or a notch at 3.58 MHz in shift-and-add.
- Out: 4:2:2, Y and alternating Cb/Cr, 720 a line.
- Estimate: 2,200 LUT4, 1,500 flops, 3 block RAM, 1.5 DSP macros.

## Stage 3: comb, deinterlace, noise reduction

- Comb: a 2-line comb on chroma using the line before, 2 block RAM.
- SDRAM field store: each field written as it comes, the previous field read for motion-adaptive deinterlace, the previous frame read for temporal noise reduction.
- Bandwidth in 4:2:0 at 1.5 bytes a pixel: write 15.6 MB/s (720x240 a field, 60 fields), read the previous field 15.6, read the previous frame 31.1: about 62 MB/s against about 160 MB/s usable (32 bits at 67.5 MHz).
- SDRAM controller: about 800 LUT4. Line FIFOs to and from it: 4 block RAM.
- Estimate: 2,500 LUT4, 1,600 flops, 8 block RAM, 1.5 DSP macros.

Stages 1-3 together: about 6,200 LUT4, 4,300 flops, 13 block RAM, 4 DSP macros, inside the ceiling with little to spare. If it overruns, the noise reduction goes first (the P4 has it now), then the comb.

## Frames to the P4

- Format: the P4's own 720x480 packed 4:2:0, the layout its decoder writes today and that both the PPA (`PPA_SRM_COLOR_MODE_YUV420`) and the H.264 encoder read. The Nano writes it as bytes and the P4's DMA puts them straight into a frame buffer. No conversion on the P4.
- Rate: 720 × 480 × 1.5 bytes × 60 = 31.1 MB/s.
- Bus: 8 lanes as a byte with PCLK at 40 MHz (40 MB/s, 78% busy), plus one lane as a frame-valid flag for PARLIO's level delimiter, so each frame lands as one transfer. The raw 40 MS/s mode already proved PCLK at 40 MHz on this ribbon.
- The P4 then does no per-pixel work: the LCD's PPA pass, the encoder and the UI. Both cores' 45-55% decode load goes, leaving room for HDZero's block decode and IDCT.

## Verification, model first

1. Captures: `iq grab 8000000` gives 200 ms (12 fields) at 40 MS/s; `models/fm_demod.py` turns it into the 13.33 MS/s frequency stream the Nano has, bit for bit.
2. Stage 1 model: the slicer and line clock in Python, checked against the P4 decoder's own line count, line period and vsync on the same capture. `decode events` and the line period (847.39 samples) are the references.
3. Stage 2 model: decode a field to RGB and compare with a `video raw` grab of the P4's picture on the same transmitter: saturation, burst phase jitter (4° on the P4), row-to-row chroma jumps.
4. Each stage's RTL bit for bit with its model in iverilog, then hardware A/B against the P4 decoder (tip noise, clicks, colour, CPU), as for the demod.

## Open

- The bus format at stage 1 and 2 is transitional: the P4 decoder has to take line-locked composite and then 4:2:2 lines, which is P4 work that goes away at stage 3. Skipping straight to stage 3's frame format with stages 1-2 inside the Nano is the alternative; it delays the first hardware result.
- SDRAM clock from the lockout PLL's divided output is untested.
- Multi-boot, if HDZero and CVBS don't fit together ([budget](fpga-budget.md)).
