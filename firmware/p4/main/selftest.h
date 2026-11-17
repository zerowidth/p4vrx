#pragma once

#include <stdbool.h>

/* Checks the C5, its EN and BOOT wires, the Nano's bus enable, PCLK and the 14 lanes; true if all passed. */
bool selftest_run(void);
bool selftest_failed(void);
/* The failed checks' details, up to max lines of 31 characters, for the test pattern. */
int selftest_lines(char lines[][32], int max);
void selftest_info(void);
void selftest_command(int argc, char **argv);
