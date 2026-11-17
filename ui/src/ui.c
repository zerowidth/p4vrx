#include "ui.h"

#include <stddef.h>

#include "icons.h"

#define BG RGB565(0, 0, 0)
#define FG RGB565(0xf2, 0xf2, 0xf2)
#define DIM RGB565(0x8a, 0x8d, 0x96)
#define FAINT RGB565(0x3a, 0x3c, 0x42)
#define BUTTON RGB565(0x26, 0x27, 0x2c)
#define BUTTON_EDGE RGB565(0x44, 0x46, 0x4d)
#define BUTTON_PRESSED RGB565(0x55, 0x58, 0x61)
#define BUTTON_CURRENT RGB565(0x1f, 0x4e, 0x7a)
#define CURRENT_EDGE RGB565(0x5a, 0xa9, 0xf0)
#define GOOD RGB565(0x4c, 0xc9, 0x6a)
#define FAIR RGB565(0xe8, 0xb3, 0x3a)
#define POOR RGB565(0xe5, 0x53, 0x4b)
#define DIGITAL RGB565(0x5a, 0xa9, 0xf0)

#define LEVEL_EMPTY 15
#define LEVEL_FULL 55
#define RADIUS 12
#define EDGE 2
/* The signal history: a bar every HISTORY_MS, BAR_W wide with a pixel between. */
#define HISTORY_MS 100
#define HISTORY_BAR_W 2
#define REDRAW_TICKS 2
#define HISTORY 48

static const char BANDS[] = "RABEFL";
static const char *const BAND_NAMES[] = {"R", "A", "B", "E", "F", "L"};
#define BAND_COUNT 6
static const uint16_t CHANNEL_MHZ[6][8] = {
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621},
};

typedef enum { SCREEN_MAIN, SCREEN_CHANNELS, SCREEN_BANDS } screen_t;

/* A touchable rectangle in panel coordinates; id names what it does on its screen. */
typedef struct {
    int x, y, w, h, id;
} button_t;

/* Ids: the channel button, a band's channel n as BUTTON_CHANNEL + band * 8 + n. */
enum { BUTTON_NONE = -1, BUTTON_TUNE = 0, BUTTON_BAND = 1, BUTTON_CHANNEL = 16 };

static ui_state_t s_state;
static screen_t s_screen;
static int8_t s_history[HISTORY];
/* Each channel's levels over the last few seconds, a sample every HISTORY_MS while a menu scans it; kept between
 * menus, -128 where the channel wasn't read. */
#define CH_HISTORY 60
static int8_t s_ch_history[48][CH_HISTORY];
static int s_ch_head;
static bool s_ch_ready;
static int s_history_head;
static uint32_t s_history_ms;
static bool s_have_state, s_dirty = true;
/* Only the main screen's signal history moved since the last draw; and what the next draw redraws, and the rows it did. */
static bool s_history_dirty, s_draw_all, s_draw_history;
static int s_drawn_y0, s_drawn_y1;
/* The button held down, and whether the touch is still on it. */
static int s_down_id = BUTTON_NONE;
static bool s_down, s_on_button;

static int band_of(const char *channel)
{
    for (int b = 0; BANDS[b]; ++b)
        if (BANDS[b] == channel[0]) return b;
    return -1;
}

static int number_of(const char *channel)
{
    int n = channel[1] - '1';
    return n >= 0 && n < 8 && !channel[2] ? n : -1;
}

uint16_t ui_channel_mhz(const char *channel)
{
    int b = band_of(channel), n = number_of(channel);
    return b >= 0 && n >= 0 ? CHANNEL_MHZ[b][n] : 0;
}

static bool same_state(const ui_state_t *a, const ui_state_t *b)
{
    for (int i = 0; i < 4; ++i)
        if (a->channel[i] != b->channel[i]) return false;
    return a->mhz == b->mhz && a->decoding == b->decoding && a->locked == b->locked && a->usb == b->usb &&
           a->mode == b->mode && a->hdz_format == b->hdz_format && a->console == b->console &&
           a->touch_off == b->touch_off && a->recording == b->recording && a->sd_card == b->sd_card;
}

