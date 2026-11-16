# P4 firmware

`firmware/p4` is a minimal ESP-IDF v6.1 app for the rev v3.2 P4, ported piece by piece from `reference/p4usb`.

## Console

The same line console answers on the high-speed USB (CDC, 303a:8000) and on UART0 through the CH343 (1a86:55d3). Replies go where the last input came from. Commands: `help`, `info`, `reboot [download]`, `pins`, `c5`.

The USB console stalls after two to four replies: the P4 keeps receiving and running commands, but its bulk IN transfer never completes until the host reopens the port. Ruled out (2026-11-15):

- TinyUSB 0.21 instead of 0.19, DWC2 DMA instead of slave mode, and pinning the USB task to the interrupt's core.
- p4usb's false-suspend workaround; `suspended` stays 0.
- Espressif's esp_tinyusb 2.4.0 wrapper (its own PHY setup, task on core 1, standard descriptors with a qualifier) stalls the same way.
- p4usb itself, rebuilt for rev v3, doesn't enumerate on this board at all.

What stays in common is the console code, the Mac and the USB 2.1 hub. Next: plug the P4's high-speed port straight into the Mac, then try a bare TinyUSB CDC echo example. Until then, scripted work uses the CH343, whose auto-reset circuit restarts the P4 when the port opens.

## C5 control and the esptool bridge

The C5's UART is on P4 50 (TX) and 51 (RX); EN on 34 and BOOT on 35 are open-drain and released at boot. P4 TX stays an undriven input except during a bridge session, and UART1 only listens to the C5 during a session or `c5 log on`.

esptool on a console port with `--before no-reset` reaches the C5: the P4 sees esptool's SYNC, resets the C5 into download mode and relays both ways, following esptool's baud change. DTR/RTS on the USB console reboot the P4 itself into its ROM loader, for P4 flashing.

## IO supply

GPIO 39-48 (D2..D0 among them) run from the P4's LDO channel 4, the board's SD card IO supply. The firmware sets it to 3.3 V at boot. Nothing may drive those pins before that.

## Tasks

- `rake p4:flash` builds and flashes over the high-speed USB ROM loader (rebooting the firmware into it with DTR/RTS), or `PORT=<CH343 port>` flashes over the CH343.
- `rake p4:console` opens the console in picocom.
- `rake c5:chip_id` and `rake c5:pinwalk` run esptool through the bridge. Over the CH343, set `PORT` to its port and `BAUD=115200`.
- `rake pins:check` runs the wiring check ([experiment](../experiments/2026-11-15-pin-mapping.md)).
