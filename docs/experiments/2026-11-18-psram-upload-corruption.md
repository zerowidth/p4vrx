# 2026-11-18: uploads corrupted while the analog decoder runs

## Question

Why did the same capture, uploaded to the P4 and decoded, give different results from one upload to the next (916, 872, 839 or 1,310 concealed records on 191225), when repeated `hdz run`s on one upload agreed?

## Setup

- Commit: this one. P4 firmware with `hdz upload` replying `crc=` (CRC-32 of the bytes received) and `iq grab` replying the CRC of the capture, taken before the decoder resumes.
- Boards: the pinned p4vrx P4, C5 and Nano, decoding analog R4 unless stated.

## Method

- Uploaded 191225's first 1.4M samples (2.8 MB) repeatedly and compared the device's CRC with the Mac's.
- Read one corrupted upload back with `iq dump` and compared it word for word.
- Repeated the uploads with `decode off`, then `decode on R4`.
- Grabbed 4M samples three times without holding the decoder, dumped them while it ran, and checked the dump against the grab's CRC.

## Results

- With the analog decoder running, 7 of 9 uploads had a wrong CRC, with the right byte count.
- Read back, one had 656 wrong words in 6 runs of 27-128 words (up to 256 bytes) at scattered places (words 355,649 to 1,253,247).
- Reading only from USB (not the UART first) changed nothing.
- With `decode off`, 4 of 4 uploads were right; after `decode on R4`, 2 of 2 were wrong again.
- Grabs (written by DMA, then invalidated) dumped while the decoder ran: 3 of 3 right.
- With the decoder paused through upload and run, all four Nov 4 captures decoded on the P4 match hdzrx at every stage, three times in a row for 191225.

## Verdict

Something in the running analog decoder loses or overwrites CPU-written data in the PSRAM capture buffer in blocks of up to 256 bytes. DMA-written captures are not affected, so it looks like cache lines being dropped rather than written back. The decoder's M2M copy DMAs and their cache syncs (`decode.c`) are the first suspects; not found yet. Worked around: `hdz upload` and `hdz run` pause the analog decoder until done, `hdzwatch.py` grabs with `hold`, checks the CRC, and sends `iq resume` after the dump. The earlier "P4 differs from the Mac on 1% of 191250" was this.

## Next

Find the cause after the event: it may also touch other CPU-written PSRAM while decoding (the encoder's input, the LCD's still, UI canvases). Write a known pattern into a PSRAM buffer with the decoder running, read it back over seconds, and bisect by turning off the strip and old-field DMAs.
