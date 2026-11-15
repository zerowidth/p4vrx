#include "ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "console.h"
#include "decode.h"
#include "oled.h"
#include "pins.h"

#define BANDS "ABEFRL"
#define BAND_COUNT 6
#define CHANNELS 8

#define POLL_MS 10
#define DEBOUNCE_POLLS 3
#define HOLD_MS 600
#define REPEAT_MS 300
/* The bars stay this long after a press, and are read again this often. */
#define SELECT_MS 4000
#define SWEEP_MS 500
#define SAVE_AFTER_MS 2000
#define MESSAGE_MS 1500
#define HISTORY_MS 500

/* decode_signal's level with no carrier at full gain, and the range a bar or the graph covers above it. */
#define LEVEL_FLOOR 12
#define LEVEL_SPAN 64
/* A scan takes the strongest channel only when it stands this far above the weakest. */
#define SCAN_MARGIN 16

/* Tuned: the band over the channel in 32 px glyphs, the MHz, the level's history, then lock and level.
 * After a press the top is the band and the MHz, over a row for each of its channels. */
#define TUNED_MHZ_Y 68
#define HISTORY_Y 80
#define HISTORY_HEIGHT 30
#define MHZ_Y 18
#define ROWS_Y 30
#define ROW_PITCH 10
#define DIGIT_X 5
#define BAR_X 14
#define BAR_WIDTH (OLED_WIDTH - BAR_X)
#define STATUS_Y 112

typedef enum { EV_BAND, EV_SCAN, EV_DOWN, EV_UP } event_t;

static QueueHandle_t s_events;
static bool s_flipped;
static char s_saved[8];

static char s_name[8];
static unsigned s_mhz;
/* -1 on a bare frequency. */
static int s_band = -1, s_ch;
static int8_t s_levels[CHANNELS];
static int8_t s_history[OLED_WIDTH];
static int64_t s_select_until, s_next_sweep, s_changed_at, s_message_until;
static const char *s_message;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void poll_buttons(void *arg)
{
    static const struct {
        int pin;
        event_t tap, hold;
        bool repeats;
    } buttons[] = {
        {PIN_BUTTON_BAND, EV_BAND, EV_SCAN, false},
        {PIN_BUTTON_DOWN, EV_DOWN, EV_DOWN, true},
        {PIN_BUTTON_UP, EV_UP, EV_UP, true},
    };
    static struct {
        uint8_t same;
        bool down, held;
        uint16_t ms;
    } state[3];
    for (int i = 0; i < 3; ++i) {
        bool down = gpio_get_level(buttons[i].pin) == 0, emit = false;
        if (down != state[i].down) {
            if (++state[i].same < DEBOUNCE_POLLS) continue;
            state[i].down = down;
            /* The band button taps on release, since holding it means something else. */
            emit = down ? buttons[i].repeats : !buttons[i].repeats && !state[i].held;
            state[i].held = false;
            state[i].ms = 0;
        } else if (down && (state[i].ms += POLL_MS) >= HOLD_MS && (buttons[i].repeats || !state[i].held)) {
            emit = state[i].held = true;
            state[i].ms = HOLD_MS - REPEAT_MS;
        }
        state[i].same = 0;
        event_t ev = state[i].held ? buttons[i].hold : buttons[i].tap;
        if (emit) xQueueSend(s_events, &ev, 0);
    }
}

static void say_message(const char *text)
{
    s_message = text;
    s_message_until = now_ms() + MESSAGE_MS;
}

/* The decoder's channel is the only state: the console changes it too. */
static void follow(void)
{
    unsigned mhz;
    const char *name = decode_channel(&mhz);
    s_mhz = mhz;
    if (strcmp(name, s_name) == 0) return;
    strlcpy(s_name, name, sizeof s_name);
    s_changed_at = now_ms();
    const char *band = name[0] ? strchr(BANDS, name[0]) : NULL;
    bool named = band && name[1] >= '1' && name[1] <= '8' && !name[2];
    int was = s_band;
    s_band = named ? (int)(band - BANDS) : -1;
    s_ch = named ? name[1] - '1' : 0;
    if (s_band != was) memset(s_levels, 0, sizeof s_levels);
}

