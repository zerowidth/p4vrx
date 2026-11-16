// SPDX-License-Identifier: GPL-3.0-only
// Copied from C5VRX c5rx (reference/c5rx/main/link.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Drives a 7-bit counter on one side's lanes ('q' or 'i') with the sample clock on the
 * clock lane, advancing once per clock. */
esp_err_t link_start(char side, uint32_t clock_hz);
void link_stop(void);
