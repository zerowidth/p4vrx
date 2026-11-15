# firmware/c6

Optional ESP32-C6 firmware: an ELRS VRX backpack that receives DVR Rec (and
possibly channel changes) over ESP-NOW and tells the P4. Planned to start from
`lap-timer/firmware/main/backpack.c` and `msp.c`. The first flash needs a USB-UART
adapter on the board's unpopulated P1 pads.
