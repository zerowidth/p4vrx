/* The browser's side of ui/: the page sets the state and touches, and reads the strip from the frame. */
#include "font.h"
#include "ui.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static uint16_t s_fb[UI_W * UI_H];
static ui_state_t s_state = {.channel = "R4", .decoding = true, .locked = true, .level = 37, .levels = {[0 ... 47] = -128}};
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

EXPORT(sim_set_status) void sim_set_status(int console, int touch_off, int recording, int sd_card)
{
    s_state.console = console;
    s_state.touch_off = touch_off;
    s_state.recording = recording;
    s_state.sd_card = sd_card;
}

/* What the UI wants scanned: 0 nothing, 1-6 a band (R A B E F L), 7 every band. */
EXPORT(sim_scan) int sim_scan(void)
{
    const char *scan = ui_scan();
    if (!scan) return 0;
    if (scan[0] == 'a' && scan[1] == 'l') return 7;
    static const char bands[] = "RABEFL";
    for (int b = 0; bands[b]; ++b)
        if (bands[b] == scan[0]) return b + 1;
    return 0;
}

EXPORT(sim_set_level) void sim_set_level(int index, int level)
{
    if (index >= 0 && index < 48) s_state.levels[index] = (int8_t)level;
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

EXPORT(sim_set_strip_left) void sim_set_strip_left(int left)
{
    ui_set_strip_left(left);
}

EXPORT(sim_strip_x) int sim_strip_x(void)
{
    return ui_strip_x();
}

/* The pause sign drawn over a held picture: its size, and its fill and halo alpha. */
EXPORT(sim_pause_w) int sim_pause_w(void)
{
    return pause_overlay.w;
}

EXPORT(sim_pause_h) int sim_pause_h(void)
{
    return pause_overlay.h;
}

EXPORT(sim_pause_fill) const uint8_t *sim_pause_fill(void)
{
    return pause_overlay.fill;
}

EXPORT(sim_pause_halo) const uint8_t *sim_pause_halo(void)
{
    return pause_overlay.halo;
}

EXPORT(sim_picture_x) int sim_picture_x(void)
{
    return ui_picture_x();
}

/* Whether the UI covers the whole panel. */
EXPORT(sim_full) int sim_full(void)
{
    return ui_layout() == UI_LAYOUT_FULL;
}

/* True when the UI was redrawn into the frame. */
EXPORT(sim_frame) int sim_frame(double ms)
{
    if (!ui_update(&s_state, (uint32_t)ms)) return 0;
    canvas_t c = ui_layout() == UI_LAYOUT_FULL ? (canvas_t){s_fb, UI_W, UI_H, UI_W}
                                               : (canvas_t){s_fb + ui_strip_x(), UI_STRIP_W, UI_H, UI_W};
    ui_draw(&c);
    return 1;
}

/* The action a touch asked for: 0 for none, else the channel to tune as band << 8 | number. */
EXPORT(sim_touch) int sim_touch(int x, int y, int down)
{
    if (!ui_touch(x, y, down, &s_action) || s_action.kind != UI_TUNE) return 0;
    return s_action.channel[0] << 8 | s_action.channel[1];
}
