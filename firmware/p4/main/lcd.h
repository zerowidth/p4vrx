#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"

void lcd_init(void);
/* A 720x480 frame in the encoder's packed 4:2:0, for the panel; it must stay valid for a field. */
void lcd_offer(const uint8_t *yuv);
/* Waits up to ms for the PPA to finish with the last frame offered; false if it didn't. */
bool lcd_wait_scaled(uint32_t ms);
/* Whether the last field offered is still waiting for, or in, the PPA. */
bool lcd_busy(void);
void lcd_command(int argc, char **argv);
/* Keeps the last frame on the panel while true; the strip still updates. */
void lcd_hold(bool hold);
/* Holds the picture and shows this one frame (same format as lcd_offer, kept valid until lcd_hold(false)). */
void lcd_still(const uint8_t *yuv);
/* While held, blacks out the picture, for when the last frame had no signal; the strip is put back. */
void lcd_blank(void);
/* The UI strip right of the picture: draw between these, and the panel shows it from the next frames. */
canvas_t *lcd_strip_begin(void);
void lcd_strip_end(int lo, int hi);
/* The whole panel for UI screens that cover the picture, shown while the picture is held; lcd_full_off goes back
 * to the picture and strip. */
canvas_t *lcd_full_begin(void);
void lcd_full_end(void);
void lcd_full_off(void);
/* Landscape coordinates for a point in the panel's own (480 across, 800 down), as the picture is turned. */
void lcd_landscape(int px, int py, int *x, int *y);
