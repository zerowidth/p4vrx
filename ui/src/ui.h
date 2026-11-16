#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"

/* The panel in landscape: the picture on the left, the UI strip on the right. No hardware or libc here, so
 * the same code runs on the P4 and in the browser simulator (ui/sim). */
#define UI_W 800
#define UI_H 480
#define UI_PIC_W 630
#define UI_STRIP_W (UI_W - UI_PIC_W)

typedef enum { UI_USB_NONE, UI_USB_CONNECTED, UI_USB_STREAMING } ui_usb_t;

/* What the receiver is doing, filled by the P4's glue or the simulator. */
typedef struct {
    char channel[4];
    uint16_t mhz;
    bool decoding, locked;
    /* decode_signal's level: about 10 on an empty channel, 37 for a weak signal, 60 close by. */
    int level;
    ui_usb_t usb;
} ui_state_t;

typedef enum { UI_NOTHING, UI_TUNE } ui_action_kind_t;

typedef struct {
    ui_action_kind_t kind;
    char channel[4];
} ui_action_t;

/* Takes the receiver's state; true when the strip should be redrawn. */
bool ui_update(const ui_state_t *now);
/* Draws the strip (UI_STRIP_W x UI_H) for the state last taken. */
void ui_draw(canvas_t *strip);
/* A touch at (x, y) in panel coordinates, down or lifted; true when it asks for an action. */
bool ui_touch(int x, int y, bool down, ui_action_t *out);
/* A channel's frequency, such as R4's 5769, or 0 for a name that isn't one. */
uint16_t ui_channel_mhz(const char *channel);
