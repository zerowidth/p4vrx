#pragma once

#include <stdbool.h>

/* HDZero burst decode on the P4 (hdzrx/): `hdz upload`, `hdz run`, `hdz decode`. False if argv isn't one of those. */
bool hdzdec_command(int argc, char **argv);
