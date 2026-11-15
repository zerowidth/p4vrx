#pragma once

/* P4 pins wired to the C5, per docs/p4-receiver-plan.md. */
#define PIN_C5_EN   26 /* open-drain to the C5's EN */
#define PIN_C5_BOOT 8  /* open-drain to the C5's GPIO 28 */
#define PIN_C5_RX   32 /* from the C5's UART0 TX, GPIO 11 */
#define PIN_C5_TX   27 /* to the C5's UART0 RX, GPIO 12 */

/* Right header, between its 3V3 and ground holes. */
#define PIN_OLED_SCL 20
#define PIN_OLED_SDA 21
/* Buttons to ground, on the three left header holes below its top ground hole. */
#define PIN_BUTTON_BAND 18
#define PIN_BUTTON_DOWN 17
#define PIN_BUTTON_UP   16

typedef struct {
    const char *name;
    int c5;
    int p4;
} wire_t;

/* The sample clock and the 14 I/Q lanes. */
extern const wire_t BUS_WIRES[];
extern const int BUS_WIRE_COUNT;