static void save(void)
{
    if (!s_name[0] || strcmp(s_name, s_saved) == 0 || now_ms() - s_changed_at < SAVE_AFTER_MS) return;
    nvs_handle_t nvs;
    if (nvs_open("p4usb", NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_str(nvs, "channel", s_name) == ESP_OK && nvs_commit(nvs) == ESP_OK) strlcpy(s_saved, s_name, sizeof s_saved);
    nvs_close(nvs);
}

static int bar(int level, int width)
{
    int w = (level - LEVEL_FLOOR) * width / LEVEL_SPAN;
    return w < 1 ? 1 : w > width ? width : w;
}

static void draw(void)
{
    char text[12], band[2] = {s_band < 0 ? '-' : BANDS[s_band], '\0'}, ch[2] = {s_band < 0 ? '-' : (char)('1' + s_ch), '\0'};
    int64_t now = now_ms();
    bool select = now < s_select_until && s_band >= 0;
    oled_clear();
    snprintf(text, sizeof text, "%u", s_mhz);
    oled_text(0, select ? MHZ_Y : TUNED_MHZ_Y, 1, text);

    if (select) {
        oled_big(8, 0, 16, band[0]);
        for (int i = 0; i < CHANNELS; ++i) {
            int y = ROWS_Y + i * ROW_PITCH;
            text[0] = '1' + i;
            text[1] = '\0';
            oled_text(DIGIT_X, y, 1, text);
            /* A pointer at the tuned channel. */
            for (int c = 0; c < 4 && i == s_ch; ++c) oled_box(c, y + c, 1, 7 - 2 * c, true);
            oled_box(BAR_X, y, BAR_WIDTH, 8, true);
            oled_box(BAR_X + 1, y + 1, BAR_WIDTH - 2, 6, false);
            oled_box(BAR_X + 1, y + 1, bar(s_levels[i], BAR_WIDTH - 2), 6, true);
        }
    } else {
        oled_big(0, 0, 32, band[0]);
        oled_big(0, 32, 32, ch[0]);
        /* The newest reading is at the right. */
        for (int i = 0; i < OLED_WIDTH; ++i) {
            int h = bar(s_history[i], HISTORY_HEIGHT);
            oled_box(i, HISTORY_Y + HISTORY_HEIGHT - h, 1, h, true);
        }
    }

    int level;
    bool video = decode_signal(&level);
    if (now < s_message_until) {
        oled_text(0, STATUS_Y, 1, s_message);
    } else {
        oled_text(0, STATUS_Y, 1, !decode_running() ? "NORX" : video ? "VID" : "----");
        int percent = (level - LEVEL_FLOOR) * 100 / LEVEL_SPAN;
        snprintf(text, sizeof text, "%d%%", percent < 0 ? 0 : percent > 100 ? 100 : percent);
        oled_text(0, STATUS_Y + 8, 1, text);
    }
    oled_show();
}

static bool pressed(void)
{
    return uxQueueMessagesWaiting(s_events) > 0;
}

static void sweep(void)
{
    if (s_band < 0) return;
    char names[CHANNELS][3];
    const char *list[CHANNELS];
    for (int i = 0; i < CHANNELS; ++i) {
        snprintf(names[i], sizeof names[i], "%c%d", BANDS[s_band], i + 1);
        list[i] = names[i];
    }
    decode_survey(list, CHANNELS, s_levels, pressed);
    s_next_sweep = now_ms() + SWEEP_MS;
}

static void tune(int band, int ch)
{
    char name[3] = {BANDS[band], (char)('1' + ch), '\0'};
    decode_start(name);
    follow();
    s_select_until = now_ms() + SELECT_MS;
    s_next_sweep = 0;
}

static void scan(void)
{
    static char names[BAND_COUNT * CHANNELS][3];
    static const char *list[BAND_COUNT * CHANNELS];
    static int8_t levels[BAND_COUNT * CHANNELS];
    for (int i = 0; i < BAND_COUNT * CHANNELS; ++i) {
        snprintf(names[i], sizeof names[i], "%c%d", BANDS[i / CHANNELS], i % CHANNELS + 1);
        list[i] = names[i];
    }
    s_message = "SCAN";
    s_message_until = INT64_MAX;
    draw();
    s_message_until = 0;
    if (!decode_survey(list, BAND_COUNT * CHANNELS, levels, NULL)) {
        say_message("NORX");
        return;
    }
    int best = 0, worst = 0;
    for (int i = 1; i < BAND_COUNT * CHANNELS; ++i) {
        if (levels[i] > levels[best]) best = i;
        if (levels[i] < levels[worst]) worst = i;
    }
    if (levels[best] - levels[worst] < SCAN_MARGIN) {
        say_message("NONE");
        return;
    }
    tune(best / CHANNELS, best % CHANNELS);
    memcpy(s_levels, levels + best / CHANNELS * CHANNELS, sizeof s_levels);
    s_next_sweep = now_ms() + SWEEP_MS;
}

static void handle(event_t ev)
{
    int band = s_band < 0 ? 0 : s_band;
    switch (ev) {
    case EV_BAND:
        tune(s_band < 0 ? 0 : (band + 1) % BAND_COUNT, s_ch);
        break;
    case EV_DOWN:
        tune(band, (s_ch + CHANNELS - 1) % CHANNELS);
        break;
    case EV_UP:
        tune(band, (s_ch + 1) % CHANNELS);
        break;
    case EV_SCAN:
        scan();
        break;
    }
}

static void ui_task(void *arg)
{
    int64_t next_history = 0;
    for (;;) {
        event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(50)) == pdTRUE) handle(ev);
        follow();
        int64_t now = now_ms();
        if (now >= next_history) {
            next_history = now + HISTORY_MS;
            int level;
            decode_signal(&level);
            memmove(s_history, s_history + 1, sizeof s_history - 1);
            s_history[OLED_WIDTH - 1] = (int8_t)level;
        }
        draw();
        if (now < s_select_until && now >= s_next_sweep && !pressed()) sweep();
        save();
    }
}

