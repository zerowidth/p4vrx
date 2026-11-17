#include "sweep.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c5.h"
#include "console.h"
#include "decode.h"
#include "lcd.h"
#include "pins.h"

#define FS_HZ (80000000 / 6)
#define US_TO_SAMPLES(us) ((int32_t)((int64_t)(us) * FS_HZ / 1000000))
/* The C5 marks each step with a low pulse on the BOOT line of 20 + 8 x entry us; the hop starts as the line rises. */
#define MARK_US 20
#define MARK_STEP_US 8
/* The LO has settled about 100 us after the rise (captures/worklog.md, 2026-11-16) and stays until the next step a
 * tick later; the look takes most of the time between. */
static int32_t s_from_us = 150, s_to_us = 500;

static char s_names[SWEEP_MAX][4];
static int s_count;
static volatile bool s_on;
static volatile int64_t s_fell;

typedef struct {
    volatile bool armed;
    int entry;
    uint32_t from, to;
    uint64_t power;
    uint32_t n, clipped;
} look_t;
/* The decoder reads samples up to a few milliseconds after they arrive, so several looks can be waiting. */
#define LOOKS 8
static look_t s_looks[LOOKS];
static int s_look_next;

static float s_level[SWEEP_MAX];
/* The last raw reads of each channel, for `decode sweep stats`. */
#define RAW 32
static float s_raw[SWEEP_MAX][RAW];
static uint32_t s_raw_n[SWEEP_MAX];
#define MEDIAN 5
static bool s_read[SWEEP_MAX];
static uint32_t s_hops, s_reads, s_clipped_reads, s_lost, s_out_of_order, s_renew_failed;
static int64_t s_started_us;

static volatile uint32_t s_ref_n;
static volatile int64_t s_ref_us;

void sweep_clock(uint32_t n, int64_t us)
{
    s_ref_n = n;
    s_ref_us = us;
}

static uint32_t sample_at(int64_t us)
{
    return s_ref_n + (uint32_t)US_TO_SAMPLES(us - s_ref_us);
}

static void finish(look_t *l)
{
    l->armed = false;
    if (!l->n) return;
    /* As decode_signal: power in dB less the gain it was read at, so levels compare across gains. */
    float db = 10 * log10f((float)l->power / l->n + 1) + 77 - decode_gain();
    if (l->clipped * 20 > l->n) ++s_clipped_reads;
    s_raw[l->entry][s_raw_n[l->entry]++ % RAW] = db;
    s_level[l->entry] = s_read[l->entry] ? 0.5f * s_level[l->entry] + 0.5f * db : db;
    s_read[l->entry] = true;
    ++s_reads;
}

void sweep_take(const uint8_t *b, uint32_t words, uint32_t first, bool odd, const uint32_t *power)
{
    for (int i = 0; i < LOOKS; ++i) {
        look_t *l = &s_looks[i];
        if (!l->armed || (int32_t)(first + words - l->from) <= 0) continue;
        for (uint32_t k = (int32_t)(l->from - first) > 0 ? l->from - first : 0; k < words; ++k) {
            if ((int32_t)(first + k - l->to) >= 0) {
                finish(l);
                break;
            }
            uint8_t info = b[2 * k + 1 + odd];
            l->power += power[info & 31];
            l->clipped += (info & 0x60) != 0;
            ++l->n;
        }
    }
}

/* The C5's marks, timed here: the rise places the look, the width says whether it's the first entry. */
static void on_edge(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (!gpio_get_level(PIN_C5_BOOT)) {
        s_fell = now;
        return;
    }
    /* The mark's width names the entry; glitches on the wire are a few us, and the marks come a tick (1 ms) apart. */
    static int64_t last;
    int64_t width = now - s_fell;
    int entry = (int)((width - MARK_US + MARK_STEP_US / 2) / MARK_STEP_US);
    if (width < MARK_US - MARK_STEP_US / 2 || entry >= s_count || now - last < 600 || !s_on) return;
    /* The C5 goes through the list in order, so a glitch that happens to look like a mark names the wrong entry. */
    static int prev = -1;
    if (prev >= 0 && entry != (prev + 1) % s_count && now - last < 2000) {
        ++s_out_of_order;
        return;
    }
    prev = entry;
    last = now;
    uint32_t rose = sample_at(now);
    look_t *l = &s_looks[s_look_next];
    if (l->armed) ++s_lost;
    l->entry = entry;
    l->from = rose + US_TO_SAMPLES(s_from_us);
    l->to = rose + US_TO_SAMPLES(s_to_us);
    l->power = 0;
    l->n = l->clipped = 0;
    l->armed = true;
    s_look_next = (s_look_next + 1) % LOOKS;
    ++s_hops;
}

