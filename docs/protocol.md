# Control protocol

Text lines on the P4's first CDC interface, in the same shape as p4usb: one
command per line, plain-line replies, `key=value` for machine-readable lines.
Every p4usb command keeps its name and output, so the lap-timer server and
existing scripts work against either board. Binary data (I/Q dumps, snapshots,
replay uploads, screenshots, OTA images) goes over a second CDC interface.

The `video` counters stay cumulative 32-bit values, and the lap-timer's regex
`(\d+) start\(s\), (\d+) sent, (\d+) skipped, (\d+) too large` must keep
matching.

| Command | Status | What it does |
| --- | --- | --- |
| `help`, `info`, `reboot [download]` | From p4usb | `info` adds FPGA ID and bitstream version |
| `status` | Extended | e.g. `status ch=R1 freq=5658 board=p4vrx mode=analog lock=1 usb=streaming dvr=off` |
| `channel`, `antenna` | From p4usb | |
| `decode`, `video`, `iq`, `census`, `wires`, `link`, `c5` | From p4usb | `link fpga` tests each hop alone |
| `mode auto\|analog\|hdzero` | Planned | Force or auto-select the decoder; reports detector scores |
| `scan [band\|all]` | Planned | Spectrum sweep, one `scan mhz=5658 level=-62` line per bin |
| `fpga` | Planned | ID, version, counters; `reg R [V]`, `tap raw\|demod\|slots`, `snap` |
| `ui` | Planned | `tap X Y`, `swipe`, `shot` (LCD frame as PNG on the data CDC) |
| `dvr start\|stop\|status`, `sd` | Planned | DVR control, card info |
| `backpack` | Planned | C6 link state, last MSP message |
| `ota` | Planned | App image over the data CDC, rollback on failed boot |
