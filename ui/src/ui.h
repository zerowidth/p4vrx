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
/* HDZero is planned, not decoded yet; the simulator can show it. */
typedef enum { UI_ANALOG, UI_HDZERO } ui_mode_t;
typedef enum { UI_HDZ_720P60, UI_HDZ_540P90, UI_HDZ_1080P30 } ui_hdz_format_t;

/* What the receiver is doing, filled by the P4's glue or the simulator. */
typedef struct {
    char channel[4];
    uint16_t mhz;
    bool decoding, locked;
    /* decode_signal's level: about 10 on an empty channel, 37 for a weak signal, 60 close by. */
    int level;
    ui_usb_t usb;
    ui_mode_t mode;
    /* HDZero only: the stream's format and the link quality in percent, in place of the analog level. */
    ui_hdz_format_t hdz_format;
    int link_quality;
    /* A console session is open, touch is switched off, a recording runs, an SD card is in. */
    bool console, touch_off, recording, sd_card;
    /* Each channel's level, as level above, by band (R A B E F L) times 8 plus number less 1; -128 unread. Only the
     * channels ui_scan asks for are read. */
    int8_t levels[48];
} ui_state_t;

typedef enum { UI_NOTHING, UI_TUNE } ui_action_kind_t;

typedef struct {
    ui_action_kind_t kind;
    char channel[4];
} ui_action_t;

/* Which part of the panel the UI draws: the strip beside the picture, or all of it. */
typedef enum { UI_LAYOUT_STRIP, UI_LAYOUT_FULL } ui_layout_t;

/* Takes the receiver's state at a time in milliseconds; true when the UI should be redrawn. */
bool ui_update(const ui_state_t *now, uint32_t ms);
ui_layout_t ui_layout(void);
/* The strip on the panel's left (the default) or right; the picture takes the other UI_PIC_W columns. */
void ui_set_strip_left(bool left);
int ui_strip_x(void);
int ui_picture_x(void);
/* Draws the UI: into the UI_STRIP_W x UI_H strip, or the whole UI_W x UI_H panel, as ui_layout says. */
void ui_draw(canvas_t *c);
/* A touch at (x, y) in panel coordinates, down or lifted; true when it asks for an action. */
bool ui_touch(int x, int y, bool down, ui_action_t *out);
/* What the open menu wants scanned, with decoding paused: NULL for nothing, a band's letter, or "all". */
const char *ui_scan(void);
/* Opens a screen as a tap would, for testing without touch: 0 the main strip, 1 the channel picker, 2 the band grid. */
void ui_open(int screen);
/* A channel's frequency, such as R4's 5769, or 0 for a name that isn't one. */
uint16_t ui_channel_mhz(const char *channel);
