#include "still.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "console.h"
#include "iq.h"

#define FS_HZ 40000000
#define SMOOTH 8
/* NTSC timing at 40 MS/s, from the start of the hsync pulse. */
#define LINE_SAMPLES 2542
#define HSYNC_MIN 100
#define HSYNC_MAX 320
#define VSYNC_MIN 700
#define BLANK_START 240
#define BLANK_LEN 80
#define ACTIVE_START 380
#define ACTIVE_LEN 2080
/* Lines from the first broad vsync pulse to the first picture line. */
#define FIRST_LINE 14
#define WIDTH 320
#define HEIGHT 240

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int cmp_i16(const void *a, const void *b)
{
    return *(const int16_t *)a - *(const int16_t *)b;
}

static void send_b64(const uint8_t *p, size_t bytes)
{
    char line[77];
    for (size_t off = 0; off < bytes; off += 57) {
        size_t chunk = bytes - off < 57 ? bytes - off : 57, o = 0;
        for (size_t k = 0; k < chunk; k += 3) {
            uint32_t v = p[off + k] << 16 | (k + 1 < chunk ? p[off + k + 1] << 8 : 0) | (k + 2 < chunk ? p[off + k + 2] : 0);
            line[o++] = B64[v >> 18 & 63];
            line[o++] = B64[v >> 12 & 63];
            line[o++] = k + 1 < chunk ? B64[v >> 6 & 63] : '=';
            line[o++] = k + 2 < chunk ? B64[v & 63] : '=';
        }
        line[o++] = '\n';
        host_write(line, o);
    }
}

/* Decodes one field of the last capture to WIDTH x HEIGHT grey and sends it. Sync is the low extreme of the
 * FM-demodulated signal unless invert is set. */
static void decode(bool invert)
{
    size_t n;
    const uint16_t *iq = iq_samples(&n);
    if (n < 2 * 525 * LINE_SAMPLES / 2) {
        say("err capture at least %u samples first\n", 525 * LINE_SAMPLES);
        return;
    }
    int16_t *v = heap_caps_malloc(n * sizeof *v, MALLOC_CAP_SPIRAM);
    uint8_t *img = heap_caps_calloc(WIDTH * HEIGHT, 1, MALLOC_CAP_SPIRAM);
    int16_t *sorted = heap_caps_malloc(4096 * sizeof *sorted, MALLOC_CAP_SPIRAM);
    if (!v || !img || !sorted) {
        say("err no memory\n");
        goto out;
    }
    /* Phase step per sample, smoothed over SMOOTH samples, in units of pi/32768. */
    int32_t acc = 0;
    int16_t steps[SMOOTH] = {0};
    float pi = (float)(M_PI);
    for (size_t k = 1; k < n; ++k) {
        float i0 = (int8_t)iq[k - 1], q0 = (int8_t)(iq[k - 1] >> 8), i1 = (int8_t)iq[k], q1 = (int8_t)(iq[k] >> 8);
        float re = i1 * i0 + q1 * q0, im = q1 * i0 - i1 * q0;
        int16_t step = (int16_t)(atan2f(im, re) / pi * 32767.0f);
        if (invert) step = -step;
        acc += step - steps[k % SMOOTH];
        steps[k % SMOOTH] = step;
        v[k] = (int16_t)(acc / SMOOTH);
    }
    v[0] = v[1];
    for (int k = 0; k < 4096; ++k) sorted[k] = v[1 + (size_t)k * (n - 2) / 4096];
    qsort(sorted, 4096, sizeof *sorted, cmp_i16);
    int lo = sorted[40], hi = sorted[4055], thr = lo + (hi - lo) / 5;

    /* Sync runs below thr: hsync pulses, and the broad vsync pulses that mark a field. */
    size_t field = 0, lines_found = 0;
    for (size_t k = 1, run = 0; k < n; ++k) {
        if (v[k] < thr) {
            ++run;
        } else {
            if (run >= VSYNC_MIN && !field && k > LINE_SAMPLES) field = k - run;
            run = 0;
        }
    }
    if (!field) {
        say("err no vsync found (levels %d..%d); try still inv\n", lo, hi);
        goto out;
    }
    /* Each picture line starts at its own hsync edge, searched near where the line period puts it. */
    size_t edge = field + FIRST_LINE * LINE_SAMPLES;
    for (int y = 0; y < HEIGHT && edge + LINE_SAMPLES < n; ++y) {
        size_t best = edge;
        for (size_t k = edge - 200; k < edge + 200; ++k) {
            if (v[k - 1] >= thr && v[k] < thr) {
                size_t run = 0;
                while (k + run < n && v[k + run] < thr) ++run;
                if (run >= HSYNC_MIN && run <= HSYNC_MAX) {
                    best = k;
                    ++lines_found;
                    break;
                }
            }
        }
        int32_t blank = 0;
        for (int k = 0; k < BLANK_LEN; ++k) blank += v[best + BLANK_START + k];
        blank /= BLANK_LEN;
        float scale = 255.0f / (float)(hi - blank > 1 ? hi - blank : 1);
        for (int x = 0; x < WIDTH; ++x) {
            size_t a = best + ACTIVE_START + (size_t)x * ACTIVE_LEN / WIDTH, b = best + ACTIVE_START + (size_t)(x + 1) * ACTIVE_LEN / WIDTH;
            int32_t sum = 0;
            for (size_t k = a; k < b; ++k) sum += v[k];
            float p = ((float)sum / (float)(b - a) - (float)blank) * scale;
            img[y * WIDTH + x] = p < 0 ? 0 : p > 255 ? 255 : (uint8_t)p;
        }
        edge = best + LINE_SAMPLES;
    }
    say("still begin %d %d levels %d..%d sync below %d, %u of %d lines locked\n", WIDTH, HEIGHT, lo, hi, thr,
        (unsigned)lines_found, HEIGHT);
    send_b64(img, WIDTH * HEIGHT);
    say("still end\n");
out:
    free(v);
    free(img);
    free(sorted);
}

void still_command(int argc, char **argv)
{
    decode(argc > 1 && strcmp(argv[1], "inv") == 0);
}
