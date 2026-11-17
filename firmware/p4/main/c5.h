// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/c5.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"

#define C5_UART UART_NUM_1

void c5_init(void);
/* Holds EN low until the next reset or boot. */
void c5_hold(void);
/* Pulses EN with BOOT released, so the C5 runs its app. */
void c5_run(void);
/* Pulses EN with BOOT held low, so the C5 enters its ROM loader. */
void c5_download(void);
void c5_command(int argc, char **argv);
/* EN and BOOT against the P4's pull-downs: 1 where the C5 holds them high. */
void c5_read_wires(int *en, int *boot);
/* TX to the C5's GPIO 12, attached only while its ROM loader is known to own that pin. */
void c5_tx_attach(void);
void c5_tx_detach(void);
/* Resets the C5 and waits for c5rx to announce itself. */
bool c5_run_c5rx(uint32_t timeout_ms);
/* Sends one command line to c5rx and waits for its "ok ..." or "err ..." reply. */
bool c5_request(const char *cmd, char *reply, size_t reply_len, uint32_t timeout_ms);
