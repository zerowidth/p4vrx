# p4vrx

A 5.8 GHz FPV video receiver with a touchscreen, built from three boards:

- **ESP32-C5-Zero** (Waveshare) tunes and streams 7+7-bit I/Q at 40 MS/s with a sample clock.
- **Tang Nano 20K** (Sipeed) starts as a pass-through, then takes over FM demodulation, analog decode and the HDZero physical layer.
- **ESP32-P4-WIFI6-Touch-LCD-4.3** (Waveshare) shows the video and the touch UI, acts as a 60 fps USB webcam, and records DVR to microSD.

Status: planning and bench bring-up. Nothing here is usable yet.

## Layout

| Folder                                      | Contents                                                                    |
| ------------------------------------------- | --------------------------------------------------------------------------- |
| `firmware/p4`, `firmware/c5`, `firmware/c6` | ESP-IDF firmware for each chip                                              |
| `fpga/`                                     | RTL, simulation, Tang Nano 20K constraints                                  |
| `models/`                                   | Python fixed-point models of the FPGA blocks                                |
| `tools/`                                    | Host tools: CLI, capture, compare                                           |
| `reference/`                                | Read-only extracts of the p4usb and c5rx firmware this project starts from  |
| `docs/`                                     | Findings, ledger, wiring, protocol. Start at [docs/INDEX.md](docs/INDEX.md) |
| `enclosure/`, `hardware/`                   | 3D-printed case, KiCad board (TK)                                           |

## Setup

```sh
mise install   # Python 3.12, Ruby, uv
rake -T        # tasks
```

### ESP-IDF

ESP-IDF lives outside the repo under `~/.espressif`, managed by EIM. The version is `IDF_VERSION` in `mise.toml` (v6.1), and `tasks/ports.rb` looks for `~/.espressif/tools/activate_idf_v6.1.sh`.

```sh
brew install eim
mise exec -- eim install # run from this repo to get the right python version
```

## Licence and credit

New work in this repository is under the MIT licence ([LICENSE](LICENSE)).

This project incorporates and adapts the C5 receive firmware (c5rx) and the P4 receiver firmware (p4usb), and builds on research findings, from [C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX contributors. Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/) for the original project and its Discord invite. Files derived from C5VRX remain under GPL-3.0-only ([LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)) and have SPDX headers saying so. Firmware images that include them are GPL-3.0 as a whole. See [reference/PROVENANCE.md](reference/PROVENANCE.md) for sources and revisions. This is not an official C5VRX release.
