#include "screen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "console.h"
#include "decode.h"
#include "lcd.h"
#include "sweep.h"
#include "touch.h"
#include "hdz.h"
#include "ui.h"
#include "uvc.h"

static const char BANDS[] = "RABEFL";

/* What the UI asks for that takes the C5 milliseconds to seconds, done on a task of its own so touches keep coming. */
typedef struct {
    enum { JOB_SCAN, JOB_TUNE } kind;
    char what[4];
    int64_t asked_us;
} job_t;
static QueueHandle_t s_jobs;

/* A short log of what each switch cost, for `ui log`. */
#define LOG_LINES 16
static char s_log[LOG_LINES][72];
static int s_log_next;

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_log[s_log_next++ % LOG_LINES], sizeof s_log[0], fmt, ap);
    va_end(ap);
}

static int64_t ms_since(int64_t us)
{
    return (esp_timer_get_time() - us) / 1000;
}

static void worker(void *arg)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(500)) != pdTRUE) {
            sweep_renew();
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        if (job.kind == JOB_SCAN) {
            if (!job.what[0]) {
                sweep_stop();
                note("%lld ms: scan stopped in %lld ms", ms_since(job.asked_us), ms_since(t0));
                continue;
            }
            const char *names[] = {job.what};
            bool ok = sweep_start(names, 1);
            note("%lld ms: scan %s %s in %lld ms", ms_since(job.asked_us), job.what, ok ? "started" : "failed", ms_since(t0));
        } else {
            sweep_stop();
            int64_t t1 = esp_timer_get_time();
            unsigned mhz;
            if (strcasecmp(job.what, decode_channel(&mhz)) != 0) decode_start(job.what);
            int64_t t2 = esp_timer_get_time();
            uint32_t fields = decode_fields();
            int level;
            while (!(decode_fields() != fields && decode_signal(&level)) && esp_timer_get_time() - t2 < 300000) vTaskDelay(1);
            note("%lld ms: tune %s: stop %lld, retune %lld, %s %lld ms", ms_since(job.asked_us), job.what, (t1 - t0) / 1000,
                 (t2 - t1) / 1000, decode_fields() != fields && decode_signal(&level) ? "locked in" : "no lock after",
                 ms_since(t2));
        }
    }
}

static void ask(int kind, const char *what)
{
    job_t job = {.kind = kind, .asked_us = esp_timer_get_time()};
    strlcpy(job.what, what ? what : "", sizeof job.what);
    xQueueSend(s_jobs, &job, 0);
}

/* Asks the worker to start or stop the channel scan when what the open menu wants changes. */
static void follow_scan(void)
{
    static char asked[4];
    const char *want = ui_scan();
    if (strcmp(want ? want : "", asked) == 0) return;
    strlcpy(asked, want ? want : "", sizeof asked);
    ask(JOB_SCAN, want);
}

static void fill_levels(ui_state_t *st)
{
    memset(st->levels, -128, sizeof st->levels);
    int count;
    const char *names[SWEEP_MAX];
    int8_t levels[SWEEP_MAX];
    if (!sweep_levels(&count, names, levels)) return;
    /* Until the worker restarts the scan, the last one is still the previous screen's. */
    const char *want = ui_scan();
    if (!want || (strcmp(want, "all") == 0 ? count != 48 : count != 8 || names[0][0] != want[0])) return;
    for (int k = 0; k < count; ++k) {
        const char *b = strchr(BANDS, names[k][0]);
        int n = names[k][1] - '1';
        if (b && n >= 0 && n < 8) st->levels[(b - BANDS) * 8 + n] = levels[k];
    }
}

/* Last touch in landscape, for the console. */
static int s_touch_x = -1, s_touch_y = -1;
static bool s_touch_down;
static uint32_t s_touches;

/* Touches from `ui tap`, in landscape, taken as if the controller had reported them. */
typedef struct {
    int x, y;
    bool down;
    int64_t at_us;
} fake_t;
static fake_t s_fakes[8];
static volatile int s_fake_head, s_fake_tail;

static bool fake_touch(bool *down, int *x, int *y)
{
    if (s_fake_tail == s_fake_head) return false;
    fake_t *f = &s_fakes[s_fake_tail % 8];
    if (esp_timer_get_time() < f->at_us) return false;
    *down = f->down, *x = f->x, *y = f->y;
    ++s_fake_tail;
    return true;
}

