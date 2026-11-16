/* The browser's side of ui/: the page sets the state and touches, and reads the strip from the frame. */
#include "ui.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static uint16_t s_fb[UI_W * UI_H];
static ui_state_t s_state = {.channel = "R4", .decoding = true, .locked = true, .level = 37};
static ui_action_t s_action;

EXPORT(sim_fb) uint16_t *sim_fb(void)
{
    return s_fb;
}

EXPORT(sim_set) void sim_set(int decoding, int locked, int level, int usb, int mhz, int hdzero)
{
    s_state.mode = hdzero ? UI_HDZERO : UI_ANALOG;
    s_state.decoding = decoding;
    s_state.locked = locked;
    s_state.level = level;
    s_state.usb = (ui_usb_t)usb;
    s_state.mhz = (uint16_t)mhz;
}

EXPORT(sim_set_hdzero) void sim_set_hdzero(int format, int link_quality)
{
    s_state.hdz_format = (ui_hdz_format_t)format;
    s_state.link_quality = link_quality;
}

EXPORT(sim_set_channel) void sim_set_channel(int band, int number)
{
    s_state.channel[0] = (char)band;
    s_state.channel[1] = (char)number;
    s_state.channel[2] = '\0';
}

EXPORT(sim_channel_mhz) int sim_channel_mhz(void)
{
    return ui_channel_mhz(s_state.channel);
}

/* True when the strip was redrawn into the frame. */
EXPORT(sim_frame) int sim_frame(void)
{
    if (!ui_update(&s_state)) return 0;
    canvas_t strip = {s_fb + UI_PIC_W, UI_STRIP_W, UI_H, UI_W};
    ui_draw(&strip);
    return 1;
}

/* The action a touch asked for: 0 for none, else the channel to tune as band << 8 | number. */
EXPORT(sim_touch) int sim_touch(int x, int y, int down)
{
    if (!ui_touch(x, y, down, &s_action) || s_action.kind != UI_TUNE) return 0;
    return s_action.channel[0] << 8 | s_action.channel[1];
}
