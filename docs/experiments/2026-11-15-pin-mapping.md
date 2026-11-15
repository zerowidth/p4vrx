# 2026-11-15: pin mapping check of the wired breadboard

## Question

Is every wire in [wiring.md](../wiring.md) connected to the right pin at both ends, with no opens, swaps or shorts?

## Setup

- Commit: the one adding this note (`firmware/p4`, `firmware/c5-pinwalk`, `fpga/boards/tangnano20k/pintest_top.v`, `tools/pinmap.py`, `tools/c5walk.py`)
- Bitstream: `pintest_top`, SRAM-loaded, then `safe_top` restored
- Boards: P4 rev v3.2 (CH343 serial 5C94101919), C5 rev v1.0 (MAC 3c:dc:75:8a:f8:fc), Nano 20K (BL616 serial 2025030317)
- Wiring changes from `../wiring.md`: none. The C5 is powered from its own USB, which doesn't enumerate while 13/14 are wired.

## Method

`rake c5:pinwalk` builds the C5 test firmware and flashes it through the P4 bridge, then `rake pins:check`.

- C5 to P4 (UART, EN, BOOT): `rake c5:chip_id` through the bridge. It only answers if all four work.
- P4 and Nano (PCLK, D13..D0, IRQ): the P4 drives one pin high, with every other P4 and Nano pin pulled down, and the Nano reports what it reads over its BL616 UART. Then the reverse: the Nano drives and the P4 reads. The P4 also reads its own other pins each time.
- I2C (SCL, SDA): the same, driving low only, against the board's pull-ups.
- C5 to Nano (clock, Q7..Q1, I7..I1): the C5 drives each pin high, then low, 250 ms each, with the rest pulled down, and logs each phase over its UART (relayed by `c5 log on`). A wire is ok when only its own Nano bit changes between the two phases.

## Results

- C5 to P4: chip-id answered, ESP32-C5 rev v1.0, 48 MHz crystal, at 115200 baud over the CH343 path. The bridge's change to 921600 failed on that path.
- P4 and Nano: PCLK and D13..D0 ok both ways, no shorts. IRQ (P4 52 / Nano 79), SCL (8 / 53) and SDA (7 / 52) open both ways.
- C5 to Nano: all 15 ok, no shorts.
- First run: D2 and D1 (P4 GPIO 46, 47) failed P4 to Nano only. GPIO 39-48 are powered by the P4's LDO channel 4, which nothing had enabled; with it at 3.3 V they pass. The Nano had also driven 3.3 V into that unpowered bank during the reverse test.
- First C5 run: Q4 and I6 read high whatever the C5 did. Nano 85 and 80 have pull-ups on the Nano board, so a walking one can't see them; the high/low comparison can, and both pass.

### Rerun with IRQ and I2C wired

Wired IRQ (P4 52) and the I2C pair (P4 7/8). All 18 P4 and Nano wires pass both ways, and all 15 C5 lanes pass, with no shorts. `safe_top` is now in the Nano's flash too, so it comes up safe after a power cycle.

## Verdict

Every wire in `wiring.md` is connected to the right pin at both ends.

## Next

- The P4 USB console stalls after a few replies (see [p4-firmware.md](../topics/p4-firmware.md)), so the check runs over the CH343.
