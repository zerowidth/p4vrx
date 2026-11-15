#pragma once

#include <stdbool.h>

/* Drawn in portrait: the panel is mounted with its long side along the stick. */
#define OLED_WIDTH 32
#define OLED_HEIGHT 128

/* A 128x32 SSD1306 on I2C. False when none answers; drawing then only fills the frame for oled_dump. */
bool oled_init(void);
void oled_flip(bool flipped);
void oled_clear(void);
void oled_box(int x, int y, int w, int h, bool on);
void oled_invert(int x, int y, int w, int h);
/* 8x8 glyphs, each pixel `scale` wide and high. */
void oled_text(int x, int y, int scale, const char *s);
/* A band letter, a digit or a dash from a smooth font, in a cell `size` square: 32 or 16. */
void oled_big(int x, int y, int size, char c);
void oled_show(void);
/* The frame as text on the console, two rows of pixels to a line. */
void oled_dump(void);