/* The bar height, 0 to 100, for the signal now: the analog level or HDZero's link quality, 0 without lock. */
static int signal_now(const ui_state_t *s)
{
    if (!s->locked) return 0;
    int v = s->mode == UI_HDZERO ? s->link_quality : (s->level - LEVEL_EMPTY) * 100 / (LEVEL_FULL - LEVEL_EMPTY);
    return v < 1 ? 1 : v > 100 ? 100 : v;
}

bool ui_update(const ui_state_t *now, uint32_t ms)
{
    if (!s_have_state || !same_state(now, &s_state)) s_dirty = true;
    s_state = *now;
    s_have_state = true;
    if ((uint32_t)(ms - s_history_ms) >= HISTORY_MS) {
        s_history_ms = ms;
        s_history[s_history_head] = (int8_t)signal_now(now);
        s_history_head = (s_history_head + 1) % HISTORY;
        if (!s_ch_ready) {
            for (int i = 0; i < 48; ++i)
                for (int k = 0; k < CH_HISTORY; ++k) s_ch_history[i][k] = -128;
            s_ch_ready = true;
        }
        bool scanned = false;
        for (int i = 0; i < 48; ++i) {
            s_ch_history[i][s_ch_head] = now->levels[i];
            scanned |= now->levels[i] != -128;
        }
        s_ch_head = (s_ch_head + 1) % CH_HISTORY;
        /* A sample every HISTORY_MS, a redraw every REDRAW_TICKS of them: the bars stay fine-grained and the
         * graphs move half as often. */
        static unsigned ticks;
        if (++ticks % REDRAW_TICKS == 0) {
            if (scanned) s_dirty = true;
            else if (s_screen == SCREEN_MAIN) s_history_dirty = true;
        }
    }
    s_draw_all = s_dirty;
    s_draw_history = s_history_dirty && s_screen == SCREEN_MAIN;
    s_dirty = s_history_dirty = false;
    return s_draw_all || s_draw_history;
}

ui_layout_t ui_layout(void)
{
    return s_screen == SCREEN_BANDS ? UI_LAYOUT_FULL : UI_LAYOUT_STRIP;
}

static int s_strip_x = 0;

void ui_set_strip_left(bool left)
{
    int x = left ? 0 : UI_PIC_W;
    if (x != s_strip_x) s_dirty = true;
    s_strip_x = x;
}

int ui_strip_x(void)
{
    return s_strip_x;
}

int ui_picture_x(void)
{
    return s_strip_x ? 0 : UI_STRIP_W;
}

/* Layout, in panel coordinates. */
#define SX (s_strip_x + 12)
#define SW (UI_STRIP_W - 24)

/* A console takes over control, and switched-off touch does the same: nothing on screen is touchable then. */
static bool touch_locked(void)
{
    return s_state.console || s_state.touch_off;
}

static int buttons(button_t *out)
{
    int n = 0, band = band_of(s_state.channel);
    band = band < 0 ? 0 : band;
    if (s_screen == SCREEN_MAIN) {
        out[n++] = (button_t){SX, 12, SW, 128, BUTTON_TUNE};
    } else if (s_screen == SCREEN_CHANNELS) {
        out[n++] = (button_t){SX, 12, SW, 64, BUTTON_BAND};
        int cw = (SW - 8) / 2, ch = (480 - 12 - 88 - 3 * 8) / 4;
        for (int i = 0; i < 8; ++i)
            out[n++] = (button_t){SX + i % 2 * (cw + 8), 88 + i / 2 * (ch + 8), cw, ch, BUTTON_CHANNEL + band * 8 + i};
    } else {
        int label_w = 96, gap = 8, bw = (UI_W - 16 - label_w - 8 * gap) / 8, bh = (UI_H - 16 - 5 * gap) / BAND_COUNT;
        for (int b = 0; b < BAND_COUNT; ++b)
            for (int i = 0; i < 8; ++i)
                out[n++] = (button_t){8 + label_w + gap + i * (bw + gap), 8 + b * (bh + gap), bw, bh, BUTTON_CHANNEL + b * 8 + i};
    }
    return n;
}

