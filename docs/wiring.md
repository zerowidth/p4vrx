# Wiring

Breadboard build: C5-Zero and Tang Nano 20K side by side on one breadboard, the
P4 board connected to a 40-pin T-breakout on the same breadboard by a ribbon
cable, with a 2x20 male pin header between the P4 and the cable. That way each
breakout pin matches the same P4 header pin. Plugging the P4 straight onto the
breakout mirrors the pinout, and the breakout's GND/3V3/5V ties then short P4
GPIOs. Go by the P4's silkscreen, not the breakout's Raspberry Pi labels.

## P4 header

All 20 usable GPIOs are assigned. 24/25 (the full-speed USB pair, Pi pins 22/24, J3 21/23) and 37/38 (the
P4 console on the CH343P) stay unused.

Pi pins 26 and 28 (J3 25 and 27) are USBD_N and USBD_P, the high-speed USB pair that also goes to the board's USB-C port. They must stop at the P4: carried down the ribbon and breakout, the stubs broke the 480 Mbit/s link, which stalled after a few replies or never enumerated. Leave those two ribbon conductors unconnected (pull the pins from the gender changer or cut the conductors). The PCB must not route them anywhere.

Waveshare's schematic (J3) numbers the header's columns opposite to the Raspberry Pi convention: each odd/even pair is swapped, so J3 pin n is Pi pin n+1 for odd n and n-1 for even n. In Pi numbering, which the breakout's silkscreen uses, the power pins sit where a Pi has them: GND on 6, 9, 14, 20, 25, 30, 34, 39, 3V3 on 1 and 17, 5V on 2 and 4. The mapping is checked physically only through the pin check (every GPIO below drives and reads at its far end) and the USB fix; the ground positions are pending a continuity check.

| Pi pins | J3 pins | Odd Pi pin | Even Pi pin |
| --- | --- | --- | --- |
| 1, 2 | 2, 1 | 3V3 | 5V |
| 3, 4 | 4, 3 | GPIO 7 (I2C SDA) | 5V |
| 5, 6 | 6, 5 | GPIO 8 (I2C SCL) | GND |
| 7, 8 | 8, 7 | GPIO 2 (D13) | GPIO 37 (console TX) |
| 9, 10 | 10, 9 | GND | GPIO 38 (console RX) |
| 11, 12 | 12, 11 | GPIO 3 (D12) | GPIO 5 (D9) |
| 13, 14 | 14, 13 | GPIO 4 (PCLK) | GND |
| 15, 16 | 16, 15 | GPIO 28 (D7) | GPIO 21 (D11) |
| 17, 18 | 18, 17 | 3V3 | GPIO 22 (D10) |
| 19, 20 | 20, 19 | GPIO 29 (D6) | GND |
| 21, 22 | 22, 21 | GPIO 30 (D5) | GPIO 24 (USB FS D-) |
| 23, 24 | 24, 23 | GPIO 31 (D4) | GPIO 25 (USB FS D+) |
| 25, 26 | 26, 25 | GND | USBD_N (USB HS D-) |
| 27, 28 | 28, 27 | GPIO 34 (C5 EN) | USBD_P (USB HS D+) |
| 29, 30 | 30, 29 | GPIO 35 (C5 BOOT) | GND |
| 31, 32 | 32, 31 | GPIO 49 (D8) | GPIO 32 (D3) |
| 33, 34 | 34, 33 | GPIO 50 (C5 UART RX) | GND |
| 35, 36 | 36, 35 | GPIO 51 (C5 UART TX) | GPIO 46 (D2) |
| 37, 38 | 38, 37 | GPIO 52 (IRQ) | GPIO 47 (D1) |
| 39, 40 | 40, 39 | GND | GPIO 48 (D0) |

