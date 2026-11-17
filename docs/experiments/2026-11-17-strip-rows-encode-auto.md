# 2026-11-17: Copy only the strip's changed rows, and encode only while streaming

## Question

The UI strip's sparkline redraw and the H.264 encoder both take memory bandwidth from the LCD ([stagger](2026-11-17-lcd-encoder-stagger.md)). Without changing what the UI shows, how close do LCD and UVC get to 60 fps together, and does the LCD reach 60 fps when nothing streams?

## Setup

- Commit: this one, on 4540.
- Bitstream: the lockout bitstream in the Nano's flash.
- Boards: the pinned p4vrx P4, C5 and Nano, external antenna, a VTX on R4.
- Wiring changes from `../wiring.md`: none.

## Method

- Strip: when only the main screen's signal history has moved, `ui_draw` redraws just its 88 rows and `ui_drawn_rows` says which. `lcd_strip_end(lo, hi)` has the PPA turn only those rows into each frame buffer. A first try diffed the whole strip against a PSRAM shadow copy instead.
- Encoder: `video encode auto`, the new default, encodes only while a UVC host streams, and forces a keyframe on resuming. `on` and `off` remain.
- Measured as before: `/tmp/cpu.py`, a 5.5 s window, camgrab as the UVC host; outputs in `captures/cpu/`.

## Results

| Case | LCD fps | UVC fps | Encode ms | `screen` on core 0 | Core 0 idle | Core 1 idle | Lines dropped/s |
|---|---|---|---|---|---|---|---|
| before, streaming | 52.8 | 52.8 | 19.9 | 11.6% | 35.7% | 30.7% | 3.7 |
| whole-strip diff, no host | 59.2 | | | 12.6% | 40.4% | 36.1% | 222.6 |
| whole-strip diff, streaming | 51.6 | 52.1 | 26.6 | 17.3% | 29.6% | 27.2% | 202.4 |
| UI-reported rows, no host | 59.9 | | | 0.9% | 52.1% | 39.4% | 0.4 |
| UI-reported rows, streaming | 58.0 | 57.8 | 16.9 | 2.7% | 44.8% | 34.2% | 3.2 |

- A strip copy of the 88 history rows takes 0.79 ms, against about 3 ms for the whole strip.
- The diff read 163 KB of PSRAM per redraw on core 0, which starved the decoder's drawing of memory: 200 lines/s dropped.
- A 14 s UVC clip taken after the encoder resumed decoded with no errors from its first frame.

## Verdict

Adopted: UI-reported rows and `video encode auto`. With no UVC host the LCD runs at 59.9 fps, and streaming gives 58.0 fps on the LCD and 57.8 on UVC. The whole-strip diff is rejected.

## Next

The last 2 fps while streaming is the PPA's picture pass and the encoder sharing bandwidth; `video lcd 2` remains the way to give UVC a full 60 fps.
