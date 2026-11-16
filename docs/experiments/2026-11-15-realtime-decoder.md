# 2026-11-15: p4usb's real-time decoder through the Nano

## Question

Does p4usb's NTSC decoder (BitScrambler phase lookup, assembly demodulator, colour) run unchanged on the P4 with the C5's lanes arriving through the Nano pass-through?

## Setup

- Commit: the one adding this note (`firmware/p4` with `decode.c`, `iq.c`, `video.c`)
- Bitstream: `passthru_top` with the P4-side outputs at DRIVE=4
- Boards and camera: as in [the first frame](2026-11-15-r4-first-frame.md), R4

## Method

`decode on R4` over the CH343: `iq_start` resets the C5, tunes, probes at 40 MS/s (gain and clock placement), then switches to 1 in 6 (13.33 MS/s) for the decoder. `decode` reports lock and noise; `video` the frame rate. `tools/grab.py` sends one 720x480 packed 4:2:0 frame (about 60 s at 115200) and converts it to PNG.

## Results

- 59.9 fps steady, 251-254 of 262 hsyncs locked per field, colour on (burst 2800-4600 against an on threshold of 700). Demodulation takes 55% of core 1 and drawing 43% of core 0.
- `captures/frame-R4-20261115-181602.png`: sharp 720x480 colour, roughly right hues, with streaks on lines that held FM clicks.
- Quality varies from boot to boot. The C5's I/Q DC offset after reset was -3 to -4 in some sessions and -6 to -7 in others (a circle fit agrees with the mean, so it is real). The -7 sessions show 15,000-47,000 click lines in 4 s against 130-1,300, and sync-tip noise 300-750 kHz against 125-400.
- Raw captures at 13.33 MS/s are clean either way (line autocorrelation 0.975, phase jumps under 1 per 1000), so the Nano samples the C5 bus right.
- While the Nano drives the P4 bus, the P4 <-> C5 UART loses characters in both directions, intermittently. With the bus idle-on and no traffic the C5 is fine. DRIVE=4 and a push-pull P4 TX didn't stop it. The firmware now pauses the bus for each C5 exchange (a few ms; a retune about 1 s).
- openFPGALoader hung at "Erase SRAM" four times; reconfiguring from flash (`-r`) cleared it each time, and the load task now does that itself.

## Verdict

Adopted: the decoder runs at full rate through the Nano. Open: boot-to-boot noise, and the UART coupling.

## Next

- Wiring: power the C5 from the P4 header again (one ground path) and keep its UART jumpers away from the Nano's P4-side lanes, then drop the bus pause.
- Find why the C5's DC offset settles differently after some resets, or rebuild the phase table from the decoder's running estimate.
- UVC once the USB console works.