| P4 GPIO | Signal | From / to | Notes |
| --- | --- | --- | --- |
| 4 | PCLK | Nano 26 | Next to a header GND |
| 2 | D13 | Nano 29 | |
| 3 | D12 | Nano 30 | |
| 21 | D11 | Nano 31 | |
| 22 | D10 | Nano 15 | Nano user LED |
| 5 | D9 | Nano 16 | Nano user LED |
| 49 | D8 | Nano 17 | Nano user LED |
| 28 | D7 | Nano 20 | Nano user LED |
| 29 | D6 | Nano 19 | Nano user LED |
| 30 | D5 | Nano 18 | Nano user LED |
| 31 | D4 | Nano 72 | |
| 32 | D3 | Nano 71 | |
| 46 | D2 | Nano 54 | |
| 47 | D1 | Nano 55 | |
| 48 | D0 | Nano 56 | |
| 52 | IRQ / frame valid | Nano 79 | Nano 79 is the WS2812 pin. For now the P4 drives it as the pass-through's bus enable |
| 50 | C5 UART RX | C5 12 | Direct |
| 51 | C5 UART TX | C5 11 | Direct |
| 35 | C5 BOOT, open-drain | C5 28 (back pad) | P4 strapping pin, held high by the C5. Verify |
| 34 | C5 EN, open-drain | C5 RESET pad | P4 strapping pin, held high by the C5's 10k. Verify. Never drive high |
| 8 | I2C SCL | Nano 53 | Shared with the GT911 touch controller |
| 7 | I2C SDA | Nano 52 | Shared with the GT911 touch controller |

P4 GPIO 39-48 (D2..D0) are powered by the P4's LDO channel 4, which the firmware sets to 3.3 V at boot; nothing may drive them before that.

In pass-through, D13..D7 carry Q7..Q1 and D6..D0 carry I7..I1 (to be confirmed
against the P4 firmware's PARLIO lane order when it's written).

## C5-Zero

| C5 pin | Signal | To |
| --- | --- | --- |
| 0 | Sample clock | Nano 77 (GCLK input) |
| 1, 2, 3, 4 | Q7, Q6, Q5, Q4 | Nano 73, 74, 75, 85 |
| 23, 24, 25 (back pads) | Q3, Q2, Q1 | Nano 27, 28, 25 |
| 6, 7, 8, 9, 10, 13, 14 | I7 .. I1 | Nano 76, 80, 42, 41, 48, 49, 86 |
| 11 | UART TX | P4 51 |
| 12 | UART RX | P4 50 |
| 28 (back pad) | BOOT | P4 35 |
| RESET pad (EN) | Reset | P4 34 |
| 5 | Spare | |
| 5V, GND | Power | P4 header 5V, ground rail |

The back pads and the RESET pad are wired underneath to a short header strip
that sits in the breadboard. EN is the RESET pad that reads ~10k to 3V3; the
other pad reads 0 ohm to GND. Add a second ground from the USB-C shell's front
tab.

C5 pins 13 and 14 are its USB D- and D+, so the C5's own USB port is unused while those lanes are wired; with them connected it doesn't enumerate. Flashing and the console go through the P4 over UART, with BOOT and EN. C5 firmware must disable USB-Serial-JTAG and claim 13/14 as GPIO. To recover the C5 over USB, pull the jumpers on 13 and 14 (Nano 49 and 86).

## Tang Nano 20K

- Pin 51 (speaker amp enable): held low in every bitstream.
- Pins 80 and 85 (C5 I6 and Q4) have pull-ups on the Nano board, so they read high when undriven.
- Its 3 GND pins and the HDMI connector shell go to the ground rails.
- Powered from its own USB-C while plugged into the Mac. Don't also feed its 5V
  pin from the P4. Grounds are always common.

## Rules

- C5 to Nano jumpers 1-3 cm: that is the 80 MHz hop.
- All 8 P4 header GNDs to the rails.
- Leave room for 33 ohm series resistors on the C5 clock and the Nano PCLK if
  the link test shows ringing.
