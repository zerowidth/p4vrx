// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/iq.h), by Twotoz and the C5VRX contributors.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/parlio_rx.h"

void iq_command(int argc, char **argv);
/* The last capture: I in the low byte, Q in the high byte, each read as 2v+1. */
const uint16_t *iq_samples(size_t *count);
/* Resets the C5 into c5rx, tunes it, starts its I/Q export with one of every `every` samples of its
 * 80 MS/s bus, finds a gain that doesn't clip, and moves the C5's clock until the P4's sampling edge reads the
 * lanes while they are steady. */
bool iq_start(const char *channel, int every);
/* The gain iq_start left the C5 at. */
int iq_gain(void);
/* The I/Q offset iq_start measured at that gain, in the lanes' 2v+1 units. */
void iq_dc(float *i, float *q);
/* The C5's name and frequency for the channel last tuned. */
const char *iq_channel(unsigned *mhz);
bool iq_retune(const char *channel);
parlio_sample_edge_t iq_edge(void);
void iq_set_edge(parlio_sample_edge_t edge);
/* A PARLIO RX unit on the C5's clock and 14 lanes: I in the low byte and Q in the high byte of each
 * 16-bit word, MSB-aligned, with lines 0 and 8 held at 1. */
esp_err_t iq_del_rx(parlio_rx_unit_handle_t rx);
/* The decoder's receive takes the Nano's demodulated phase rather than raw lanes. */
void iq_phase_lanes(bool on);
esp_err_t iq_new_rx(size_t max_recv_size, parlio_rx_unit_handle_t *ret);
/* The 1024-entry table bs_phase.bsasm looks phases up in, around an I/Q offset. Only its first 512
 * entries depend on the offset. */
void iq_bs_build_lut(uint16_t *lut, float dc_i, float dc_q);
/* The magnitude, in the lanes' 2v+1 units, at the middle of one of the table's 32 magnitude codes. */
float iq_bs_code_centre(int code);
