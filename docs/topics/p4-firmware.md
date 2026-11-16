# P4 firmware

`firmware/p4` is a minimal ESP-IDF v6.1 app for the rev v3.2 P4, ported piece by piece from `reference/p4usb`.

## Console

The same line console answers on the high-speed USB (CDC, 303a:8000) and on UART0 through the CH343 (1a86:55d3). Replies go where the last input came from. Commands: `help`, `info`, `reboot [download]`, `pins`, `c5`.

The USB console stalls after two to four replies: the P4 keeps receiving and running commands, but its bulk IN transfer never completes until the host reopens the port. Ruled out (2026-11-15):

- TinyUSB 0.21 instead of 0.19, DWC2 DMA instead of slave mode, and pinning the USB task to the interrupt's core.
- p4usb's false-suspend workaround; `suspended` stays 0.
- Espressif's esp_tinyusb 2.4.0 wrapper (its own PHY setup, task on core 1, standard descriptors with a qualifier) stalls the same way.
- p4usb itself, rebuilt for rev v3, doesn't enumerate on this board at all.

- Espressif's untouched `tusb_serial_device` example stalls the same way after one echo; CherryUSB's example doesn't enumerate.

So it is outside our firmware: the cable, the USB 2.1 hub or the Mac. Next: the P4's high-speed port straight into the Mac with another cable. Until then, scripted work uses the CH343, whose auto-reset circuit restarts the P4 when the port opens.

## C5 control and the esptool bridge

The C5's UART is on P4 50 (TX) and 51 (RX); EN on 34 and BOOT on 35 are open-drain and released at boot. P4 TX stays an undriven input except during a bridge session, and UART1 only listens to the C5 during a session or `c5 log on`.

esptool on a console port with `--before no-reset` reaches the C5: the P4 sees esptool's SYNC, resets the C5 into download mode and relays both ways, following esptool's baud change. DTR/RTS on the USB console reboot the P4 itself into its ROM loader, for P4 flashing.

## IO supply

GPIO 39-48 (D2..D0 among them) run from the P4's LDO channel 4, the board's SD card IO supply. The firmware sets it to 3.3 V at boot. Nothing may drive those pins before that.

## Capture and stills

The Nano's `passthru_top` registers the C5's 14 lanes on its sample clock and drives them, with the inverted clock as PCLK, onto D13..D0 (Q7..Q1, I7..I1). It drives the bus only while the P4 holds the IRQ wire high (`pins bus on`), so a P4 in reset or before its LDO 4 is up sees high-Z.

`iq cap [samples] [pos|neg]` captures up to 1.6M samples into PSRAM through PARLIO RX (sample on the rising edge) and prints levels and per-lane toggle rates; `iq dump` sends them as base64. `still` decodes one NTSC field of a 1.4M-sample capture to 320x240 grey. `tools/iqcap.py` and `tools/still.py` run the whole sequence over the CH343 and save to `captures/` ([first frame](../experiments/2026-11-15-r4-first-frame.md)).

## Real-time decoder

`decode on [channel]` is p4usb's decoder: `iq_start` probes at 40 MS/s (gain, then slipping the C5's clock until the rising-edge capture is clean, since the Nano re-registers the bus), then runs at 1 in 6 (13.33 MS/s) with the BitScrambler phase lookup. It fills 720x480 packed 4:2:0 frames at 60 fps; `video` shows the rate and `video grab` sends the newest frame, which `tools/grab.py` saves as PNG ([experiment](../experiments/2026-11-15-realtime-decoder.md)).

While the Nano drives the bus, the UART to the C5 loses characters, so `c5_request` turns the bus off for each exchange.

## Tasks

- `rake p4:flash` builds and flashes over the high-speed USB ROM loader (rebooting the firmware into it with DTR/RTS), or `PORT=<CH343 port>` flashes over the CH343.
- `rake p4:console` opens the console in picocom.
- `rake c5:flash`, `rake c5:chip_id` and `rake c5:pinwalk` run esptool through the bridge. Over the CH343, set `PORT` to its port and `BAUD=115200`.
- `rake pins:check` runs the wiring check ([experiment](../experiments/2026-11-15-pin-mapping.md)).
