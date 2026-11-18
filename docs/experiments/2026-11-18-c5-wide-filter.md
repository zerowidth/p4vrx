# 2026-11-18: the C5's receive filter for HDZero

## Question

What narrows the C5's receive band to about ±6 MHz, which costs HDZero 15-20 dB on its outer bins and 600-1,900 concealed records a frame ([gap list](../hdzero-gaps.md)), and can it be widened without breaking analog?

## Setup

- Commit: this one. C5 firmware with a new `11p 0|1` command, P4 with `iq grab N wide [settle_ms]`.
- Bitstream: `fm_top` build 3 (flash), raw 40 MS/s mode for the grabs.
- Boards: the pinned p4vrx P4, C5 and Nano. Analog VTX on R4 (5769 MHz), weak late in the session (gain 73-77).

## Method

- `firmware/c5/main/radio.c` turns the PHY's 802.11p setting on (`phy_11p_set(1, 0)`) in `receive_state`. Its comment: the setting narrows the receive filter to about 12 MHz and halves the bus rate (40 MS/s instead of 80), which gives the P4 more clock positions that read the lanes clean. With it off, the C5 keeps one bus sample in two (`every` 2), so the grab stays at 40 MS/s.
- Grabbed 400k samples on R4 with 802.11p on and off, and compared Welch spectra (4,096 and 1,024 points) against the level within ±2 MHz.
- Read the analog decoder's sync-tip noise and clicked lines over 5 s with each setting.
- Grabbed with the switch inside `iq grab` (wide, settle 0, 5, 20, 50 ms) and checked the first 10k samples against the rest, and the analog decoder after each.

## Results

The same analog signal, 802.11p on against off, in dB against the level within ±2 MHz:

| Offset | On | Off | Off less on |
| --- | --- | --- | --- |
| 4 MHz | -10.7 | -11.2 | -0.5 |
| 6 MHz | -26.0 | -22.6 | +3.4 |
| 8 MHz | -39.9 | -27.2 | +12.7 |
| 9.4 MHz (bin 355) | -44.3 | -29.8 | +14.5 |
| 10.5 MHz | -46.4 | -34.3 | +12.1 |
| 11.7 MHz (bin 444) | -47.4 | -38.4 | +9.0 (on is at its floor) |

- With the signal weaker later, the wide grabs read flat within 2 dB from 6 to 11 MHz (-15 to -17 dB), and the narrow ones fell from -20 to -42 dB over the same span.
- Neither setting's grab had a sample-to-sample jump over 1.5× RMS (0 per thousand on R4): the lanes read clean at 40 MS/s with 802.11p off.
- Analog with 802.11p off: sync-tip noise 726 kHz against 171, and 55,192 clicked lines in 5 s against 11. Switching back gave 192 kHz and 1 line.
- Inside a grab the switch is in effect within the first 10k samples (0.25 ms) at any settle time. The first version sent `11p 1` once after the grab, and the C5 didn't take it: its UART loses characters beside a running bus, and a check byte makes it refuse a garbled command. Analog stayed broken (4,601 clicked lines in 3 s) until a manual `c5 send 11p 1`. Three tries each way fixed it; analog was back to its baseline after each of six grabs.

## Verdict

Adopted for HDZero grabs only: `iq grab N wide`, `hdz decode wide=1`, and `tools/hdzwatch.py --filter ab` (the default), which alternates narrow and wide grabs so each detection at the event gives both. Analog keeps 802.11p on. Whether the wide filter recovers HDZero's concealed records needs an HDZero transmitter; the expectation from the table is most of the 15-20 dB at the outer bins back.

## Next

At the event, compare concealed records between narrow and wide grabs of the same pilot (`hdzdecode.py` prints the filter). If wide wins, make it `hdz decode`'s default.
