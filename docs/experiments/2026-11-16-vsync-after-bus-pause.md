# 2026-11-16: vertical sync after bus pauses and noise

## Question

At a weak signal (gain 67-76, tip noise 130-610 kHz) p4vrx looked more jittery than p4usb, as if losing vertical sync. Why, and can the decoder hold the picture still?

## Setup

- Commit: the one that adds this note, on top of 5021f7e2
- Bitstream: `passthru_lockout_top` from the Nano's flash
- Boards: p4vrx (C5, Nano, P4) on R4 with the C5's external antenna; p4usb as the reference, read only through UVC

## Method

- `decode events` logs the last 64 sync events (vertical corrections, ignored broad pulses, line clock re-acquisitions, parity changes, table reloads) with the line count and the pulse's offset from the line clock.
- `decode nodes` times the PARLIO DMA node interrupts and counts gaps longer than two nodes (302 us).
- 10-60 s lossless clips from both receivers (`camgrab ... -c:v ffv1`), scored by the vertical shift of each frame's row profile against the clip's median frame.
- One desense check: I/Q RMS on an empty channel (R8, gain 70) at C5 lane drive strengths 0, 2 and 3.

## Results

- Every C5 request turns the Nano's bus off (`c5 pause on`). Each one stopped the samples for 3.7-5.2 ms, and up to 26 ms while the gain control hunted, which is 6-14 stops per 30 s. A ping, a status query and an unknown command all did it.
- The line count then lagged by the lost lines, the next vertical sync fell outside the ±6-line window and was ignored for three fields, and a vertical correction followed. In the first log, 6 of 6 re-acquisitions after a stop were followed by 3-4 fields drawn 50-100 lines off.
- With `c5 pause off`, 200 of 200 pings and gain steps passed while decoding, with no bus stops and the sample rate at +4.9 ppm. But three `decode on` starts in a row then failed with lost C5 replies, so the pause stays on.
- Treating a node gap as lost lines (cut the field and believe the next vertical sync anywhere) removed those rolls: over two minutes with a C5 query every 10 s, every stop was followed by a clean re-acquisition, with no ignored vsyncs and no corrections.
- Noise was the remaining cause. A lone stretch of noise at least 18 us below the slicer, usually running into an hsync, was taken as a vertical sync inside the window, and the picture moved for four fields. A vsync now needs a second broad pulse half a line later, or a line later when the detector is skipping to each hsync. Without the line-later case, a wrong count never recovered: one clip stayed rolled for 2.4 s. It also needs the first pulse to start within 3 us of a line's start or middle. In one minute at 522 kHz tip noise, 545 broad pulses failed that test.
- Frames shifted by more than 1 px, p4vrx against p4usb over the same seconds: 0.2% vs 16.5% (20 s), 1.5% vs 1.6% (60 s), 2.2% vs 5.0% (60 s, tip noise 522 kHz). Conditions changed between clips (tip noise 126-522 kHz), so only same-clip pairs compare.
- Desense: I/Q RMS on R8 was 3 at drive strengths 0, 2 and 3, with no difference at that resolution.

## Verdict

Adopted. The bus pause stays on, and the decoder now treats a bus stop as lost lines and confirms vertical sync with a second broad pulse at a line's start or middle. p4vrx is now no less vertically stable than p4usb in the same conditions. The lane drive strength showed no desense.

## Next

- Each gain step still costs a cut field. The gain control could step less often on a weak signal, or the UART could move off the bus wiring on the PCB so the pause can go.
- After autostart the status shows `R4 (0 MHz)`: the tune reply's frequency was not parsed. This is cosmetic.
