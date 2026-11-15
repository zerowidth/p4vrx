# models

Python fixed-point models, one per FPGA block. Each is checked against a float
reference (hdzero-reference, the p4usb decoder), and the RTL in `../fpga/sim`
must match its model bit for bit.
