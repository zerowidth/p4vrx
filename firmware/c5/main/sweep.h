#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SWEEP_MAX 48

/* Steps the LO through the named channels once a tick, marking each step on the BOOT line; none stops it, puts the
 * LO back on the tuned channel and releases the line. The number armed, or -1 for a name that isn't a channel. */
int sweep_set(const char *const *names, int count);
void sweep_counts(uint32_t *hops, uint32_t *passes);
/* Keeps a running scan going another 1.5 s; false if none is running. */
bool sweep_renew(void);
