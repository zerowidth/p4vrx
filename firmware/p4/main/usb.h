// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/usb.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void usb_init(void);
void usb_console_info(void);
/* True while a program has the console open (DTR asserted). */
bool usb_console_connected(void);
void usb_console_write(const void *data, size_t len);
size_t usb_console_read(uint8_t *buf, size_t len, uint32_t timeout_ms);
