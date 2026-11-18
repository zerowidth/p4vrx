#include "hdzdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"
#include "hdzrx.h"
#include "iq.h"
#include "lcd.h"
#include "usb.h"

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
/* Internal RAM, leaving 64 KB for everything else; PSRAM otherwise (about twice as slow). */
static void *alloc_fast(size_t n)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) > n + 65536 ? heap_caps_malloc(n, MALLOC_CAP_INTERNAL) : NULL;
}
static int64_t now_us(void) { return esp_timer_get_time(); }
static void yield(void) { vTaskDelay(1); }

static uint8_t *s_lcd;
static bool s_show = true;

static uint8_t clip(float v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5f); }

/* The frame into the panel's 720x480 packed 4:2:0 (C Y Y a pixel pair; the PPA reads U on even rows), nearest coded
 * pixel, with section 11.2's range expansion. A 16:9 mode is letterboxed in the 4:3 picture (354 rows). */
static void to_lcd(const hz_frame *f)
{
    if (!s_lcd) s_lcd = heap_caps_aligned_calloc(128, 1, 720 * 480 * 3 / 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_lcd) return;
    const int W = f->width, H = f->height, rows = f->mode->wide ? 354 : 480, top = (480 - rows) / 2;
    for (int y = 0; y < 480; ++y) {
        uint8_t *o = s_lcd + y * 1080;
        int sy = (y - top) * H / rows;
        bool in = y >= top && y < top + rows;
        const int16_t *c = in ? (y & 1 ? f->cr : f->cb) + sy * (W / 2) : NULL;
        for (int p = 0; p < 360; ++p) {
            if (!in) {
                o[3 * p] = 128, o[3 * p + 1] = o[3 * p + 2] = 0;
                continue;
            }
            int sx0 = (2 * p) * W / 720, sx1 = (2 * p + 1) * W / 720;
            o[3 * p] = clip(1.14f * c[sx0 / 2] + 128);
            o[3 * p + 1] = clip(1.14f * (f->y[sy * W + sx0] - 128) + 129);
            o[3 * p + 2] = clip(1.14f * (f->y[sy * W + sx1] - 128) + 129);
        }
    }
    lcd_still(s_lcd);
}

static void on_frame(void *ctx, const hz_frame *f)
{
    if (s_show && f->index == 0) to_lcd(f);
    char sp[160];
    int o = 0;
    for (int i = 0; i < f->nspecials && o < (int)sizeof sp - 8; ++i) o += snprintf(sp + o, sizeof sp - o, "%s%d", i ? ", " : "", f->specials[i]);
    sp[o] = 0;
    static char line[512];
    int n = snprintf(line, sizeof line, "hdz frame {\"index\": %d, \"mode\": \"%s\", \"width\": %d, \"height\": %d, \"o0\": %d, \"rot\": %d, "
        "\"hp_zero_fraction\": %.4f, \"nslots\": %d, \"gap\": %d, \"concealed\": %d, \"specials\": [%s]}\n", f->index, f->mode->name,
        f->width, f->height, f->o0, f->rot, (double)f->hp_zero_fraction, f->nslots, f->gap, f->nmissing, sp);
    host_write(line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}

static char s_mode[16];
static int s_ref_runs, s_no_gap, s_force;

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
    hz_opts o = {.dump = dump, .want = want, .alloc = alloc, .alloc_fast = alloc_fast, .release = heap_caps_free, .now_us = now_us, .yield = yield,
                 .max_frames = frames, .no_conceal = !conceal,
                 .mode = s_mode[0] ? s_mode : NULL, .ref_runs = s_ref_runs, .no_gap = s_no_gap, .force = s_force};
    if (used < cap) o.y_buf = (hz_cf *)(buf + used), o.y_cap = (cap - used) / sizeof(hz_cf);
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    hz_info info;
    int64_t t0 = esp_timer_get_time();
    iq_pause_decoder();
    iq_buffer_hold(true);
    int got = hz_receive(raw, n, 40e6, &o, &info, on_frame, NULL);
    iq_buffer_hold(false);
    iq_resume_decoder();
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
        "\"frames\": %d, \"runs\": [%s], \"ms\": {%s, \"total\": %.1f}, \"psram_free\": %u, \"internal_block\": %u}\n",
        (unsigned)n, info.start, info.cfo_hz, (double)info.cp_ratio, info.whole_bins, info.nsym, got, runs, ms, total / 1e3,
        (unsigned)free_before, (unsigned)internal);
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
    /* The analog decoder stays paused until hdz run (or iq resume): it corrupts the buffer while it runs. */
    iq_pause_decoder();
    say("ok upload ready %u\n", (unsigned)n);
    size_t got = 0, want = n * 2;
    int64_t t0 = esp_timer_get_time();
    iq_buffer_hold(true);
    /* A read can return 0 before its timeout (a stale wake-up), so give up only after 3 s without data. */
    int64_t last = t0;
    while (got < want && esp_timer_get_time() - last < 3000000) {
        /* USB only: host_read takes UART bytes first, and stray ones there were spliced into the upload. */
        size_t r = usb_console_read(buf + got, want - got, 100);
        if (r) got += r, last = esp_timer_get_time();
    }
    if (got < want) {
        iq_buffer_hold(false);
        say("err hdz upload: %u of %u bytes\n", (unsigned)got, (unsigned)want);
        return;
    }
    iq_set_samples(n);
    iq_buffer_hold(false);
    say("ok upload %u samples in %lld ms crc=%08lx\n", (unsigned)n, (esp_timer_get_time() - t0) / 1000,
        (unsigned long)esp_rom_crc32_le(0, buf, want));
}

