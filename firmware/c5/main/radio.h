// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX c5rx (reference/c5rx/main/radio.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Starts the receiver on first use, then tunes to a channel name like "R3" or a frequency in MHz. */
esp_err_t radio_tune(const char *channel);
/* The tuned channel name (or "" for a bare frequency) and frequency in MHz, 0 before the first tune. */
const char *radio_channel(void);
uint16_t radio_mhz(void);
void radio_set_gain(uint8_t index);
uint8_t radio_gain(void);
/* Wideband RSSI and noise floor in dBm; false when the PHY gave an implausible value. */
bool radio_rssi(int *dbm);
bool radio_noise_floor(int *dbm);
/* Drives MODEM_DIAG bits q_top..q_top-6 onto Q7..Q1 and i_top..i_top-6 onto I7..I1, with a clock on
 * GPIO 0 that keeps one of every `every` samples of the 80 MS/s bus. Q is DIAG 0-9 and I is DIAG 10-19,
 * so 9 and 19 give the top 7 bits of each. */
esp_err_t radio_iq_start(int every, int q_top, int i_top);
void radio_iq_stop(void);
void radio_clock_slip(uint32_t us);
/* Pad drive strength, 0-3, for the data lanes and the clock. */
void radio_set_drive(int data, int clock);

void radio_hop_time(uint16_t mhz, int n, int how, uint32_t *out_min, uint32_t *out_max, uint32_t *back_min, uint32_t *back_max);
/* Moves the LO to mhz with the PLL setter alone, in about 400 us, leaving the rest of the radio as tuned; the
 * PLL has settled about 100 us in. */
void radio_lo_hop(uint16_t mhz);
bool radio_channel_mhz(const char *name, uint16_t *mhz);
