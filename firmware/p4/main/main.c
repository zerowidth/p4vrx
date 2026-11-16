// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/main.c), by Twotoz and the C5VRX contributors.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge.h"
#include "c5.h"
#include "console.h"
#include "pins.h"
#include "usb.h"
#include "uvc.h"
#include "video.h"

void app_main(void)
{
    /* Above the USB task, so the console and the flashing bridge keep up. */
    vTaskPrioritySet(NULL, 6);
    c5_init();
    pins_init();
    console_init();
    video_init();
    uvc_init();
    usb_init();
    say("\np4vrx ready, type help\n> ");

    uint8_t buf[64];
    for (;;) {
        size_t n = host_read(buf, sizeof buf, 20);
        bridge_poll();
        if (bridge_host_bytes(buf, n)) continue;
        for (size_t i = 0; i < n; ++i) console_feed(buf[i]);
    }
}
