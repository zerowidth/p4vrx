// SPDX-License-Identifier: GPL-3.0-only
// Copied from C5VRX p4usb (reference/p4usb/main/uvc.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void uvc_init(void);
void uvc_counts(uint32_t *sent, uint32_t *skipped);
void uvc_info(void);
/* Whether a host has enumerated the camera, and whether it is taking frames. */
bool uvc_mounted(void);
bool uvc_streaming(void);
/* Streaming, and the host has taken a frame within the last half second or so. */
bool uvc_taking(void);
