#pragma once

#include <stdint.h>

void lcd_init(void);
/* A 720x480 frame in the encoder's packed 4:2:0, for the panel; it must stay valid for a field. */
void lcd_offer(const uint8_t *yuv);
void lcd_command(int argc, char **argv);
