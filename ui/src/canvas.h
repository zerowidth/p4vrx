#pragma once

#include <stdint.h>

#include "font.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

/* RGB565 pixels in the landscape orientation the viewer sees, stride pixels a row. */
typedef struct {
    uint16_t *px;
    int w, h, stride;
} canvas_t;

void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint16_t color);
/* A filled rectangle with antialiased corners of radius r. */
void canvas_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint16_t color);
/* Draws text with its baseline at y, blended over what is there, and returns the pen's x after it. */
int canvas_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint16_t color);
int canvas_text_width(const font_t *f, const char *s);
/* Draws an icon with its cell's top-left corner at (x, y). */
void canvas_icon(canvas_t *c, const icon_set_t *set, int icon, int x, int y, uint16_t color);