/* Polls the touch controller and passes what it reports to the UI, which may ask to tune. */
static void follow_touch(void)
{
    bool down;
    uint16_t px, py;
    int x = s_touch_x, y = s_touch_y;
    if (!fake_touch(&down, &x, &y)) {
        if (!touch_read(&down, &px, &py)) return;
        if (down) lcd_landscape(px, py, &x, &y);
    }
    if (down && !s_touch_down) ++s_touches;
    s_touch_down = down;
    s_touch_x = x, s_touch_y = y;
    ui_action_t action;
    if (ui_touch(x, y, down, &action) && action.kind == UI_TUNE) {
        note("tap: tune %s", action.channel);
        ask(JOB_TUNE, action.channel);
    }
}

/* Feeds the UI the receiver's state and touches every 20 ms, and redraws whatever it says has changed. */
static void screen_task(void *arg)
{
    for (;;) {
        ui_state_t st = {0};
        unsigned mhz;
        st.decoding = decode_running();
        strlcpy(st.channel, decode_channel(&mhz), sizeof st.channel);
        st.mhz = (uint16_t)mhz;
        st.locked = decode_signal(&st.level);
        static int polls;
        if (++polls % 5 == 0) hdz_poll();
        float score;
        if (hdz_detected(&score)) {
            /* No HDZero decode yet: the badge and the detector's strength, 2.2 to 5 as 40-100%. */
            st.mode = UI_HDZERO;
            st.link_quality = (int)(score * 20) > 100 ? 100 : (int)(score * 20);
        }
        st.usb = uvc_streaming() ? UI_USB_STREAMING : uvc_mounted() ? UI_USB_CONNECTED : UI_USB_NONE;
        follow_touch();
        follow_scan();
        fill_levels(&st);
        if (ui_update(&st, (uint32_t)(esp_timer_get_time() / 1000))) {
            bool full = ui_layout() == UI_LAYOUT_FULL;
            canvas_t *c = full ? lcd_full_begin() : lcd_strip_begin();
            if (c) {
                ui_draw(c);
                int y0, y1;
                ui_drawn_rows(&y0, &y1);
                if (full) lcd_full_end();
                else lcd_strip_end(y0, y1);
            }
            if (!full) lcd_full_off();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void screen_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "touch") == 0) {
        say("%s; %lu touches, last at %d,%d (landscape), %s\n", touch_status(), (unsigned long)s_touches, s_touch_x, s_touch_y,
            s_touch_down ? "down" : "up");
    } else if (strcmp(sub, "log") == 0) {
        for (int k = 0; k < LOG_LINES; ++k) {
            const char *line = s_log[(s_log_next + k) % LOG_LINES];
            if (line[0]) say("%s\n", line);
        }
    } else if (strcmp(sub, "tap") == 0 && argc > 3) {
        int x = atoi(argv[2]), y = atoi(argv[3]), hold = argc > 4 ? atoi(argv[4]) : 80;
        int64_t now = esp_timer_get_time();
        s_fakes[s_fake_head % 8] = (fake_t){x, y, true, now};
        ++s_fake_head;
        s_fakes[s_fake_head % 8] = (fake_t){x, y, false, now + hold * 1000ll};
        ++s_fake_head;
        note("console: tap %d,%d for %d ms", x, y, hold);
        say("ok\n");
    } else if (strcmp(sub, "open") == 0 && argc > 2) {
        ui_open(atoi(argv[2]));
        say("ok\n");
    } else if (strcmp(sub, "pick") == 0 && argc > 2) {
        ui_open(0);
        note("console: tune %s", argv[2]);
        ask(JOB_TUNE, argv[2]);
        say("ok\n");
    } else {
        say("ui touch | log | tap <x> <y> [ms] | open 0|1|2 (main, channel picker, band grid) | pick <channel>\n");
    }
}

void screen_init(void)
{
    touch_init();
    say("%s\n", touch_status());
    s_jobs = xQueueCreate(8, sizeof(job_t));
    xTaskCreatePinnedToCore(worker, "screen_work", 4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(screen_task, "screen", 4096, NULL, 3, NULL, 0);
}
