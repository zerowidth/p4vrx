#pragma once

#include <stdbool.h>

/* P4 header pins wired to the C5, per docs/wiring.md. */
#define PIN_C5_TX   50 /* to the C5's UART0 RX, GPIO 12 */
#define PIN_C5_RX   51 /* from the C5's UART0 TX, GPIO 11 */
#define PIN_C5_BOOT 35 /* open-drain to the C5's GPIO 28 */
#define PIN_C5_EN   34 /* open-drain to the C5's EN; never driven high */

void pins_init(void);
void pins_command(int argc, char **argv);