static bool is_current(int id)
{
    int band = band_of(s_state.channel), n = number_of(s_state.channel);
    return id >= BUTTON_CHANNEL && band >= 0 && n >= 0 && id == BUTTON_CHANNEL + band * 8 + n;
}

/* A button: a border with a fill inside, brighter while held, blue for the current channel. */
static void draw_button(canvas_t *c, int ox, const button_t *b)
{
    bool held = s_down && s_on_button && s_down_id == b->id, current = is_current(b->id);
    canvas_round_rect(c, b->x - ox, b->y, b->w, b->h, RADIUS, current ? CURRENT_EDGE : BUTTON_EDGE);
    canvas_round_rect(c, b->x - ox + EDGE, b->y + EDGE, b->w - 2 * EDGE, b->h - 2 * EDGE, RADIUS - EDGE,
                      held ? BUTTON_PRESSED : current ? BUTTON_CURRENT : BUTTON);
}

/* A channel button's history: bars along its foot, oldest on the left, each column the strongest reading of its
 * slice of time. One scale for every channel, so a transmitter stands out from the empty ones in grey. */
#define SPARK_FLOOR 20
#define SPARK_TOP 45

static int spark_h(const button_t *b)
{
    return b->h / 4;
}

static void draw_history_in(canvas_t *c, int ox, const button_t *b)
{
    const int8_t *h = s_ch_history[b->id - BUTTON_CHANNEL];
    int x0 = b->x - ox + 7, w = b->w - 14, sh = spark_h(b), y1 = b->y + b->h - 7;
    canvas_fill(c, x0, y1 - 1, w, 1, FAINT);
    for (int col = 0; col < w; ++col) {
        /* Column col covers samples from oldest to newest across the width. */
        int from = col * CH_HISTORY / w, to = (col + 1) * CH_HISTORY / w;
        int best = -128;
        for (int k = from; k < (to > from ? to : from + 1); ++k) {
            int v = h[(s_ch_head + k) % CH_HISTORY];
            if (v > best) best = v;
        }
        if (best == -128) continue;
        int bar = (best - SPARK_FLOOR) * (sh - 1) / (SPARK_TOP - SPARK_FLOOR);
        bar = bar < 1 ? 1 : bar > sh - 1 ? sh - 1 : bar;
        uint16_t color = best >= 35 ? GOOD : best >= 28 ? FAIR : DIM;
        canvas_fill(c, x0 + col, y1 - 1 - bar, 1, bar, color);
    }
}

/* A channel's number, centred in the part of its button above the history. */
static void number_in(canvas_t *c, int ox, const button_t *b, const char *s, uint16_t color)
{
    const font_t *f = &font_medium;
    int room = b->h - spark_h(b) - 7;
    int baseline = b->y + (room + f->ascent - f->descent) / 2;
    canvas_text(c, f, b->x - ox + (b->w - canvas_text_width(f, s)) / 2, baseline, s, color);
}

static void text_in(canvas_t *c, int ox, const button_t *b, const font_t *f, const char *s, uint16_t color)
{
    int baseline = b->y + (b->h + f->ascent - f->descent) / 2 - 2;
    canvas_text(c, f, b->x - ox + (b->w - canvas_text_width(f, s)) / 2, baseline, s, color);
}

static void format_uint(char *out, unsigned v)
{
    char tmp[12];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10);
    while ((v /= 10) && n < 11);
    while (n) *out++ = tmp[--n];
    *out = '\0';
}

static void centred(canvas_t *c, const font_t *f, int y, const char *s, uint16_t color)
{
    canvas_text(c, f, (c->w - canvas_text_width(f, s)) / 2, y, s, color);
}

static void draw_status(canvas_t *c, int top)
{
    const ui_state_t *s = &s_state;
    char text[12];
    uint16_t mhz = s->mhz ? s->mhz : ui_channel_mhz(s->channel);
    if (s->decoding && mhz) {
        format_uint(text, mhz);
        int w = canvas_text_width(&font_small, text) + 5 + canvas_text_width(&font_small, "MHz");
        int x = canvas_text(c, &font_small, (c->w - w) / 2, top, text, DIM);
        canvas_text(c, &font_small, x + 5, top, "MHz", FAINT);
    }
    bool hdz = s->mode == UI_HDZERO;
    if (s->decoding) centred(c, &font_small, top + 32, hdz ? "HDZERO" : "ANALOG", hdz ? DIGITAL : DIM);
}

