#pragma once

/* The P4's side of ui/: the receiver's state in, the panel's strip out, and the scans the menus ask for. */
void screen_init(void);
/* ui open N opens a screen and ui pick tunes as a tap would, for testing until touch is wired. */
void screen_command(int argc, char **argv);
