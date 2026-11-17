# P4 firmware

`firmware/p4` is a minimal ESP-IDF v6.1 app for the rev v3.2 P4, ported piece by piece from `reference/p4usb`.

## Console

The same line console answers on the high-speed USB (CDC, 303a:8000) and on UART0 through the CH343 (1a86:55d3). Replies go where the last input came from. Commands: `help`, `info`, `reboot [download]`, `pins`, `c5`.

The high-speed USB console works once header Pi pins 26/28, J3 25/27 (the high-speed pair, also on the header) don't continue down the ribbon: with the header disconnected it passed 360 commands and 200 back-to-back replies (342 KB) with nothing dropped. Before that, ribbon stubs on the pair made it stall after a few replies, and changing TinyUSB, DMA mode, task cores or Espressif's own examples didn't help ([wiring.md](../wiring.md)).

## C5 control and the esptool bridge

The C5's UART is on P4 50 (TX) and 51 (RX); EN on 34 and BOOT on 35 are open-drain and released at boot. P4 TX stays an undriven input except during a bridge session, and UART1 only listens to the C5 during a session or `c5 log on`.

With I/Q running, noise from the lanes reaches both ends of the C5 UART. It keeps the line from going idle, so both UARTs interrupt on every byte (RX full threshold 1) rather than waiting for 120 bytes or an idle gap. It also changes command characters, so `c5_request` appends ` #hh`, the XOR of the command's bytes, and c5rx answers `err check` to any line without a matching one ([experiment](../experiments/2026-11-17-c5-uart-stall.md)).

esptool on a console port with `--before no-reset` reaches the C5: the P4 sees esptool's SYNC, resets the C5 into download mode and relays both ways, following esptool's baud change. DTR/RTS on the USB console reboot the P4 itself into its ROM loader, for P4 flashing.

## IO supply

GPIO 39-48 (D2..D0 among them) run from the P4's LDO channel 4, the board's SD card IO supply. The firmware sets it to 3.3 V at boot. Nothing may drive those pins before that.

## Capture and stills

The Nano's `passthru_top` registers the C5's 14 lanes on its sample clock and drives them, with the inverted clock as PCLK, onto D13..D0 (Q7..Q1, I7..I1). It drives the bus only while the P4 holds the IRQ wire high (`pins bus on`), so a P4 in reset or before its LDO 4 is up sees high-Z. Use `passthru_lockout_top` instead: the C5 clock jumper picks up crosstalk from the lanes, which adds edges, and every extra edge is an extra sample. The lockout bitstream takes one C5 clock edge per 55 ns, which keeps the bus clock within 40 ppm of 80/6 MHz. `iq rate` measures the bus clock.

`iq cap [samples] [pos|neg]` captures up to 1.6M samples into PSRAM through PARLIO RX (sample on the rising edge) and prints levels and per-lane toggle rates; `iq dump` sends them as base64. `still` decodes one NTSC field of a 1.4M-sample capture to 320x240 grey. `tools/iqcap.py` and `tools/still.py` run the whole sequence over the CH343 and save to `captures/` ([first frame](../experiments/2026-11-15-r4-first-frame.md)).

## Real-time decoder

`decode on [channel]` is p4usb's decoder: `iq_start` probes at 40 MS/s (gain, then slipping the C5's clock until the rising-edge capture is clean, since the Nano re-registers the bus), then runs at 1 in 6 (13.33 MS/s) with the BitScrambler phase lookup. It fills 720x480 packed 4:2:0 frames at 60 fps; `video` shows the rate and `video grab` sends the newest frame, which `tools/grab.py` saves as PNG ([experiment](../experiments/2026-11-15-realtime-decoder.md)).

C5 requests turn the Nano's bus off for each exchange (`c5 pause on`, the default). With `c5 pause off`, 200 of 200 pings and gain steps passed while decoding, but three `decode on` starts in a row failed with lost replies, so the pause stays. Each pause stops the samples for 4-26 ms, and the automatic gain sends a request every few seconds on a weak signal. The decoder takes a gap between DMA nodes as lost lines: the field is cut and the next vertical sync is believed wherever it lands ([experiment](../experiments/2026-11-16-vsync-after-bus-pause.md)).

A vertical sync is believed only when a second broad pulse follows the first by half a line (or a line, when the detector is skipping to each hsync), and when the first starts within 3 us of a line's start or middle. Noise that ran into an hsync passed the old one-pulse test and moved the picture for four fields. `decode events` lists the last 64 sync events and gain steps and `decode nodes` the bus stops since last asked.

## Tasks

- `rake p4:flash` builds and flashes over the high-speed USB ROM loader (rebooting the firmware into it with DTR/RTS), or `PORT=<CH343 port>` flashes over the CH343.
- `rake p4:console` opens the console in picocom.
- `rake c5:flash`, `rake c5:chip_id` and `rake c5:pinwalk` run esptool through the bridge. Over the CH343, set `PORT` to its port and `BAUD=115200`.
- `rake pins:check` runs the wiring check ([experiment](../experiments/2026-11-15-pin-mapping.md)).

At 1 in 6, `iq_start` slips the C5's clock until a capture shows the fewest phase spikes, since the 40 MS/s probe's place doesn't carry over. `iq counter q|i [MHz]` runs the C5's counter on one lane group and counts repeats and skips through the Nano.

At power-up the P4 starts decoding on the last channel started with `decode on`, kept in NVS (namespace `p4vrx`, key `channel`). With none saved it waits for `decode on`.

The LCD (`lcd.c`, adapted from Waveshare's BSP, Apache-2.0) shows each decoded field: the PPA scales it to 630x480 on the panel's left, turns it a quarter turn and converts it to RGB565 in one of three DPI frame buffers, about 12 ms a frame, at 59.8 fps alongside UVC. The 170 px on the right hold the UI ([ui](ui.md)). `lcd` reports rates, `lcd rotate` flips the picture and `lcd grab` sends the frame buffer (`tools/lcdgrab.py`).

## Dev practice: fail fast

The boards come up in about a second or not at all. Wait 3-5 s for a console or a boot, retry once, then stop and look at why. Long sleeps and long polling timeouts only hide the failure. Builds and flashes get timeouts as long as they actually take: tens of seconds.
