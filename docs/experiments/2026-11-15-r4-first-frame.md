# 2026-11-15: first frame from R4 through the Nano

## Question

Does analog video from a camera on R4 reach the P4 through the C5 and a Nano pass-through, clean enough to decode a picture?

## Setup

- Commit: the one adding this note (`firmware/c5`, `fpga/boards/tangnano20k/passthru_top.v`, `firmware/p4` with `iq` and `still`)
- Bitstream: `passthru_top`, SRAM-loaded; `safe_top` in flash
- Boards: as in [the pin mapping check](2026-11-15-pin-mapping.md); camera transmitting on R4
- Wiring changes from `../wiring.md`: the IRQ wire runs P4 to Nano as the bus enable for now

## Method

`rake c5:flash`, then `TOP=passthru_top rake fpga:load` with the P4 booted. Over the CH343 console: `c5 start`, `c5 send tune R4`, `c5 send iq on` (top 7 bits of I and Q, 40 MS/s), `pins bus on`, then `iq cap`. `tools/iqcap.py` saves a capture; `tools/still.py` runs `iq cap 1400000` and `still`, which FM-demodulates with atan2 on the P4, finds vsync and each hsync, and samples 320 pixels across each of 240 lines.

## Results

- R4 is 5769 MHz (c5rx's table). C5 status: RSSI -82 dBm at gain 52, internal antenna.
- Bus off: the capture times out (no PCLK). Bus on, 65,472 samples: I -127..63, Q -69..63, rms 58, 3 clipped. Toggles per 1000 samples, Q7..Q1: 74 77 219 409 522 498 502; I the same.
- Rising-edge sampling of PCLK (the inverted C5 clock) reads cleaner than falling: roughness 17 against 31.
- `captures/iq-R4-20261115-172254.u16` (131,072 samples), FM-demodulated on the Mac: autocorrelation peak at 2541 samples, 63.52 us, 15.742 kHz (NTSC), r = 0.957; carrier offset -0.7 MHz.
- `captures/still-R4-20261115-172626.png`: a clear 320x240 grey field (desk lamp, helping hands, USB cable), 240 of 240 lines locked to hsync, sync below -4067 of a -6701..6473 range.

## Verdict

Adopted: the C5 to Nano to P4 path carries clean NTSC video at 40 MS/s.

## Next

- Port p4usb's real-time decoder (demodulator, BitScrambler phase, colour) for continuous frames.
- UVC once the USB console works.
