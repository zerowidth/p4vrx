#pragma once

/* The OLED and the three buttons: band, channel down, channel up. Shows a starting screen and returns
 * the channel saved at the last run, or NULL. */
const char *ui_init(void);
/* Follows whatever channel the decoder is on, however it got there. */
void ui_start(void);
void ui_command(int argc, char **argv);
