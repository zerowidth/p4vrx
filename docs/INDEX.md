# Knowledge index

Where the current truth on each subject lives. Topics are rewritten when an
experiment changes them; the ledger and experiment notes keep the history.

## This repo

| Subject | Start here |
| --- | --- |
| What has been tried, decided or ruled out | [ledger.md](ledger.md) |
| Breadboard wiring and pin budget | [wiring.md](wiring.md) |
| Text control protocol on the P4's console | [protocol.md](protocol.md) |
| Experiment write-ups | [experiments/](experiments/) ([template](experiments/TEMPLATE.md)) |
| Living topic docs | [topics/](topics/) |
| FPGA toolchain, rake tasks, safe bitstream | [topics/fpga-toolchain.md](topics/fpga-toolchain.md) |
| P4 firmware: console, C5 bridge, IO supply, wiring check | [topics/p4-firmware.md](topics/p4-firmware.md) |
| Panel UI, its fonts and the browser simulator | [topics/ui.md](topics/ui.md) |
| Startup self-check: what it checks, what the Nano needs | [self-check.md](self-check.md) |
| FM demod in the Nano: design, model results | [fpga-fm-demod.md](fpga-fm-demod.md) |
| HDZero detector (cyclic prefix): design, model results | [fpga-cp-detect.md](fpga-cp-detect.md) |
| HDZero at the event: capture and decode runbook | [hdzero-event.md](hdzero-event.md) |
| HDZero burst decoder in C (P4 and Mac), stage checks against hdzrx | [hdzero-decoder.md](hdzero-decoder.md) |
| Where hdzrx differs from hdzero-reference, and what to port | [hdzero-gaps.md](hdzero-gaps.md) |
| Nano resource budget: what fits, HDZero reserve, CVBS ceiling | [fpga-budget.md](fpga-budget.md) |
| CVBS decode in the Nano (phase 3b): design | [fpga-cvbs.md](fpga-cvbs.md) |
| Where the reference firmware came from | [../reference/PROVENANCE.md](../reference/PROVENANCE.md) |

## Elsewhere in `~/Documents/projects/fpv`

These predate this repo. Read them before redoing their work.

| Subject | Where |
| --- | --- |
| C5 + P4 receiver findings: I/Q lanes and levels, PARLIO and DMA pitfalls, P4 CPU and PSRAM costs, the demodulator | `C5VRX/docs/p4-receiver-findings.md` |
| C5 + P4 receiver plan: original pin plan, UVC, control protocol | `C5VRX/docs/p4-receiver-plan.md` |
| C5VRX's own knowledge index (RF, AGC, CVBS research) | `C5VRX/docs/KNOWLEDGE_INDEX.md` |
| HDZero decoding from C5 captures, findings, failures | `hdzero-captures/docs/decoding.md`, `hdzero-captures/docs/findings.md` |
| HDZero air protocol and reference receiver | `hdzero-reference/SPEC.md`, `hdzero-reference/hdzero_ref/` |
| Lap timer: recording, `status`/`video` parsing of the P4, ELRS backpack firmware | `lap-timer/docs/plan.md`, `lap-timer/firmware/main/backpack.c` |
| Waveshare board: schematic, BSP, examples | `ESP32-P4-WIFI6-Touch-LCD-4.3/` |