/* Indicators along the strip's foot: USB (faint when unplugged, green while streaming), then whichever of an open
 * console and switched-off touch apply. */
static void draw_icons(canvas_t *c, int y)
{
    const ui_state_t *s = &s_state;
    const icon_set_t *set = &icons_24;
    int x = 12, gap = set->w + 6;
    canvas_icon(c, set, ICON_USB, x, y, s->usb == UI_USB_STREAMING ? GOOD : s->usb == UI_USB_CONNECTED ? DIM : FAINT);
    x += gap;
    if (s->console) canvas_icon(c, set, ICON_DESKTOP, x, y, FAIR), x += gap;
    if (touch_locked()) {
        canvas_icon(c, set, ICON_HAND_POINTER, x, y, DIM);
        canvas_icon(c, set, ICON_SLASH, x, y, POOR);
    }
}

/* Bars of the last few seconds' signal, newest on the right, coloured by strength. */
static void draw_history(canvas_t *c, int x, int y, int w, int h)
{
    const char *label = s_state.mode == UI_HDZERO ? (s_state.locked ? "LINK" : "NO LINK") : (s_state.locked ? "SIGNAL" : "NO SIGNAL");
    canvas_text(c, &font_small, x, y - 8, label, s_state.locked ? DIM : POOR);
    canvas_fill(c, x, y + h - 1, w, 1, FAINT);
    int bars = w / (HISTORY_BAR_W + 1);
    bars = bars > HISTORY ? HISTORY : bars;
    for (int k = 0; k < bars; ++k) {
        int v = s_history[(s_history_head - 1 - k + HISTORY) % HISTORY];
        int bh = v * (h - 2) / 100, bx = x + w - (k + 1) * (HISTORY_BAR_W + 1);
        if (bh) canvas_fill(c, bx, y + h - 1 - bh, HISTORY_BAR_W, bh, v > 66 ? GOOD : v > 33 ? FAIR : POOR);
    }
}

#define HISTORY_Y 336
#define HISTORY_H 88

void ui_drawn_rows(int *y0, int *y1)
{
    *y0 = s_drawn_y0, *y1 = s_drawn_y1;
}

void ui_draw(canvas_t *c)
{
    if (!s_draw_all && s_draw_history) {
        canvas_fill(c, 0, HISTORY_Y, c->w, HISTORY_H, BG);
        draw_history(c, 12, HISTORY_Y, c->w - 24, HISTORY_H);
        s_drawn_y0 = HISTORY_Y, s_drawn_y1 = HISTORY_Y + HISTORY_H;
        return;
    }
    s_drawn_y0 = 0, s_drawn_y1 = c->h;
    button_t list[BAND_COUNT * 8 + 2];
    int n = buttons(list), ox = s_screen == SCREEN_BANDS ? 0 : s_strip_x;
    canvas_fill(c, 0, 0, c->w, c->h, BG);
    for (int i = 0; i < n && !touch_locked(); ++i) draw_button(c, ox, &list[i]);
    int band = band_of(s_state.channel);
    if (s_screen == SCREEN_MAIN) {
        text_in(c, ox, &list[0], &font_large, s_state.decoding ? s_state.channel : "--", FG);
        draw_status(c, 172);
        draw_history(c, 12, HISTORY_Y, c->w - 24, HISTORY_H);
        draw_icons(c, UI_H - 12 - icons_24.h);
    } else if (s_screen == SCREEN_CHANNELS) {
        /* The band name, with a grid icon at the right saying it opens the full band grid. */
        const button_t *b = &list[0];
        const icon_set_t *set = &icons_24;
        button_t text = {b->x + 4, b->y, b->w - set->w - 12, b->h, b->id};
        text_in(c, ox, &text, &font_medium, band >= 0 ? BAND_NAMES[band] : "Band", FG);
        canvas_icon(c, set, ICON_GRIP, b->x - ox + b->w - set->w - 10, b->y + (b->h - set->h) / 2, DIM);
        for (int i = 1; i < n; ++i) {
            char num[2] = {(char)('1' + (list[i].id - BUTTON_CHANNEL) % 8), '\0'};
            number_in(c, ox, &list[i], num, is_current(list[i].id) ? FG : DIM);
            draw_history_in(c, ox, &list[i]);
        }
    } else {
        int bh = list[0].h, gap = 8;
        for (int b = 0; b < BAND_COUNT; ++b)
            canvas_text(c, &font_medium, 12, 8 + b * (bh + gap) + (bh + font_medium.ascent - font_medium.descent) / 2 - 2,
                        BAND_NAMES[b], b == band ? FG : DIM);
        for (int i = 0; i < n; ++i) {
            char num[2] = {(char)('1' + (list[i].id - BUTTON_CHANNEL) % 8), '\0'};
            number_in(c, ox, &list[i], num, is_current(list[i].id) ? FG : DIM);
            draw_history_in(c, ox, &list[i]);
        }
    }
}

