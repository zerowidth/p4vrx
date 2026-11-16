// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/video.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define VIDEO_WIDTH 720
#define VIDEO_HEIGHT 480
#define VIDEO_FPS 60

void video_init(void);
/* A 720x480 frame in packed 4:2:0 (C Y0 Y1 a pixel pair, C being V on even rows and U on odd ones) for the
 * NTSC decoder to fill; video_field_done publishes it and the next call returns the other one. */
uint8_t *video_field_buffer(void);
void video_field_done(void);
void video_command(int argc, char **argv);
