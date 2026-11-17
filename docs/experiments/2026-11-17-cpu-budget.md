# 2026-11-17: P4 CPU budget with decode, LCD and UVC

## Question

With live analog decode, the LCD at 60 fps and UVC H.264 streaming all running, how much of each P4 core is left, and what falls short of 60 fps?

## Setup

- Commit: a2720fc9 plus a `cpu` console command (`firmware/p4/main/cpu.c`) and FreeRTOS run-time stats (`CONFIG_FREERTOS_USE_TRACE_FACILITY`, `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`).
- Bitstream: the lockout bitstream in the Nano's flash.
- Boards: the pinned p4vrx P4, C5 and Nano, external antenna, a VTX on R4.
- Wiring changes from `../wiring.md`: none.

## Method

`cpu 5000` takes FreeRTOS run-time counters (esp_timer microseconds) 5 s apart and gives each task's share of a core, and each core's idle task. Decoder fields, overruns and dropped lines, encoded frames and UVC frames sent are counter differences across the same window (5.5 s). Subsystems were switched with `lcd show on|off` and `video encode on|off`, with the decoder running throughout. The UVC host was macOS through `camgrab p4vrx <file>.mp4 -t 16`, started 4 s before the window. The UI strip sat on its main screen. Script: `/tmp/cpu.py`; outputs in `captures/cpu/`.

## Results

| Running | Core 0 idle | Core 1 idle | Fields/s | LCD fps | Encoded/s | UVC sent/s | Encode ms | LCD scale+turn ms |
|---|---|---|---|---|---|---|---|---|
| decode | 47.6% | 37.5% | 59.7 | off | off | 0 | | |
| decode + LCD | 44.8% | 37.3% | 59.5 | 59.8 | off | 0 | | 8.6 |
| decode + encoder | 46.5% | 33.6% | 59.7 | off | 59.7 | 0 (no host) | 9.0 | |
| decode + encoder + UVC | 42.9% | 33.0% | 59.7 | off | 59.7 | 59.7 | 8.4 | |
| decode + LCD + encoder | 38.7% | 31.2% | 59.4 | 52.0 | 53.6 | 0 (no host) | 19.4 | 13.9 |
| decode + LCD + encoder + UVC | 34.4% | 29.8% | 59.1 | 53.6 | 52.0 | 52.0 | 21.3 | 13.1 |

Tasks with everything on, as a share of their core:

- Core 1: `decode` (demod and sync) 65.0%, `video` (encoder feed) 2.6%.
- Core 0: `decode_draw` 46.7%, `screen` (UI strip, touch poll every 20 ms) 11.6%, `lcd` 4.2%, `decode_ctl` 0.3%, `esp_timer` 0.2%.
- Unpinned: `uvc` 3.1%, `usb` 1.7%, `c5_relay` 0.4%.

Other observations:

- The decoder kept 59.1-59.9 fields/s in every case, with up to 0.5 ring overruns/s and 4-10 dropped lines/s.
- UVC sent every frame the encoder made. Without a host the encoder's frames are refused by the USB stack, at the same rate.
- `screen` costs 6.3-7.5% of core 0 with the LCD off and 11.6% with it on.
- The `.mp4` files camgrab wrote hold 398 and 406 frames in 15.9 s (25 fps). That is AVFoundation decoding and ffmpeg re-encoding on the Mac, not what the P4 sent; the P4's sent counter is the measure.

## Verdict

CPU has headroom: with everything on, core 0 is 34% idle and core 1 30% idle. The decoder holds 59-60 fields/s throughout.

What is over budget is the LCD and the H.264 encoder together: both drop to 52-54 fps. Each alone runs at 59.7-59.8 fps, the encoder in 8-9 ms a frame and the PPA scale-and-turn in 8.6 ms. Together the encoder takes 19-21 ms (over the 16.7 ms frame) and the PPA 13-14 ms. The CPU is idle meanwhile, so the contention is in memory: both read the 720x480 field from PSRAM, the PPA writes RGB565 frame buffers in PSRAM, and the DPI panel reads one continuously.

## Next

- Find what the encoder and PPA share: PSRAM bandwidth, DMA-2D channels, or the cache writeback before each (`CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS`). The frame buffers (768 KB each) can't move to internal RAM, so the likelier fix is to stagger the two so they don't run at the same time in each field.
- `screen` at 11.6% of core 0 for a mostly static strip is worth a look: it redraws the sparkline every 100 ms and the LCD copies the strip into frame buffers after each change.
- Repeat with HDZero decode when it exists; it will move load off the CPU and onto the bus.
