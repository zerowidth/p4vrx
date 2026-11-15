# Provenance of reference/

SPDX-License-Identifier: GPL-3.0-only

`p4usb/` and `c5rx/` are read-only extracts of firmware from a fork of
[C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX contributors.
They are GPL-3.0-only ([../LICENSES/GPL-3.0.txt](../LICENSES/GPL-3.0.txt)). See
the [official C5VRX website](https://twotoz.github.io/C5VRX/) for the original
project and its Discord invite.

Don't edit these files. Port pieces into `firmware/` and name the source file
in the commit message. `rake ref:build` checks they still build.

## Source

| | |
| --- | --- |
| Repository | `zerowidth/C5VRX`, fork of `Twotoz/C5VRX` |
| Pushed so far | [zerowidth/C5VRX PR #3](https://github.com/zerowidth/C5VRX/pull/3), branch `p4usb` at `3dfac08` |
| Extracted from | `bcf7743e89f16b8cdd71cd01bdd4d11028298e5d` (2026-11-07, "p4usb: decode straight into the encoder's 4:2:0, and drop the 4:2:2 frame"), 29 commits past PR #3, not yet pushed. It carries the H.264 UVC work |
| Paths | `p4usb/`, `c5rx/`, `main/menu_font.h` |
| Upstream base | `Twotoz/C5VRX` `main` at `c2f22be` |

## Changes from the source

- `main/menu_font.h` (from C5VRX's main firmware) copied into `p4usb/main/`, and
  `PRIV_INCLUDE_DIRS "../../main"` removed from `p4usb/main/CMakeLists.txt`, so
  `p4usb/` builds on its own.
- Dropped `p4usb/sdkconfig 2` (a stray copy) and the analysis scripts
  `p4usb/tools/big_font.py`, `iq.py`, `phase_table_sim.py` and
  `table_compare.py`. Kept `p4usb/tools/iqcap.py`, which saves raw I/Q
  captures from the P4.
- Not included: build output, `managed_components/` and `sdkconfig` (generated),
  C5VRX's Rakefile and mise tasks. Port discovery from that Rakefile is adapted in
  `../tasks/ports.rb`.

Nothing else was changed.