const char *ui_scan(void)
{
    static char band[2];
    int b = band_of(s_state.channel);
    if (s_screen == SCREEN_BANDS) return "all";
    if (s_screen != SCREEN_CHANNELS) return NULL;
    band[0] = BANDS[b < 0 ? 0 : b];
    return band;
}

/* Each screen starts its channel history fresh rather than showing levels from an earlier visit. */
static void set_screen(screen_t screen)
{
    if (screen == s_screen) return;
    s_screen = screen;
    for (int i = 0; i < 48; ++i)
        for (int k = 0; k < CH_HISTORY; ++k) s_ch_history[i][k] = -128;
}

void ui_open(int screen)
{
    set_screen(screen == 2 ? SCREEN_BANDS : screen == 1 ? SCREEN_CHANNELS : SCREEN_MAIN);
    s_dirty = true;
}

static int hit(int x, int y)
{
    button_t list[BAND_COUNT * 8 + 2];
    int n = buttons(list);
    for (int i = 0; i < n; ++i)
        if (x >= list[i].x && x < list[i].x + list[i].w && y >= list[i].y && y < list[i].y + list[i].h) return list[i].id;
    return BUTTON_NONE;
}

static bool tune(int id, ui_action_t *out)
{
    int b = (id - BUTTON_CHANNEL) / 8, n = (id - BUTTON_CHANNEL) % 8;
    out->kind = UI_TUNE;
    out->channel[0] = BANDS[b];
    out->channel[1] = (char)('1' + n);
    out->channel[2] = '\0';
    return true;
}

/* A press acts when lifted on the button it went down on. Elsewhere, a tap off the strip's picker or on the
 * band grid's labels closes it. */
bool ui_touch(int x, int y, bool down, ui_action_t *out)
{
    out->kind = UI_NOTHING;
    if (touch_locked()) {
        s_down = s_on_button = false;
        set_screen(SCREEN_MAIN);
        return false;
    }
    int id = hit(x, y);
    if (down) {
        if (!s_down) s_down = true, s_down_id = id;
        bool on = id == s_down_id && id != BUTTON_NONE;
        if (on != s_on_button) s_dirty = true;
        s_on_button = on;
        return false;
    }
    if (!s_down) return false;
    bool tapped = id == s_down_id;
    int pressed = s_down_id;
    s_down = s_on_button = false;
    s_down_id = BUTTON_NONE;
    s_dirty = true;
    if (!tapped) return false;
    if (pressed == BUTTON_TUNE && s_state.decoding) {
        set_screen(SCREEN_CHANNELS);
    } else if (pressed == BUTTON_BAND) {
        set_screen(SCREEN_BANDS);
    } else if (pressed >= BUTTON_CHANNEL) {
        set_screen(SCREEN_MAIN);
        return tune(pressed, out);
    } else if (pressed == BUTTON_NONE && s_screen != SCREEN_MAIN) {
        set_screen(SCREEN_MAIN);
    }
    return false;
}
