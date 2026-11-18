#include "hdzdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"
#include "hdzrx.h"
#include "iq.h"

/* At least one whole frame and the next training run (35 ms at 40 MS/s). */
#define WINDOW 1400000

static const char *s_dump = "";

static int want(void *ctx, const char *name)
{
    if (!strcmp(s_dump, "all")) return 1;
    const char *p = strstr(s_dump, name);
    size_t n = strlen(name);
    return p && (p == s_dump || p[-1] == ',') && (p[n] == 0 || p[n] == ',');
}

static void dump(void *ctx, const char *name, const void *d, size_t n)
{
    say("dump %s %u\n", name, (unsigned)n);
    console_base64(d, n);
    say("dump end\n");
}

static void *alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static int64_t now_us(void) { return esp_timer_get_time(); }
static void yield(void) { vTaskDelay(1); }

static void on_frame(void *ctx, const hz_frame *f)
{
    char sp[160];
    int o = 0;
    for (int i = 0; i < f->nspecials && o < (int)sizeof sp - 8; ++i) o += snprintf(sp + o, sizeof sp - o, "%s%d", i ? ", " : "", f->specials[i]);
    sp[o] = 0;
    say("hdz frame {\"index\": %d, \"o0\": %d, \"rot\": %d, \"hp_zero_fraction\": %.4f, \"nslots\": %d, \"concealed\": %d, \"specials\": [%s]}\n",
        f->index, f->o0, f->rot, (double)f->hp_zero_fraction, f->nslots, f->nmissing, sp);
}

static void run(int frames, size_t window, int conceal)
{
    size_t n, cap;
    const uint16_t *raw = iq_samples(&n);
    uint8_t *buf = (uint8_t *)iq_buffer(&cap);
    if (!raw || !n || !buf) {
        say("err hdz run: no samples (iq grab, or hdz upload)\n");
        return;
    }
    if (n > window) n = window;
    size_t used = ((const uint8_t *)raw - buf) + (n * 2 + 127) / 128 * 128;
    hz_opts o = {.dump = dump, .want = want, .alloc = alloc, .release = heap_caps_free, .now_us = now_us, .yield = yield,
                 .max_frames = frames, .no_conceal = !conceal};
    if (used < cap) o.y_buf = (hz_cf *)(buf + used), o.y_cap = (cap - used) / sizeof(hz_cf);
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    hz_info info;
    int64_t t0 = esp_timer_get_time();
    int got = hz_receive(raw, n, 40e6, &o, &info, on_frame, NULL);
    int64_t total = esp_timer_get_time() - t0;
    if (got < 0) {
        say("err hdz run: no memory (%u bytes of PSRAM free)\n", (unsigned)free_before);
        return;
    }
    char runs[HZ_MAX_RUNS * 16 + 4], ms[256];
    int p = 0;
    for (int i = 0; i < info.nruns; ++i) p += snprintf(runs + p, sizeof runs - p, "%s[%d, %d]", i ? ", " : "", info.runs[i][0], info.runs[i][1]);
    runs[p] = 0;
    p = 0;
    for (int i = 0; i < HZ_T_N; ++i) p += snprintf(ms + p, sizeof ms - p, "%s\"%s\": %.1f", i ? ", " : "", hz_stage_names[i], info.us[i] / 1e3);
    static char line[768];
    int len = snprintf(line, sizeof line, "hdz info {\"samples\": %u, \"unsettled\": 0, \"start\": %d, \"cfo_hz\": %.3f, \"cp_ratio\": %.4f, \"whole_bins\": %d, \"nsym\": %d, "
        "\"frames\": %d, \"runs\": [%s], \"ms\": {%s, \"total\": %.1f}, \"psram_free\": %u}\n",
        (unsigned)n, info.start, info.cfo_hz, (double)info.cp_ratio, info.whole_bins, info.nsym, got, runs, ms, total / 1e3,
        (unsigned)free_before);
    host_write(line, len < (int)sizeof line ? len : (int)sizeof line - 1);
    say("ok hdz run frames=%d cp=%.2f cfo=%.0f ms=%lld\n", got, (double)info.cp_ratio, info.cfo_hz, total / 1000);
}

static void upload(size_t n)
{
    size_t cap;
    uint8_t *buf = (uint8_t *)iq_buffer(&cap);
    if (!buf || n == 0 || n * 2 > cap) {
        say("err hdz upload: 1..%u samples\n", (unsigned)(cap / 2));
        return;
    }
    say("ok upload ready %u\n", (unsigned)n);
    size_t got = 0, want = n * 2;
    int64_t t0 = esp_timer_get_time();
    /* host_read can return 0 before its timeout (a stale wake-up), so give up only after 3 s without data. */
    int64_t last = t0;
    while (got < want && esp_timer_get_time() - last < 3000000) {
        size_t r = host_read(buf + got, want - got, 100);
        if (r) got += r, last = esp_timer_get_time();
    }
    if (got < want) {
        say("err hdz upload: %u of %u bytes\n", (unsigned)got, (unsigned)want);
        return;
    }
    iq_set_samples(n);
    say("ok upload %u samples in %lld ms\n", (unsigned)n, (esp_timer_get_time() - t0) / 1000);
}

/* hdz upload N | hdz run [frames=N] [window=N] [conceal=0|1] [dump=all|a,b] | hdz decode [samples] [same options] */
bool hdzdec_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "upload") && argc > 2) {
        upload(strtoul(argv[2], NULL, 10));
        return true;
    }
    if (strcmp(sub, "run") && strcmp(sub, "decode")) return false;
    int frames = 1, conceal = 1, first = 2;
    size_t window = WINDOW, grab = WINDOW;
    if (!strcmp(sub, "decode") && argc > 2 && strchr(argv[2], '=') == NULL) grab = strtoul(argv[2], NULL, 10), first = 3;
    s_dump = "";
    for (int i = first; i < argc; ++i) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "window=", 7)) window = strtoul(argv[i] + 7, NULL, 10);
        else if (!strncmp(argv[i], "conceal=", 8)) conceal = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "dump=", 5)) s_dump = argv[i] + 5;
    }
    if (!strcmp(sub, "decode") && !iq_grab((int)grab)) return true;
    run(frames, window, conceal);
    return true;
}