const char *ui_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t nvs;
    uint8_t flipped = 0;
    if (err == ESP_OK && nvs_open("p4usb", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof s_saved;
        if (nvs_get_str(nvs, "channel", s_saved, &len) != ESP_OK) s_saved[0] = '\0';
        nvs_get_u8(nvs, "flip", &flipped);
        nvs_close(nvs);
    }
    s_flipped = flipped;

    static const int pins[] = {PIN_BUTTON_BAND, PIN_BUTTON_DOWN, PIN_BUTTON_UP};
    for (int i = 0; i < 3; ++i) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }

    oled_init();
    oled_flip(s_flipped);
    oled_clear();
    oled_text(0, 0, 2, "P4");
    oled_text(4, 18, 1, "USB");
    oled_text(0, STATUS_Y, 1, s_saved);
    oled_show();
    return s_saved[0] ? s_saved : NULL;
}

void ui_start(void)
{
    s_events = xQueueCreate(8, sizeof(event_t));
    const esp_timer_create_args_t poll = {.callback = poll_buttons, .name = "buttons"};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&poll, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, POLL_MS * 1000));
    xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 4, NULL, 0);
}

void ui_command(int argc, char **argv)
{
    static const char *const presses[] = {"band", "scan", "down", "up"};
    const char *sub = argc > 1 ? argv[1] : "";
    for (int i = 0; i < 4; ++i) {
        if (strcmp(sub, presses[i]) == 0) {
            event_t ev = (event_t)i;
            xQueueSend(s_events, &ev, 0);
            return;
        }
    }
    if (strcmp(sub, "flip") == 0) {
        s_flipped = !s_flipped;
        oled_flip(s_flipped);
        nvs_handle_t nvs;
        if (nvs_open("p4usb", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, "flip", s_flipped);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    } else if (strcmp(sub, "sweep") == 0 && s_band >= 0) {
        int64_t t0 = esp_timer_get_time();
        s_next_sweep = 0;
        sweep();
        say("%c1-8:", BANDS[s_band]);
        for (int i = 0; i < CHANNELS; ++i) say(" %d", s_levels[i]);
        say(", %lld ms\n", (esp_timer_get_time() - t0) / 1000);
    } else if (strcmp(sub, "") == 0) {
        oled_dump();
    } else {
        say("ui [band | scan | down | up | sweep | flip]\n");
    }
}
