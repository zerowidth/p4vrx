// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/iq.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stddef.h>
#include <stdint.h>

void iq_command(int argc, char **argv);
/* The last capture: I in the low byte, Q in the high byte, each read as 2v+1. */
const uint16_t *iq_samples(size_t *count);
