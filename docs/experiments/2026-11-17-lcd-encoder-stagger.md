# 2026-11-17: LCD and H.264 encoder at 60 fps together

## Question

The LCD and the H.264 encoder each run at 59.8 fps alone but 52-54 fps together ([cpu budget](2026-11-17-cpu-budget.md)). Can staggering them within each field, or cutting other memory traffic, bring both to 59.8 fps while UVC streams?

## Setup

- Commit: f273 plus `video stagger 0|1|2` and `video lcd N` (this commit).
- Bitstream: the lockout bitstream in the Nano's flash.
- Boards: the pinned p4vrx P4, C5 and Nano, external antenna, a VTX on R4.
- Wiring changes from `../wiring.md`: none.

## Method

Decode on R4, the LCD showing, the encoder on, and macOS streaming UVC through camgrab. Each case is a 5.5 s window of counter differences (`/tmp/cpu.py`, outputs in `captures/cpu/`): LCD frames shown, frames encoded and sent over UVC, with `cpu 5000` for core load. `video stagger 0` hands the field to the LCD and the encoder at once, as before. `1` encodes once the PPA has finished scaling the field into a frame buffer. `2` offers the field to the LCD once it's encoded. `video lcd 2` gives the LCD every other field. One build stepped the UI strip's signal history every 500 ms instead of 100 ms.

## Results

| Case | LCD fps | UVC fps | Encode ms | PPA ms | `screen` on core 0 | Core 0 idle | Core 1 idle |
|---|---|---|---|---|---|---|---|
| stagger 0 (before) | 52.8 | 52.8 | 19.9 | 12.6 | 11.6% | 35.7% | 30.7% |
| stagger 1 | 48.1 | 48.0 | 11.5 | 9.2 | | 37.9% | 31.8% |
| stagger 2 | 52.6 | 52.8 | 15.9 | 13.9 | | 35.6% | 30.5% |
| LCD every 2nd field | 29.6 | 59.2 | 18.7 | 12.4 | | 37.0% | 30.4% |
| LCD every 2nd field, stagger 1 | 29.8 | 59.6 | 11.6 | 8.5 | | 36.2% | 30.1% |
| strip history every 500 ms | 57.0 | 57.0 | 16.9 | 12.4 | 3.7% | 42.5% | 32.5% |

- The decoder kept 59.2-59.6 fields/s in every case.
- Run one after the other (stagger 1), the PPA and encoder each take close to their standalone times, but 9.2 + 11.5 ms is more than a 16.7 ms field, so the rate falls to 48 fps.
- The PPA already uses its largest burst, 128 bytes. Its 90° turn writes the picture a column at a time across PSRAM.
- The UI strip redraws every 100 ms on the main screen because the signal sparkline moves. Each redraw is a PPA pass turning the 170x480 strip into the three frame buffers, about 3 ms each. That traffic costs 4 fps of LCD and UVC, and 8% of core 0.

## Verdict

Staggering doesn't get both to 60 fps: overlapped they share bandwidth, and in series they don't fit in a field. The best with both at full rate is 57.0 fps, with the strip redrawn less often. LCD at every other field while streaming gives UVC 59.6 fps and the LCD 29.8 fps, cleanest with stagger 1. Both switches stay, off by default.

## Next

- Copy only the strip's changed rows (the sparkline) to the frame buffers rather than the whole strip, three times. That keeps the 100 ms history and should recover most of the 4 fps.
- Decide between LCD at 30 fps while a UVC host streams, and 57 fps on both with a cheaper strip. Without a host, the encoder could skip fields entirely and leave the LCD at 59.8 fps.
