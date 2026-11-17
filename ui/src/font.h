#pragma once

#include <stdint.h>

/* A glyph's 4-bit alpha bitmap starts at offset in the font's bits, two pixels a byte, rows padded to a byte;
 * left and top place it from the pen on the baseline. */
typedef struct {
    uint32_t offset;
    uint8_t w, h;
    int8_t left, top;
    uint8_t advance;
} font_glyph_t;

/* Printable ASCII, baked by tools/bakefont.py. */
typedef struct {
    const font_glyph_t *glyphs;
    const uint8_t *bits;
    uint8_t ascent, descent;
} font_t;

extern const font_t font_small, font_medium, font_large;

/* Icons in cells of w by h pixels; a glyph's left and top place it from the cell's top-left corner. */
typedef struct {
    const font_glyph_t *glyphs;
    const uint8_t *bits;
    uint8_t w, h;
} icon_set_t;

/* A picture drawn over video: 8-bit alpha for a light fill and a dark halo around it, w by h. */
typedef struct {
    uint16_t w, h;
    const uint8_t *fill, *halo;
} overlay_t;

extern const overlay_t pause_overlay;
