# Startup self-check

Built: `firmware/p4/main/selftest.c`, c5rx `walk`, and the Nano's `lockout_regs_top`. On the p4vrx boards every check passes at power-up in 437 ms. A faked open D8 stopped autostart and put "SELF-CHECK FAILED / D8 open" at the top of the LCD's test pattern. Twice, the first run after flashing the P4 saw D10 driven with the bus off and later runs passed; the bus-off check now takes a second look 20 ms on, and `info` says when it needed one.

At power-up, before decoding starts, the P4 checks every wire and chip it depends on, then shows what failed. The same checks run on demand with `selftest`. The goal is to tell a loose ribbon, an unpowered C5 or the wrong bitstream apart in a second, rather than finding out from a black picture.

## Checks

They run in this order; each stops on its first failure and later checks that depend on it are marked skipped. The whole run should take under 500 ms after c5rx is up.

| Check | How | Pass | Needs from the FPGA |
| --- | --- | --- | --- |
| C5 alive | `ping` with the check byte, 3 tries of 300 ms, after `c5_run_c5rx` | `ok c5rx` | nothing |
| C5 EN and BOOT | Read P4 34 and 35 with pull-downs while c5rx runs, as `c5 pins` does | both 1 (the C5's pull-ups) | nothing |
| Nano present | I2C read of registers 0-3 at 0x42, clear of the GT911 (0x5D/0x14) | "PV", build 1, PLL locked | the register file |
| Bus enable (IRQ wire) | Bus off: the 14 lanes follow P4 pull-ups then pull-downs (PCLK always reads low from the P4, so it is left out). Bus on with I/Q running: they don't | undriven when off, driven when on | nothing (the lockout bitstream already floats them, `pins float`) |
| PCLK running | PCNT counts PCLK for 1 ms with the bus on and the walk's clock running | 12-14.7 MHz | on failure, the Nano's per-ms counts of C5 clock edges taken and seen say whether the clock reached it |
| Lanes, C5 to P4 | c5rx disconnects the modem from the 14 lanes and drives walking ones, then walking zeros, as GPIO with its clock running; the P4 reads each pattern | 28 patterns match | nothing |
| Lanes, Nano to P4 | The Nano drives the same patterns from its pattern register, with PCLK from its own 27 MHz clock halved; runs even when the C5 checks fail | 28 patterns match | the pattern source |

Comparing the last two places a fault on the C5-to-Nano hop or the Nano-to-P4 hop. A failing pattern names the lanes: one stuck bit is an open or a short to a rail, two bits that always agree are shorted together, two that trade places are swapped. `tools/c5walk.py` and `firmware/c5` pin walk already do the C5-to-Nano half offline with `pintest_top`; this brings it into c5rx.

## Results

- At boot, a failure stops autostart, and the LCD shows the failing check with its detail (for example "lanes: D3 open, D7 and D8 shorted"). A pass shows nothing.
- `selftest` stops decoding, reruns everything, prints one line per check (`ok`, `fail <detail>` or `skipped`), and restarts decoding if it was running.
- `info` adds one line: the last run's time and its failures, or "self-check ok".

## FPGA side

`lockout_regs_top` is `passthru_lockout_top` with an I2C target at 0x42 (`fpga/rtl/i2c_target.v`, open-drain SDA on the touch bus's pull-ups). A write sets the register pointer and writes from it; a read returns registers from it; both step it per byte.

| Register | Contents |
| --- | --- |
| 0x00-0x01 | "PV" |
| 0x02 | build, 1 |
| 0x03 | status: bit 0 PLL lock, 1 bus enable as the Nano sees it, 2 C5 clock taken in the last ms, 3 lanes from the pattern |
| 0x04-0x05 | C5 clock edges taken per ms, low byte first; reading the low byte holds the high byte |
| 0x06-0x07 | C5 clock edges seen per ms, the same way; seen less taken is what the lockout refused |
| 0x08 | control: bit 0 lanes from the pattern, bit 1 PCLK from the 27 MHz clock halved |
| 0x09-0x0a | lane pattern, D7..D0 then D13..D8 |

The bus is still driven only while the P4 holds IRQ high.

## Open questions

- Whether a failing check should still let decoding start (for example, one lane open costs a bit of the picture but still gives video).
- Whether c5rx should report its own build, so the P4 can flag a C5 running older firmware.
