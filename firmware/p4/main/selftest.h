#pragma once

#include <stdbool.h>

/* Checks the C5, its EN and BOOT wires, the Nano's bus enable, PCLK and the 14 lanes; true if all passed. */
bool selftest_run(void);
bool selftest_failed(void);
/* The failed checks' details, up to max lines of 31 characters, for the test pattern. */
int selftest_lines(char lines[][32], int max);
void selftest_info(void);
void selftest_command(int argc, char **argv);
void nano_command(int argc, char **argv);
#include <stdint.h>
/* fm_top's control register: lanes from the pattern, PCLK of its own, FM demod, offsets tracked. */
#define NANO_PATTERN 1
#define NANO_OWN_CLOCK 2
#define NANO_FM 4
#define NANO_TRACK_OFFSETS 8
#define NANO_RAW40 16
#define NANO_MARKS 32
/* HDZero spectra on the bus (fm_top build 5): docs/hdzero-fpga.md. */
#define NANO_HDZ 64
bool nano_control(uint8_t control);
/* fm_top's mean CORDIC magnitude over the last 32768 samples. */
bool nano_magnitude(int *magnitude);
/* Whether the Nano's bitstream has the FM demod (build 2 or later). */
bool nano_has_fm(void);
/* fm_top's 40 MHz PLL phase, 0-15. */
bool nano_pll_phase(int phase);
/* The HDZero detector's last decision (Nano build 3 or later). */
bool nano_hdz(uint32_t *peak, uint32_t *total, uint16_t *bin, uint16_t *decisions);
/* The Nano's build, or -1 without one. */
int nano_build(void);
/* Whether the Nano's line clock saw a vsync in its last field (build 4). */
bool nano_field_vsync(bool *vsync);
