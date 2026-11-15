# p4vrx

An FPV video receiver: an ESP32-C5-Zero (RF front end, I/Q out) feeds a Sipeed
Tang Nano 20K (DSP), which feeds a Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
(screen, touch UI, USB webcam, DVR). Goal: analog and HDZero decode with
automatic switching, 60 fps UVC and LCD video.

## Layout

- `firmware/p4`, `firmware/c5`, `firmware/c6`: ESP-IDF apps, one per chip.
- `fpga/`: RTL, simulation benches, board constraints for the Tang Nano 20K.
- `models/`: Python fixed-point models of the FPGA blocks.
- `tools/`: host-side Python (CLI, capture, compare, plots).
- `reference/`: read-only extracts of p4usb and c5rx. See `reference/PROVENANCE.md`.
- `docs/`: knowledge. Start at `docs/INDEX.md`.
- `enclosure/`, `hardware/`: 3D design files and KiCad.
- `captures/`: large data, not committed.

## Before you work

1. Read `docs/INDEX.md` for the current truth on the area you're touching.
2. Grep `docs/ledger.md` for the area. If something was already tried, read its
   experiment note before trying it again, and say why this attempt differs.

## After an experiment

Every experiment, including the ones that fail, gets:

1. A note in `docs/experiments/YYYY-MM-DD-slug.md`, from `docs/experiments/TEMPLATE.md`.
2. One line in `docs/ledger.md`, appended at the bottom.
3. An update to the topic in `docs/topics/` if it changes what we believe.

All three go in the same commit as the code. A current hardware result beats an
old assumption. Record numbers with units, not adjectives.

## Hardware rules

- Find boards by USB ID and serial number (`tasks/ports.rb`), never by port
  order. Only touch the boards you were told to use: other sessions may be
  debugging a p4usb, S3 or C5 on the same Mac.
- Web UI tests run against a server with fake video and fake RSSI, never a real
  board.
- Hold the Nano's pin 51 (speaker amp enable) low in every bitstream.

## Git

- Commit locally. Never push, and never open PRs asked.

## Provenance and licence

- New work is MIT (`LICENSE`).
- Anything copied or ported from C5VRX stays GPL-3.0-only
  (`LICENSES/GPL-3.0.txt`) and keeps its notices. Give each such file an
  `SPDX-License-Identifier: GPL-3.0-only` header and, when porting from
  `reference/`, name the source file in the commit message.
- Every new source file gets an SPDX header, so the split stays visible.
- `reference/` is never edited. Port pieces out of it.
- Keep the C5VRX credit in `README.md`.

## Running things

`rake -T` lists the tasks. `mise install` sets up the tool versions. ESP-IDF
lives under `~/.espressif`, at the version `mise.toml` names.
