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

Without detection (a weak signal, or to grab regardless): `--now` takes one capture and exits. `--samples 8000000` takes 200 ms. To test the chain without a transmitter, `--fake 40` makes the P4 report HDZero for 40 s.

## Decode

```bash
uv run python tools/hdzdecode.py captures/hdz
```

One line per capture: CP strength (HDZero reads 3-7x, analog and noise about 1), carrier offset, frames, how many had special symbols, and the concealed-record median. PNG frames and the full log are in each `-frames/` folder. On the 2026-11-04 captures: 191225 and 191250 decode with 11 frames and specials in each; 191237 partly; 191206 syncs but its bits don't decode.

## What to look for

- CP over 3x but no special symbols: timing and CFO work, the bits don't. Compare with 191206.
- Concealed records in the thousands: noisy band edges; try another gain (`decode gain N`, then `--now`).
- A clipped capture: the `.json`'s gain over about 65 on a strong signal.
