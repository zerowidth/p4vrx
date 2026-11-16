# FPGA toolchain

The Tang Nano 20K (GW2AR-LV18QN88C8/I7, family GW2A-18C) builds with the open toolchain from the YosysHQ OSS CAD Suite: yosys (`synth_gowin`), nextpnr-himbaechel (gowin uarch), apicula's `gowin_pack` and openFPGALoader. No Gowin IDE.

## Install

`mise install` in the repo installs the suite. `mise.toml` pins the release as `github:YosysHQ/oss-cad-suite-build` with `bin_path = "oss-cad-suite/bin"`, since the archive keeps its top-level directory. Homebrew has yosys and openFPGALoader but neither nextpnr-himbaechel nor apicula, so it can't supply the toolchain.

The tasks use the tools on `PATH` when mise is active, else ask `mise which`.

Release 2026-11-14 ships yosys 0.71+34, nextpnr-0.11.1-92-g5e2718b6, apycula 0.36.dev39 and openFPGALoader v1.2.0. gowin_pack warns that numpy, msgspec and fastcrc are missing; the warnings are harmless.

## Tasks

- `rake fpga:build` synthesises, places, routes and packs `fpga/build/<TOP>.fs`. `TOP` defaults to `safe_top`; sources are `fpga/boards/tangnano20k/*.v` and `fpga/rtl/**/*.v`. Logs and the nextpnr JSON report land in `fpga/build/`. The timing target is 27 MHz.
- `rake fpga:load` writes the bitstream to SRAM. It is lost at power off.
- `rake fpga:flash` writes it to the Nano's flash.
- `rake fpga:detect` checks the JTAG chain.

openFPGALoader sometimes hangs at "Erase SRAM" (seen twice on 2026-11-15, after several loads). The load tasks give up after 120 s. `openFPGALoader -b tangnano20k -r` reconfigures the FPGA from flash, which clears it, and the next SRAM load works.

The Nano's onboard BL616 enumerates as an FT2232 (USB 0403:6010, product "USB Debugger", manufacturer SIPEED), not 359f. `openFPGALoader -b tangnano20k` finds it by that ID.

## Safe bitstream

`fpga/boards/tangnano20k/safe_top.v` with `safe_top.cst` is the breadboard-safe image. Every pin wired to the C5 or P4 (per [wiring.md](../wiring.md)) is an explicit input buffer with `PULL_MODE=NONE`, so it stays high-Z. Pin 51 is driven low. A heartbeat counter on the 27 MHz clock (pin 4) stays internal. The input buffers carry `(* keep *)` so yosys doesn't drop the unused inputs and leave those pins to the packer's default.

First build (2026-11-15): 35/384 IOB, 27 LUT4, 26 ALU, 25 DFF; 439 MHz max on the 27 MHz clock. SRAM load succeeded. On 2026-11-15 `safe_top` was also written to the Nano's flash, so the Nano boots safe after a power cycle.

## Other tops

- `pintest_top` reads every wired pin and drives P4-facing pins on command, over the BL616 UART (pins 69/70), for `rake pins:check`.
- `passthru_top` forwards the C5 lanes to the P4 bus, enabled by the P4 on the IRQ wire. Load it only after the P4 has booted.

Build or load any of them with `TOP=<name>`.
