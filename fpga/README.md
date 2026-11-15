# fpga

Gateware for the Tang Nano 20K (GW2AR-18).

- `rtl/`: one module per block (pass-through, link test, register file, FM demod,
  CVBS decode, HDZero PHY, snapshot ring).
- `sim/`: Verilator and cocotb benches, fed with real captures and checked against
  `../models`.
- `boards/tangnano20k/`: pin constraints, PLL, top level.

Nano pin 51 (speaker amp enable) is held low in every bitstream.
