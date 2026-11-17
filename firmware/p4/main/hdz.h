#pragma once

#include <stdbool.h>

/* Reads the Nano's HDZero detector, every 100 ms or so, and prints `event hdzero=0|1 ...` when its state changes. */
void hdz_poll(void);
bool hdz_detected(float *score);
/* One line, key=value: hdzero, score, peak_bin, decisions, ch. */
void hdz_info(void);
void hdz_command(int argc, char **argv);
