# Wiring

Breadboard build: C5-Zero and Tang Nano 20K side by side on one breadboard, the
P4 board connected to a 40-pin T-breakout on the same breadboard by a ribbon
cable, with a 2x20 male pin header between the P4 and the cable. That way each
breakout pin matches the same P4 header pin. Plugging the P4 straight onto the
breakout mirrors the pinout, and the breakout's GND/3V3/5V ties then short P4
GPIOs. Go by the P4's silkscreen, not the breakout's Raspberry Pi labels.

## P4 header

All 20 usable GPIOs are assigned. 24/25 (the header's USB D-/D+) and 37/38 (the
P4 console on the CH343P) stay unused.

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
| 52 | IRQ / frame valid | Nano 79 | Optional; Nano 79 is the WS2812 pin |
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
