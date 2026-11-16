#pragma once

#include <stdint.h>

#include "canvas.h"

void lcd_init(void);
/* A 720x480 frame in the encoder's packed 4:2:0, for the panel; it must stay valid for a field. */
void lcd_offer(const uint8_t *yuv);
void lcd_command(int argc, char **argv);
/* The UI strip right of the picture: draw between these, and the panel shows it from the next frames. */
canvas_t *lcd_strip_begin(void);
void lcd_strip_end(void);
