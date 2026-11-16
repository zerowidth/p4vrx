# 2026-11-16: extra PCLK edges from data-lane crosstalk

## Question

Why does p4vrx's colour come out 2x over-saturated, with hue jumps that change whole lines at once, while p4usb's colour from the same camera is clean?

## Setup

- Boards: P4 (p4vrx), Nano with `passthru_top` in SRAM, C5 on `firmware/c5`; p4usb only read as a UVC camera.
- Wiring as in `../wiring.md`: PCLK shares the ribbon with the 14 data lanes.

## Method

- Offline: in raw captures, find hsync edges and measure the burst phase in the decoder's absolute-sample frame and relative to each line's start.
- Live: count samples against the P4's `esp_timer` (`decode` prints the rate), and cross-check the P4's clock against the Mac's over USB across 120 s.
- `iq rate [q_top i_top [places]]`: time a 1.4M-sample PARLIO capture between its first and its filling partial-receive callbacks, with the C5 sending chosen MODEM_DIAG bits, its link counter, or slipping through clock places. `iq N` now prints the same rate.
- C5: `clk` reads PCR's PARLIO TX clock register; `clk <sel> <div>` sets it; `drive <data> <clk>` sets the lanes' pad drive (0-3).

## Results

- The line period reads 849-853 samples against 847.41 nominal, wandering by 1-2 samples a line. In the absolute frame the burst has no fixed frequency (coherence 0.08-0.12 at the best one); relative to line start, adjacent lines sit near 180 degrees apart as NTSC expects.
- The P4's clock matches the Mac's to -46 ppm over 120 s. Samples arrive +3267 ppm fast against the Mac.
- The C5's register is right: PLL_F240M (240 MHz) divided by 18, crystal read as 48 MHz.
- Bus clock by what the lanes carry, at default drive: static bits (31 31) +17 ppm; link counter exactly 240/19 MHz; I/Q (9 19) +1400 to +18500 ppm, depending on gain (+1400 at 60, +18500 at 40) and clock place; the lowest, busiest bits (6 6) +6.9%.
- The extra rate matches the line period: +6916 ppm read 853.1 samples a line, against 847.41 x 1.006916 = 853.27.
- Pad drive 0 on the data lanes brought one session down to +36 to +189 ppm at every one of 20 places. After the next `iq start` it read +3933 to +7019 ppm.
- With `passthru_pll_top` loaded, whose clock to the P4 comes only from the Nano's PLL (locked to the C5 clock, and gated off while unlocked): static lanes +17 ppm; I/Q at gain 60, 52 and 40 read -50,000, -96,000 and -400,000 ppm; the busiest bits -564,000 ppm. Lane activity breaks the PLL's lock, so the C5 clock is already corrupted at the Nano's input (C5 GPIO 0 to Nano 77, a breadboard jumper among the lane jumpers).
- `passthru_lockout_top`: a C5 clock edge sets a clean clock only while armed. A 135 MHz count (from a PLL on the 27 MHz oscillator) clears it 22-30 ns after it rises and re-arms it 44-52 ns after, so at most one edge is taken per 75 ns period. Its rising edge comes straight from the C5's edge. The bus clock then read +8 to +36 ppm with static lanes, I/Q at gain 40, 52 and 60, and the busiest bits.
- With it, decoding R4: line period 847.39 samples (847.41 nominal); sync-tip noise 93-156 kHz across clock places (150-1000 before); 74 lines with clicks in 30 s, about 0.04 a field (about 42 a field before); burst steady at 8855.
- Side by side with p4usb (`img/2026-11-16-lockout-vs-p4usb.jpg`, p4vrx left): saturation 14.5 vs 15.6, row-to-row chroma jumps 3.6 vs 3.5, luma high-pass 4.41 vs 4.14. Before the lockout: 30.2 vs 15.2 and 13.6 vs 3.2.
- The P4's GPIO glitch filter on PCLK at 20/16 ns or longer stops the capture; anything shorter is refused.

## Verdict

Adopted: `passthru_lockout_top`, which brings p4vrx's colour, clicks and sync level with p4usb. Rewiring the C5 clock as a twisted pair is still worth doing for margin; the cause: Crosstalk from the data lanes adds spurious PCLK edges at 0.1-2% of the clock rate. Every extra edge is an extra sample, so the video is stretched in time. The colour burst then rotates against the decoder's fixed 189/704 subcarrier table, by about 160 degrees a line at +0.3%. That explains the over-saturation (a rotating average shrinks the burst), the hue jumps that change whole lines, and probably many of the clicks. The P4's glitch filter can't pass the clock, and lowering the drive doesn't help reliably, but a lockout in the Nano does.

## Next

- Run the C5 clock jumper (C5 GPIO 0 to Nano 77) on its own wire, twisted with a ground and away from the lane jumpers; do the same for Nano 26 to P4 GPIO 4, which is on the ribbon. Try a 33-47 ohm series resistor at the C5's clock pin.
- Re-run `iq rate` and the side-by-side after each change; the target is under 50 ppm in every state.