static bool tell_c5(void)
{
    char cmd[8 + SWEEP_MAX * 4] = "sweep", reply[96];
    for (int k = 0; k < s_count && s_on; ++k) strcat(strcat(cmd, " "), s_names[k]);
    /* The C5 answers in a few ms; noise on the UART loses the odd reply, so ask again rather than wait. */
    for (int tries = 0; tries < 5; ++tries)
        if (c5_request(cmd, reply, sizeof reply, 200)) return true;
    say("sweep: %s\n", reply);
    return false;
}

static void release_boot(void)
{
    gpio_isr_handler_remove(PIN_C5_BOOT);
    gpio_set_intr_type(PIN_C5_BOOT, GPIO_INTR_DISABLE);
    gpio_set_level(PIN_C5_BOOT, 1);
    gpio_set_direction(PIN_C5_BOOT, GPIO_MODE_INPUT_OUTPUT_OD);
}

/* A band letter stands for its eight channels and "all" for every band. */
static void take_names(const char *const *names, int count)
{
    s_count = 0;
    for (int k = 0; k < count && s_count < SWEEP_MAX; ++k) {
        const char *bands = strcmp(names[k], "all") == 0 ? "RABEFL" : strlen(names[k]) == 1 ? names[k] : NULL;
        if (!bands) {
            strlcpy(s_names[s_count++], names[k], sizeof s_names[0]);
            continue;
        }
        for (const char *b = bands; *b; ++b)
            for (int n = 1; n <= 8 && s_count < SWEEP_MAX; ++n) snprintf(s_names[s_count++], sizeof s_names[0], "%c%d", *b, n);
    }
}

bool sweep_start(const char *const *names, int count)
{
    static bool isr;
    if (!isr) {
        esp_err_t err = gpio_install_isr_service(0);
        isr = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    }
    if (s_on) sweep_stop();
    take_names(names, count);
    if (!s_count) return false;
    memset(s_read, 0, sizeof s_read);
    memset(s_raw_n, 0, sizeof s_raw_n);
    memset(s_looks, 0, sizeof s_looks);
    s_hops = s_reads = s_clipped_reads = s_lost = s_out_of_order = 0;
    lcd_hold(true);
    decode_hold(true);
    /* The C5 drives the BOOT line while it steps; ours only listens. */
    gpio_set_direction(PIN_C5_BOOT, GPIO_MODE_INPUT);
    gpio_set_intr_type(PIN_C5_BOOT, GPIO_INTR_ANYEDGE);
    gpio_isr_handler_add(PIN_C5_BOOT, on_edge, NULL);
    s_on = true;
    if (!tell_c5()) {
        s_on = false;
        release_boot();
        decode_hold(false);
        lcd_hold(false);
        return false;
    }
    s_started_us = esp_timer_get_time();
    return true;
}

void sweep_stop(void)
{
    if (!s_on) return;
    s_on = false;
    tell_c5();
    release_boot();
    decode_hold(false);
    lcd_hold(false);
}

bool sweep_renew(void)
{
    char reply[48];
    if (!s_on) return false;
    if (c5_request("renew", reply, sizeof reply, 300)) return true;
    ++s_renew_failed;
    return false;
}

bool sweep_running(void)
{
    return s_on;
}

