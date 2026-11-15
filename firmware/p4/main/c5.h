// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/c5.h), by Twotoz and the C5VRX contributors.
#pragma once

#include "driver/uart.h"

#define C5_UART UART_NUM_1

void c5_init(void);
/* Pulses EN with BOOT released, so the C5 runs its app. */
void c5_run(void);
/* Pulses EN with BOOT held low, so the C5 enters its ROM loader. */
void c5_download(void);
void c5_command(int argc, char **argv);
/* TX to the C5's GPIO 12, attached only while its ROM loader is known to own that pin. */
void c5_tx_attach(void);
void c5_tx_detach(void);
