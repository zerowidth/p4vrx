# Nano resource budget

What the GW2AR-18 in the Tang Nano 20K has, what `fm_top` uses, what the HDZero PHY is expected to need, and the ceiling that leaves for CVBS decode (phase 3b).

## The device

GW2AR-LV18QN88C8/I7: 20,736 LUT4, 15,552 flops, 46 block RAMs of 18 kbit (828 kbit), 24 DSP macros (each 2 MULT18X18 or 4 MULT9X9: 48 or 96), 2 rPLLs, and 64 Mbit of SDR SDRAM in the package (2M × 32).

## `fm_top` today (build 3, 2026-11-17)

Lockout pass-through, I2C registers, self-check patterns, FM demod at 40 MS/s, HDZero detector, raw 40 MS/s mode.

| Resource | Used | Of | Share |
| --- | --- | --- | --- |
| LUT4 | 4,043 | 20,736 | 19% |
| ALU (carry cells) | 1,840 | 15,552 | 11% |
| Flops | 3,564 | 15,552 | 22% |
| Block RAM | 10 | 46 | 21% |
| DSP | 8 MULT18 + 9 MULT9 (about 6.5 macros) | 24 macros | 27% |
| rPLL | 2 | 2 | 100% |

Timing: the 40 MHz domain closes at 71.7 MHz, the lockout's 135 MHz at 371 MHz, I2C's 27 MHz at 107 MHz.

The detector is 8 of the 10 block RAMs and most of the flops, and is part of HDZero's budget below.

## HDZero PHY, estimated

From `hdzero-reference/SPEC.md` and `hdzero-captures/docs/decoding.md`: 27 MS/s (17 MS/s narrowband), 1024-point FFT, 32-sample prefix, 16-QAM with hierarchical bits, 823-824 data slots a symbol, 409 data symbols a frame, about 60 frames a second. That's about 1.35 Mbit a frame, 81 Mbit/s (10 MB/s) of slots to the P4, which the bus carries at 14 bits × 13.33 MHz (23 MB/s).

| Block | LUT4 | Block RAM | DSP macros |
| --- | --- | --- | --- |
| DC and CFO: NCO, sin/cos table, complex multiply | 300 | 1 | 2 |
| Resample 40 to 27 (or 17) MS/s: 8-tap cubic, complex | 800 | 1 | 4 |
| Symbol timing from the detector, FFT window, frame sync on the training symbol | 600 | 2 | 1 |
| 1024-point FFT, radix-2² single-path delay feedback, 27 MS/s | 3,500 | 6 | 8 |
| Channel estimate from pilots, equalise (multiply by conj(H)/\|H\|²) | 1,200 | 3 | 3 |
| Whole-bin CFO from continual pilots, per-bin phase trim | 600 | 1 | 1 |
| Demap, descramble, slot order, FIFO to the P4 | 800 | 2 | 0 |
| Total | about 7,800 | about 16 | about 19 |

These are estimates from similar cores, not syntheses. The FFT is the big one.

## The split

| | LUT4 | Flops | Block RAM | DSP macros | PLL |
| --- | --- | --- | --- | --- | --- |
| Device | 20,736 | 15,552 | 46 | 24 | 2 |
| `fm_top` today | 4,043 | 3,564 | 10 | 6.5 | 2 |
| Reserved for HDZero | 8,000 | 6,000 | 16 | 12 | 0 |
| Margin for routing and timing (10%) | 2,000 | 1,500 | 2 | 1 | |
| Ceiling for CVBS decode (3b) | 6,600 | 4,400 | 18 | 4.5 | 0 |

- DSP is the tight one. CVBS decode has to use LUT shift-and-add for its fixed filters, and 9×9 multipliers (18 of them in 4.5 macros) for chroma demodulation and line resampling.
- No PLL is free. The SDRAM's clock comes from the lockout PLL's divided output (135 MHz / 2 = 67.5 MHz), and HDZero's 27 MS/s runs on the 40 MHz clock with a clock enable.
- SDRAM: 32 bits at 67.5 MHz is 270 MB/s peak, about 160 MB/s in practice. A field store with deinterlace and noise reduction needs about 60 MB/s (below).

## If both don't fit

Mode-specific bitstreams: an analog image (FM demod, CVBS decode, the HDZero detector to notice a switch) and an HDZero image (PHY, and a small FM detector to notice analog). The P4 would switch by having the Nano reload.

- Reload time: `openFPGALoader -r` to the Nano answering on I2C again took 1.8 s (2026-11-17, polled every 0.3 s). That's a noticeable but acceptable gap on a mode change.
- Two images in the Nano's flash need a way to pick one. GW2A devices load from a fixed flash address; a second image at another address needs the device's multi-boot support, which isn't confirmed for this part. To check: the GW2A-18 datasheet's configuration chapter, and whether openFPGALoader can write at an offset.
- Triggering the reload from the P4: no header pins are free. Options are the IRQ wire (Nano 79) repurposed as RECONFIG_N, or user logic that triggers reconfiguration from an I2C command if the primitive exists.
- Until that's settled, the plan is one bitstream within the split above.