/* hdz upload N | hdz run [frames=N] [window=N] [conceal=0|1] [dump=all|a,b] [show=0|1]
 * [mode=720p60|540p90|1080p30|960x720p60, else detected] [runs=hdzrx|ref] [gap=0|1] [force=1: decode under CP 1.8x] | hdz decode [samples] [same
 * options, and wide=1 for the C5's wide filter] | hdz show off. The first frame stays on the LCD until hdz show off. */
bool hdzdec_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "show") && argc > 2 && !strcmp(argv[2], "off")) {
        lcd_hold(false);
        say("ok hdz show off\n");
        return true;
    }
    if (!strcmp(sub, "upload") && argc > 2) {
        upload(strtoul(argv[2], NULL, 10));
        return true;
    }
    if (strcmp(sub, "run") && strcmp(sub, "decode")) return false;
    int frames = 1, conceal = 1, first = 2, wide = 0;
    size_t window = WINDOW, grab = WINDOW;
    if (!strcmp(sub, "decode") && argc > 2 && strchr(argv[2], '=') == NULL) grab = strtoul(argv[2], NULL, 10), first = 3;
    s_dump = "";
    s_show = true;
    s_mode[0] = 0;
    s_ref_runs = s_no_gap = s_force = 0;
    for (int i = first; i < argc; ++i) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "window=", 7)) window = strtoul(argv[i] + 7, NULL, 10);
        else if (!strncmp(argv[i], "conceal=", 8)) conceal = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "dump=", 5)) s_dump = argv[i] + 5;
        else if (!strncmp(argv[i], "show=", 5)) s_show = atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "wide=", 5)) wide = atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "mode=", 5)) strlcpy(s_mode, argv[i] + 5, sizeof s_mode);
        else if (!strncmp(argv[i], "runs=", 5)) s_ref_runs = !strcmp(argv[i] + 5, "ref");
        else if (!strncmp(argv[i], "gap=", 4)) s_no_gap = !atoi(argv[i] + 4);
        else if (!strncmp(argv[i], "force=", 6)) s_force = atoi(argv[i] + 6);
    }
    if (strcmp(sub, "decode")) {
        run(frames, window, conceal);
        return true;
    }
    /* Analog decoding is noise while HDZero is on air, and sharing the cores doubles the burst's time. */
    if (iq_grab((int)grab, wide, true)) run(frames, window, conceal);
    iq_resume_decoder();
    return true;
}
