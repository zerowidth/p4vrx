// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX c5rx (reference/c5rx/main/lanes.h), by Twotoz and the C5VRX contributors.
#pragma once

#include "driver/gpio.h"

typedef struct {
    const char *name;
    gpio_num_t gpio;
} lane_t;

/* Clock, then Q7..Q1, then I7..I1: the same order as the P4's wire table. */
extern const lane_t LANES[];
#define LANE_COUNT 15
#define LANE_CLK 0
#define LANE_Q1 7
#define LANE_I1 14

void lanes_drive_low(void);