bool sweep_levels(int *count, const char **names, int8_t *levels)
{
    if (!s_on) return false;
    *count = s_count;
    for (int k = 0; k < s_count; ++k) {
        names[k] = s_names[k];
        /* The median of the last few reads: a stray read on the wrong channel can't move it. */
        int n = s_raw_n[k] < MEDIAN ? s_raw_n[k] : MEDIAN;
        if (!n) {
            levels[k] = -128;
            continue;
        }
        float v[MEDIAN];
        for (int i = 0; i < n; ++i) v[i] = s_raw[k][(s_raw_n[k] - 1 - i) % RAW];
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0 && v[j] < v[j - 1]; --j) {
                float t = v[j];
                v[j] = v[j - 1], v[j - 1] = t;
            }
        levels[k] = (int8_t)lrintf(v[n / 2]);
    }
    return true;
}

void sweep_command(int argc, char **argv)
{
    const char *sub = argc > 2 ? argv[2] : "";
    if (strcmp(sub, "on") == 0 && argc > 3) {
        if (!sweep_start((const char *const *)argv + 3, argc - 3)) say("sweep: not started\n");
    } else if (strcmp(sub, "off") == 0) {
        int64_t t0 = esp_timer_get_time();
        sweep_stop();
        say("stopped in %lld us\n", esp_timer_get_time() - t0);
    } else if (strcmp(sub, "pick") == 0 && argc > 3) {
        /* As a tap on a channel would: stop, tune, and time until the decoder locks again. */
        int64_t t0 = esp_timer_get_time();
        sweep_stop();
        int64_t stopped = esp_timer_get_time();
        unsigned mhz;
        /* The decoder retunes in place for another channel; for the same one it only has to resume. */
        if (strcasecmp(argv[3], decode_channel(&mhz)) != 0) decode_start(argv[3]);
        int64_t tuned = esp_timer_get_time();
        int level;
        /* Locked means a field finished since the hold, with its vertical sync and most of its lines. */
        uint32_t fields = decode_fields();
        while (!(decode_fields() != fields && decode_signal(&level)) && esp_timer_get_time() - tuned < 3000000) vTaskDelay(1);
        int64_t locked = esp_timer_get_time();
        say("stop %lld us, tune %lld us, lock %lld us: %lld ms in all%s\n", stopped - t0, tuned - stopped, locked - tuned,
            (locked - t0) / 1000, locked - tuned < 3000000 ? "" : " (no lock in 3 s)");
        return;
    } else if (strcmp(sub, "stats") == 0) {
        say("raw reads per channel: mean, standard deviation, min-max in dB over the last %d\n", RAW);
        for (int k = 0; k < s_count; ++k) {
            int n = s_raw_n[k] < RAW ? s_raw_n[k] : RAW;
            if (!n) continue;
            float sum = 0, sq = 0, lo = 999, hi = -999;
            for (int i = 0; i < n; ++i) {
                float v = s_raw[k][i];
                sum += v, sq += v * v;
                lo = v < lo ? v : lo, hi = v > hi ? v : hi;
            }
            float mean = sum / n, sd = sqrtf(sq / n - mean * mean > 0 ? sq / n - mean * mean : 0);
            say(" %s %.1f %.1f %.0f-%.0f%s", s_names[k], mean, sd, lo, hi, k % 4 == 3 ? "\n" : ";");
        }
        say("\n");
        return;
    } else if (strcmp(sub, "window") == 0 && argc > 4) {
        s_from_us = atoi(argv[3]);
        s_to_us = atoi(argv[4]);
    }
    float secs = (esp_timer_get_time() - s_started_us) / 1e6f;
    say("sweep %s, %d channels, look %ld-%ld us after each pulse; %lu hops, %lu reads (%lu clipped), %lu lost", s_on ? "on" : "off",
        s_count, (long)s_from_us, (long)s_to_us, (unsigned long)s_hops, (unsigned long)s_reads, (unsigned long)s_clipped_reads,
        (unsigned long)s_lost);
    say(", %lu marks out of order, %lu renewals failed", (unsigned long)s_out_of_order, (unsigned long)s_renew_failed);
    if (s_on && secs > 0 && s_count) say(", %.1f passes a second", s_reads / secs / s_count);
    say("\n");
    for (int k = 0; k < s_count && s_on; ++k) {
        say(" %s", s_names[k]);
        if (s_read[k]) say(" %.0f", s_level[k]);
        else say(" -");
        if (k % 8 == 7) say("\n");
    }
    if (s_on && s_count % 8) say("\n");
}
