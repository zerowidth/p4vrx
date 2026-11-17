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
bool nano_control(uint8_t control);
/* fm_top's mean CORDIC magnitude over the last 32768 samples. */
bool nano_magnitude(int *magnitude);
