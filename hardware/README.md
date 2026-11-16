# hardware

KiCad project for the carrier PCB: low-profile headers for the P4 board, C5 and
Nano, plus solder jumpers for a no-FPGA build that wires the C5 straight to the
P4.

The P4 header's Pi pins 26 and 28 (J3 25 and 27) carry the high-speed USB pair (USBD_N, USBD_P). Leave them unconnected on the carrier, with no trace or pad stub; see [../docs/wiring.md](../docs/wiring.md).
