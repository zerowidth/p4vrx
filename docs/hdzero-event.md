# HDZero at the event: capture and decode

How to collect HDZero I/Q with the p4vrx receiver and see what it decodes to, on the Mac, within a minute or so of a pilot powering up.

## Before

- The Nano runs `fm_top` build 3 or later from its flash (`nano read 0 3` gives `50 56 03`), and `selftest` says ok.
- `hdz` prints `hdzero=0 score=1.1-1.4` on analog or an empty channel.
- The hdzero-captures checkout sits beside this repo (`../hdzero-captures`), or `HDZERO_CAPTURES` points at it.

## Capture

1. Tune the P4 to the HDZero pilot's channel (picker, or `decode on R8`).
2. Run the watcher on the Mac:

   ```bash
   uv run --with pyserial python tools/hdzwatch.py --decode
   ```

3. When the P4 sees HDZero it prints `event hdzero=1 score=… ch=…`. The watcher then grabs 4M samples (100 ms, about 6 frames) every 15 s, up to 4 per detection. Each grab pauses the analog decoder for about 0.1 s; the dump takes about 35 s.
4. Files land in `captures/hdz/` as `iq-CH-TIME.bin` and `.json`, in hdzero-captures' format. With `--decode`, each is decoded straight away into `iq-CH-TIME-frames/`.

The watcher alternates the C5's receive filter (`--filter ab`): narrow is analog's 802.11p setting, about ±6 MHz; wide turns it off for the grab only, about twice the width, which should recover HDZero's outer bins ([the filter test](experiments/2026-11-18-c5-wide-filter.md)). Each capture's `.json` and `hdzdecode.py`'s line name the filter. `--filter narrow` or `wide` fixes it.

Without detection (a weak signal, or to grab regardless): `--now` takes one capture and exits. `--samples 8000000` takes 200 ms. To test the chain without a transmitter, `--fake 40` makes the P4 report HDZero for 40 s.

## Decode

```bash
uv run python tools/hdzdecode.py captures/hdz
```

One line per capture: CP strength (HDZero reads 3-7x, analog and noise about 1), carrier offset, frames, how many had special symbols, and the concealed-record median. PNG frames and the full log are in each `-frames/` folder. On the 2026-11-04 captures: 191225 and 191250 decode with 11 frames and specials in each; 191237 partly; 191206 syncs but its bits don't decode (hdzrx's float32 running sum in cp_sync, see [the gap list](hdzero-gaps.md); the C decoder decodes it).

## Decode on the P4

- `hdz decode` grabs 1.4M samples (35 ms) and decodes one frame on the P4 in about 5 s, with the analog decoder paused, and holds the frame on the LCD until `hdz show off`. It prints `hdz frame {...}` and `ok hdz run frames=N cp=… cfo=…`.
- To check the P4 against hdzrx on what it just saw: `uv run --with pyserial --with-requirements ../hdzero-captures/requirements.txt python tools/hdzrun.py --grab`. It grabs, fetches the I/Q, runs it on the P4 with dumps, and prints the stage-by-stage compare and the first stage that differs.
- On a capture already on the Mac: `tools/hdzrun.py captures/hdz/iq-CH-TIME` uploads it and does the same. `decode on` reuses the capture buffer, so upload after tuning.

## What to look for

- CP over 3x but no special symbols: timing and CFO work, the bits don't. Compare with 191206.
- Concealed records in the thousands: noisy band edges. Compare narrow and wide captures of the same pilot; also try another gain (`decode gain N`, then `--now`).
- If wide wins: `hdz decode wide=1` on the P4.
- A clipped capture: the `.json`'s gain over about 65 on a strong signal.
