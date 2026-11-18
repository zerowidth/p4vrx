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
/* Raw 40 MS/s I/Q from the Nano into the capture buffer, pausing the decoder; prints `ok grab ...`. wide turns the
 * C5's 802.11p setting off for the grab, about twice the receive filter's width. hold keeps the decoder paused
 * afterwards, until `iq resume`. spectra takes the Nano's HDZero front end's words (fm_top build 5) instead of raw
 * I/Q. */
bool iq_grab(int n, bool wide, bool hold, bool spectra);
/* The capture buffer, for samples uploaded from the host, and its size; iq_set_samples marks n of them as the last
 * capture. */
uint16_t *iq_buffer(size_t *bytes);
void iq_set_samples(size_t n);
/* Keeps every other capture out of the buffer while held (an HDZero decode reads it, and keeps its 27 MS/s
 * samples after the burst). */
void iq_buffer_hold(bool hold);
/* Pauses the analog decoder, if it runs, until iq_resume_decoder or `iq resume`: while it runs, something in it
 * overwrites 256-byte blocks of the capture buffer (2026-11-18, not found yet). */
void iq_pause_decoder(void);
void iq_resume_decoder(void);
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
