# Startup self-check (sketch, not built)

At power-up, before decoding starts, the P4 checks every wire and chip it depends on, then shows what failed. The same checks run on demand with `selftest`. The goal is to tell a loose ribbon, an unpowered C5 or the wrong bitstream apart in a second, rather than finding out from a black picture.

## Checks

They run in this order; each stops on its first failure and later checks that depend on it are marked skipped. The whole run should take under 500 ms after c5rx is up.

| Check | How | Pass | Needs from the FPGA |
| --- | --- | --- | --- |
| C5 alive | `ping` with the check byte, 3 tries of 300 ms, after `c5_run_c5rx` | `ok c5rx` | nothing |
| C5 EN and BOOT | Read P4 34 and 35 with pull-downs while c5rx runs, as `c5 pins` does | both 1 (the C5's pull-ups) | nothing |
| FPGA present | I2C read of an ID register at an address clear of the GT911 (0x5D/0x14) | magic and a known build | an I2C target and register file |
| Bus enable (IRQ wire) | Bus off: the 15 bus pins follow P4 pull-ups then pull-downs. Bus on with I/Q running: they don't | undriven when off, driven when on | nothing (the lockout bitstream already floats them, `pins float`) |
| PCLK running | Time a short PARLIO capture, as `iq` does for the bus clock | 13.33 MHz within 0.5% | nothing; a clock-count register would say whether the C5's clock reaches the Nano |
| Lanes, C5 to P4 | c5rx disconnects the modem from the 14 lanes and drives walking ones, then walking zeros, as GPIO with its clock running; the P4 reads each pattern | 28 patterns match | nothing |
| Lanes, Nano to P4 | The Nano drives the same patterns itself | 28 patterns match | a pattern source on the lanes, chosen over I2C |

Comparing the last two places a fault on the C5-to-Nano hop or the Nano-to-P4 hop. A failing pattern names the lanes: one stuck bit is an open or a short to a rail, two bits that always agree are shorted together, two that trade places are swapped. `tools/c5walk.py` and `firmware/c5` pin walk already do the C5-to-Nano half offline with `pintest_top`; this brings it into c5rx.

## Results

- At boot, a failure stops autostart, and the LCD shows the failing check with its detail (for example "lanes: D3 open, D7 and D8 shorted"). A pass shows nothing.
- `selftest` stops decoding, reruns everything, prints one line per check (`ok`, `fail <detail>` or `skipped`), and restarts decoding if it was running.
- `info` adds one line: the last run's time and its failures, or "self-check ok".

## FPGA side

The current bitstream, `passthru_lockout_top`, has none of this. Its I2C pins (Nano 52 SCL, 53 SDA, shared with the GT911 on P4 7/8) are bare input buffers, kept only so the pins aren't left floating. The IRQ wire (P4 52 to Nano 79) is the bus enable, driven by the P4. There is no register file in `fpga/rtl/` yet; the README lists one as planned.

It would need:

- An I2C target on the 27 MHz clock, SDA open-drain, at a free 7-bit address such as 0x42. The GT911 bus already has pull-ups.
- A small register file:
  - ID: a magic byte and a build stamp, so the P4 knows which bitstream is loaded.
  - Status: PLL lock, the bus enable as the Nano sees it, and whether the C5's clock edged in the last millisecond.
  - Counts: C5 clock edges per millisecond (about 13 333) and edges the lockout refused.
  - Lane source: the C5, or a 14-bit pattern register, with PCLK from the C5's clock or the 27 MHz clock halved.
- The bus enable on Nano 79 stays as it is. If that wire later becomes the Nano's interrupt to the P4, a register that pulses it covers the IRQ check.

## Open questions

- Whether a failing check should still let decoding start (for example, one lane open costs a bit of the picture but still gives video).
- Whether c5rx should report its own build, so the P4 can flag a C5 running older firmware.
