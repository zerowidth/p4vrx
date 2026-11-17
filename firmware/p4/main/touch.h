#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The panel's GT911 touch controller on the board's I2C (GPIO 7 and 8, shared with the Nano's I2C). */
bool touch_init(void);
/* Whether a finger is down, and where, in the controller's own coordinates (480 across, 800 down). */
bool touch_read(bool *down, uint16_t *x, uint16_t *y);
/* "GT911 at 0x5D" or why not, for the boot report. */
const char *touch_status(void);
