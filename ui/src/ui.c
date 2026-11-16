#include "ui.h"

#define BG RGB565(0x16, 0x17, 0x1b)
#define FG RGB565(0xf2, 0xf2, 0xf2)
#define DIM RGB565(0x8a, 0x8d, 0x96)
#define TRACK RGB565(0x33, 0x35, 0x3c)
#define PRESSED RGB565(0x24, 0x26, 0x2d)
#define GOOD RGB565(0x4c, 0xc9, 0x6a)
#define FAIR RGB565(0xe8, 0xb3, 0x3a)
#define POOR RGB565(0xe5, 0x53, 0x4b)
#define DIGITAL RGB565(0x5a, 0xa9, 0xf0)

#define LEVEL_EMPTY 15
#define LEVEL_FULL 55
#define BAR_W 130
#define BAR_H 18
#define BAR_STEP (BAR_W / 10)
/* The channel's area of the strip, which a tap steps to the band's next channel. */
#define TUNE_H 210

static const char BANDS[] = "RABEFL";
/* HDZero's channels: Raceband and F2 and F4. */
static const char *const HDZ_CHANNELS[] = {"R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "F2", "F4"};
#define HDZ_COUNT (int)(sizeof HDZ_CHANNELS / sizeof HDZ_CHANNELS[0])
static const uint16_t CHANNEL_MHZ[6][8] = {
    {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917},
    {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725},
    {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866},
    {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945},
    {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880},
    {5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621},
};

/* What the strip shows: the state with the bar in steps, and whether the channel is held down. */
typedef struct {
    ui_state_t s;
    int bar;
    bool pressed;
} shown_t;

static shown_t s_shown = {.bar = -1};
static bool s_dirty = true;
static bool s_down;
static int s_down_x, s_down_y;

static int band_of(const char *channel)
{
    for (int b = 0; BANDS[b]; ++b)
        if (BANDS[b] == channel[0]) return b;
    return -1;
}

uint16_t ui_channel_mhz(const char *channel)
{
    int b = band_of(channel), n = channel[1] - '1';
    return b >= 0 && n >= 0 && n < 8 && !channel[2] ? CHANNEL_MHZ[b][n] : 0;
}

static bool same_state(const ui_state_t *a, const ui_state_t *b)
{
    for (int i = 0; i < 4; ++i)
        if (a->channel[i] != b->channel[i]) return false;
    return a->mhz == b->mhz && a->decoding == b->decoding && a->locked == b->locked && a->usb == b->usb &&
           a->mode == b->mode && a->hdz_format == b->hdz_format;
}

bool ui_update(const ui_state_t *now)
{
    int bar = now->mode == UI_HDZERO ? now->link_quality * BAR_W / 100
                                     : (now->level - LEVEL_EMPTY) * BAR_W / (LEVEL_FULL - LEVEL_EMPTY);
    bar = bar < 0 ? 0 : bar > BAR_W ? BAR_W : bar;
    /* The bar moves in tenths, and only once the level is a step and a half away, so noise in it doesn't
     * redraw the strip every pass. */
    int off = bar - s_shown.bar;
    if (s_shown.bar < 0 || off >= BAR_STEP * 3 / 2 || off <= -BAR_STEP * 3 / 2) {
        s_shown.bar = (bar + BAR_STEP / 2) / BAR_STEP * BAR_STEP;
        s_dirty = true;
    }
    if (!same_state(now, &s_shown.s)) {
        s_shown.s = *now;
        s_dirty = true;
    }
    bool dirty = s_dirty;
    s_dirty = false;
    return dirty;
}

static void centred(canvas_t *c, const font_t *f, int y, const char *s, uint16_t color)
{
    canvas_text(c, f, (c->w - canvas_text_width(f, s)) / 2, y, s, color);
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

void ui_draw(canvas_t *c)
{
    const ui_state_t *s = &s_shown.s;
    char text[12];
    canvas_fill(c, 0, 0, c->w, c->h, BG);
    if (s_shown.pressed) canvas_fill(c, 0, 0, c->w, TUNE_H, PRESSED);
    centred(c, &font_large, 120, s->decoding ? s->channel : "--", FG);
    uint16_t mhz = s->mhz ? s->mhz : ui_channel_mhz(s->channel);
    if (s->decoding && mhz) {
        format_uint(text, mhz);
        int w = canvas_text_width(&font_medium, text) + 6 + canvas_text_width(&font_small, "MHz");
        int x = canvas_text(c, &font_medium, (c->w - w) / 2, 178, text, FG);
        canvas_text(c, &font_small, x + 6, 178, "MHz", DIM);
    }
    static const char *const formats[] = {"HDZERO 720p60", "HDZERO 540p90", "HDZERO 1080p30"};
    bool hdz = s->mode == UI_HDZERO;
    if (s->decoding) centred(c, &font_small, 232, hdz ? formats[s->hdz_format] : "ANALOG", hdz ? DIGITAL : DIM);
    static const char *const usb[] = {"USB off", "USB ready", "USB live"};
    canvas_text(c, &font_small, 20, 320, usb[s->usb], s->usb == UI_USB_STREAMING ? GOOD : DIM);

    /* Analog shows the carrier's level; HDZero shows how much of the stream arrives intact. */
    int x = (c->w - BAR_W) / 2, y = 400, bar = s_shown.bar;
    const char *label = s->locked ? (hdz ? "LINK" : "SIGNAL") : (hdz ? "NO LINK" : "NO SIGNAL");
    canvas_text(c, &font_small, x, y - 14, label, s->locked ? DIM : POOR);
    canvas_fill(c, x, y, BAR_W, BAR_H, TRACK);
    uint16_t color = bar > BAR_W * 2 / 3 ? GOOD : bar > BAR_W / 3 ? FAIR : POOR;
    if (s->locked) canvas_fill(c, x, y, bar, BAR_H, color);
}

static void next_channel(const ui_state_t *s, char *out)
{
    if (s->mode == UI_HDZERO) {
        int i = 0;
        while (i < HDZ_COUNT && !(HDZ_CHANNELS[i][0] == s->channel[0] && HDZ_CHANNELS[i][1] == s->channel[1])) ++i;
        const char *next = HDZ_CHANNELS[(i + 1) % HDZ_COUNT];
        out[0] = next[0], out[1] = next[1], out[2] = '\0';
        return;
    }
    int b = band_of(s->channel), n = s->channel[1] - '1';
    if (b < 0 || n < 0 || n > 7) b = 0, n = -1;
    out[0] = BANDS[b];
    out[1] = (char)('1' + (n + 1) % 8);
    out[2] = '\0';
}

bool ui_touch(int x, int y, bool down, ui_action_t *out)
{
    out->kind = UI_NOTHING;
    bool on_channel = x >= UI_PIC_W && y < TUNE_H;
    if (down && !s_down) {
        s_down = true;
        s_down_x = x, s_down_y = y;
    }
    bool pressed = s_down && down && on_channel && s_down_x >= UI_PIC_W && s_down_y < TUNE_H;
    if (pressed != s_shown.pressed) {
        s_shown.pressed = pressed;
        s_dirty = true;
    }
    if (!down && s_down) {
        s_down = false;
        /* A tap: lifted on the channel it went down on, without wandering off it. */
        bool tap = on_channel && s_down_x >= UI_PIC_W && s_down_y < TUNE_H;
        if (tap && s_shown.s.decoding) {
            next_channel(&s_shown.s, out->channel);
            out->kind = UI_TUNE;
            return true;
        }
    }
    return false;
}
