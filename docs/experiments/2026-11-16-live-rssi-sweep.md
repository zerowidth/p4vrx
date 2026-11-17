# 2026-11-16: other channels' levels while decoding (parked)

## Question

Can the C5 look at other channels during the analog vertical blanking interval and come back without disturbing the picture, so the channel picker can show live levels next to live video?

## Setup

- Boards: p4vrx (C5, Nano with `passthru_lockout_top`, P4) decoding R4; the C5's BOOT line (P4 GPIO 35 to C5 GPIO 28) as a trigger after boot
- Code: `captures/live-sweep.patch` (not committed)
- Background: FPVGateC5MK (CC BY-NC-SA 4.0) measures 8 channels about 1000 times a second each with a C5. Only facts from its notes were used, no code: the PLL-only setter `phy_set_rf_freq_offset(xtal_sel, mhz, trim)`, with the crystal selector at `phy_param[49]` and the trim as an int16 at `phy_param[30]`, takes about 400 us, 300 of them a fixed wait, and the PLL has settled about 100 us in.

## Method

- On the C5, `hoptime` timed `phy_set_freq` (what `radio_tune` uses) against the PLL-only setter.
- Once a field, the P4 pulled the BOOT line low for (1 + n) x 20 us at a time predicted from the previous vertical sync. The C5 timed the pulse and hopped its LO to channel n and back. The P4 read the channel's power from the I/Q magnitude codes it already decodes, in a window after the rise.
- `decode sweep profile` recorded the I/Q power in 4.8 us bins around a hop, to find the window. `camgrab` clips scored the frame-to-frame change by row band with the sweep off and on.

## Results

- `phy_set_freq`: 763-1281 us out, 764-777 us back. PLL-only: 400-417 us out, 399-400 us back.
- Hopping R4 to an empty R1, the power left R4's 35 dB at the rise, settled at 8-11 dB from 135 us to 410 us, and was back at 35 dB by 495 us. Five profiles agreed to one 4.8 us bin once the P4 mapped time to samples from the DMA position and the last node interrupt, rather than by counting interrupts: the driver coalesces interrupts.
- Sweeping R1-R8 once a field, so each channel refreshed at 7.5 Hz: R4 (the live carrier) read 34-37 dB, the others 8-22 dB, with 0 missed looks over hundreds of fields.
- The picture suffered at the top. Frame-to-frame change by row went from 1.2-3.2 with the sweep off to 7.6-36.6 in rows 0-16, depending on the hop's line. The hop landed on decoder lines 8-20 (line 20 is the first drawn line) and wandered 5-10 lines from field to field (5-32 overall). The P4's pulse was 4-6 us late on average, so the wander came from elsewhere and wasn't traced. Having the decoder ignore samples while the LO was away shifted the picture down about 20 rows.
- Spinning to time the pulse on core 0 cost drawing up to about 25000 dropped lines in 6 s.
- The BOOT wire carried about 3400 glitches a second, each 3-4 us, even with the P4 driving it push-pull; a 1 us glitch filter on the C5 broke the I/Q stream. The C5 stopped answering commands twice and needed a reset.
- Linking `esp_timer_start_once` into the P4 image moved the RAM layout 128 bytes and set off a boot-time store access fault while creating the main task (`prvInitialiseNewTask`, fault at 0x4ff674c0 in ordinary heap). The fault depends on the layout alone and is still open.

## Verdict

Parked. Hops and levels work, but keeping the picture clean needs the line wander traced, the decoder taught to ride through the gap, and a cleaner trigger than the BOOT wire. Channel levels will come from a sweep while a menu is open, with video paused, as it must be for a band change anyway.

## Next

- A paused sweep for the channel picker and band grid.
- Trace the boot fault, which any change can trigger.
