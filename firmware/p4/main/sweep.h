#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SWEEP_MAX 48

/* Channel levels with decoding paused: the C5 steps its LO through a list on pulses from the P4, about a
 * millisecond a channel, and the P4 reads each channel's power from the I/Q while the LO sits on it. */
void sweep_command(int argc, char **argv);
/* Starts stepping through channels (names like "R4", a band letter for its eight, or "all"); the picture holds
 * its last frame. False if the C5 refused. */
bool sweep_start(const char *const *names, int count);
/* Stops stepping and puts the LO back on the decoded channel; the decoder resumes. */
void sweep_stop(void);
bool sweep_running(void);
/* The C5 stops a scan 1.5 s after it was last renewed, so a reset P4 can't boot with it marking the BOOT wire. */
bool sweep_renew(void);
/* Each listed channel's level as decode_signal gives it, -128 until read. False when not running. */
bool sweep_levels(int *count, const char **names, int8_t *levels);
/* From the decoder: a node's samples, the first numbered first, with its table of power by magnitude code. */
void sweep_take(const uint8_t *bytes, uint32_t words, uint32_t first, bool odd, const uint32_t *power);
/* From the decoder after each batch: sample n arrived at time us. */
void sweep_clock(uint32_t n, int64_t us);
