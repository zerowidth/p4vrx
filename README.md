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
| `ui/`                                       | Panel UI in plain C, its fonts, and a browser simulator                     |
| `reference/`                                | Read-only extracts of the p4usb and c5rx firmware this project starts from  |
| `docs/`                                     | Findings, ledger, wiring, protocol. Start at [docs/INDEX.md](docs/INDEX.md) |
| `enclosure/`, `hardware/`                   | 3D-printed case, KiCad board (TK)                                           |

## Setup

```sh
mise install   # Python 3.12, Ruby, uv, OSS CAD Suite (FPGA tools)
rake -T        # tasks
```

Copy `mise.local.toml.example` to `mise.local.toml` and fill in the boards' USB serials (`rake ports` lists them): `P4VRX_P4_UART_SERIAL`, `P4VRX_P4_USB_SERIAL` and `P4VRX_NANO_SERIAL`. Rake and `tools/` then only open those boards, and refuse to guess when another P4 or Nano shares the USB ID.

`rake p4:flash` flashes the P4. The C5 flashes through the P4 (`rake c5:chip_id` checks the link); its own USB is unused. `rake pins:check` checks the breadboard wiring. See [docs/topics/p4-firmware.md](docs/topics/p4-firmware.md).

### UI simulator

`rake ui:dev` builds the panel UI to WebAssembly and serves it at http://127.0.0.1:8790/, rebuilding on every change. It needs two tools that aren't in `mise.toml`, and stops naming them if either is missing:

- Xcode's clang, `/usr/bin/clang` from the Command Line Tools (`xcode-select --install`), which compiles to `wasm32`.
- `rust-lld` from a Rust toolchain installed with rustup, run as `rust-lld -flavor wasm` (the same linker as `wasm-ld`). Rake finds it at `$(rustc --print sysroot)/lib/rustlib/<host>/bin/rust-lld` for the active toolchain.

The baked fonts (`ui/fonts/inter.c`) are committed, so building needs nothing more. Only `rake ui:fonts`, which rebakes them, needs Inter from the `font-inter` Homebrew cask (`brew install --cask font-inter`; `INTER_TTF=path` uses another copy) and FreeType's Python bindings, which `uv` fetches as `freetype-py`. See [ui/fonts/README.md](ui/fonts/README.md) and [docs/topics/ui.md](docs/topics/ui.md).

### ESP-IDF

ESP-IDF lives outside the repo under `~/.espressif`, managed by EIM. The version is `IDF_VERSION` in `mise.toml` (v6.1), and `tasks/ports.rb` looks for `~/.espressif/tools/activate_idf_v6.1.sh`.

```sh
brew install eim
mise exec -- eim install # run from this repo to get the right python version
```

## Licence and credit

New work in this repository is under the MIT licence ([LICENSE](LICENSE)).

Files without a licence header are MIT (see LICENSE); files adapted from C5VRX carry GPL-3.0-only headers.

This project incorporates and adapts the C5 receive firmware (c5rx) and the P4 receiver firmware (p4usb), and builds on research findings, from [C5VRX](https://github.com/Twotoz/C5VRX) by Twotoz and the C5VRX contributors. Visit the [official C5VRX website](https://twotoz.github.io/C5VRX/) for the original project and its Discord invite. Files derived from C5VRX remain under GPL-3.0-only ([LICENSES/GPL-3.0.txt](LICENSES/GPL-3.0.txt)) and have SPDX headers saying so. Firmware images that include them are GPL-3.0 as a whole. See [reference/PROVENANCE.md](reference/PROVENANCE.md) for sources and revisions. This is not an official C5VRX release.
