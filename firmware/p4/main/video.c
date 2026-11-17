// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/video.c), by Twotoz and the C5VRX contributors.
#include "video.h"

#include <stdio.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bridge.h"
#include "console.h"
#include "decode.h"
#include "lcd.h"
#include "overlay.h"
#include "uvc.h"

/* One held per reader, the newest finished, and one being encoded, so the producer never waits. */
#define SLOTS (VIDEO_READERS + 2)
#define SLOT_BYTES (256 * 1024)
/* A keyframe every second keeps a host that joins or loses a frame from waiting long for a picture. */
#define H264_GOP (VIDEO_FPS / 2)
/* Noise from an analog receiver costs bits: 8 Mbit/s left 17 KB a frame at 60 fps. */
#define H264_BITRATE_DEFAULT 16000000
#define H264_QP_MIN 26
#define H264_QP_MAX 51
#define TEXT_HALVES 3
#define MARKER 24

typedef struct {
    uint8_t *base;
    size_t cap;
    video_frame_t frame;
} slot_t;

static slot_t s_slots[SLOTS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_newest = -1;
static int s_held[VIDEO_READERS] = {-1, -1};
static uint32_t s_taken[VIDEO_READERS];
static SemaphoreHandle_t s_published;
static uint8_t *s_raw;
static uint32_t s_raw_size;
static int s_fill;
static volatile int s_ready = -1;
static int64_t s_last_field_us;
static esp_h264_enc_handle_t s_enc;
static esp_h264_enc_param_hw_handle_t s_enc_param;
/* The test pattern for the encoder, in the packed 4:2:0 its hardware reads: C Y Y a pixel pair, C being V on even rows and U on odd ones. */
static uint8_t *s_yuv420;
/* The decoder fills one, in the same layout, while the encoder reads the other. */
static uint8_t *s_field[2];
static uint32_t s_yuv420_size;
static TaskHandle_t s_task;

static uint32_t s_frames;
static uint32_t s_errors;
static esp_err_t s_last_error;
static uint32_t s_render_us;
static uint32_t s_encode_us;
static uint32_t s_fps_tenths;
static uint32_t s_bitrate = H264_BITRATE_DEFAULT;
static uint32_t s_keyframes;
static uint32_t s_convert_us;
static volatile bool s_want_key;
/* `video encode off` stops the encoder, to see what it costs the decoder. */
static volatile bool s_encode = true;

#define NOTIFY_TICK 1
#define NOTIFY_FIELD 2
/* With the decoder stopped and no field for this long, the test pattern takes over. While it runs, a dead channel
 * or a paused decoder gives fields slowly or not at all, and the last one stays up rather than flicking to this. */
#define PATTERN_AFTER_US (100 * 1000)
/* Its 4:2:0 conversion runs on the CPU through PSRAM, about 12 ms a frame, so it runs at 10 fps. */
#define PATTERN_EVERY 6
static uint32_t s_pattern_ticks;

static void tick(void *arg)
{
    xTaskNotify(s_task, NOTIFY_TICK, eSetBits);
}

static int writable_slot(void)
{
    taskENTER_CRITICAL(&s_lock);
    int w = 0;
    for (;; ++w) {
        bool busy = w == s_newest;
        for (int r = 0; r < VIDEO_READERS; ++r) busy |= w == s_held[r];
        if (!busy) break;
    }
    taskEXIT_CRITICAL(&s_lock);
    return w;
}

static size_t newest_len(void)
{
    taskENTER_CRITICAL(&s_lock);
    size_t len = s_newest < 0 ? 0 : s_slots[s_newest].frame.len;
    taskEXIT_CRITICAL(&s_lock);
    return len;
}

static void render(uint32_t seq)
{
    uint32_t ms = esp_timer_get_time() / 1000;
    uint32_t sent, skipped;
    uvc_counts(&sent, &skipped);
    char lines[10][32];
    snprintf(lines[0], sizeof lines[0], "P4VRX TEST PATTERN");
    snprintf(lines[1], sizeof lines[1], "uptime %02lu:%02lu:%02lu.%03lu", (unsigned long)(ms / 3600000),
             (unsigned long)(ms / 60000 % 60), (unsigned long)(ms / 1000 % 60), (unsigned long)(ms % 1000));
    snprintf(lines[2], sizeof lines[2], "frame  %lu", (unsigned long)seq);
    snprintf(lines[3], sizeof lines[3], "rate   %lu.%lu fps", (unsigned long)(s_fps_tenths / 10),
             (unsigned long)(s_fps_tenths % 10));
    snprintf(lines[4], sizeof lines[4], "h264   %u bytes", (unsigned)newest_len());
    snprintf(lines[5], sizeof lines[5], "encode %lu us", (unsigned long)s_encode_us);
    snprintf(lines[6], sizeof lines[6], "usb    %lu sent %lu skip", (unsigned long)sent, (unsigned long)skipped);
    snprintf(lines[7], sizeof lines[7], "video  no decoded fields");
    snprintf(lines[8], sizeof lines[8], "bridge %s", bridge_active() ? "active" : "idle");
    unsigned mhz;
    const char *chan = decode_channel(&mhz);
    if (mhz) snprintf(lines[9], sizeof lines[9], "chan   %s %u MHz%s", chan, mhz, decode_running() ? "" : ", stopped");
    else snprintf(lines[9], sizeof lines[9], "chan   %s%s", chan, decode_running() ? "" : ", stopped");

    memset(s_raw, 0, s_raw_size);
    for (int i = 0; i < 10; ++i) {
        overlay_text(s_raw, VIDEO_WIDTH, 48, 48 + i * 5 * TEXT_HALVES, TEXT_HALVES, lines[i]);
    }
    /* A sweeping block makes dropped or repeated frames visible. */
    int span = VIDEO_WIDTH - MARKER;
    int x = (int)(seq * 8 % (2 * span));
    if (x > span) x = 2 * span - x;
    overlay_box(s_raw, VIDEO_WIDTH, x, VIDEO_HEIGHT - 2 * MARKER, MARKER, MARKER, 0xff);
}

/* Both source buffers are 720x480; a 4:2:0 frame is 3 bytes per two pixels, each line carrying one chroma sample. */
static void gray_to_yuv420(const uint8_t *gray)
{
    uint8_t *out = s_yuv420;
    for (int y = 0; y < VIDEO_HEIGHT; ++y) {
        const uint8_t *row = gray + y * VIDEO_WIDTH;
        for (int x = 0; x < VIDEO_WIDTH; x += 2) {
            *out++ = 128;
            *out++ = row[x];
            *out++ = row[x + 1];
        }
    }
    esp_cache_msync(s_yuv420, s_yuv420_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void encode(const uint8_t *raw, bool color, uint32_t seq, int64_t t_us)
{
    int64_t c0 = esp_timer_get_time();
    uint8_t *yuv = s_yuv420;
    if (color) yuv = (uint8_t *)raw;
    else gray_to_yuv420(raw);
    s_convert_us = esp_timer_get_time() - c0;
    int w = writable_slot();
    slot_t *s = &s_slots[w];
    esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = yuv, .len = s_yuv420_size}, .pts = (uint32_t)(t_us / 1000)};
    esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = s->base, .len = s->cap}};
    if (s_want_key) {
        s_want_key = false;
        esp_h264_enc_force_idr(&s_enc_param->base);
    }
    esp_h264_err_t err = esp_h264_enc_process(s_enc, &in, &out);
    if (err != ESP_H264_ERR_OK) {
        ++s_errors;
        s_last_error = err;
        return;
    }
    bool key = out.frame_type == ESP_H264_FRAME_TYPE_IDR;
    if (key) ++s_keyframes;
    taskENTER_CRITICAL(&s_lock);
    s->frame = (video_frame_t){.data = s->base, .len = out.length, .seq = seq, .key = key, .t_us = t_us};
    s_newest = w;
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_published);
}

static void video_task(void *arg)
{
    int64_t window_start = esp_timer_get_time();
    uint32_t window_frames = 0;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        uint32_t seq = s_frames + 1;
        int64_t t0 = esp_timer_get_time();
        const uint8_t *raw;
        bool field = bits & NOTIFY_FIELD;
        if (field) {
            raw = s_field[s_ready];
            s_last_field_us = t0;
        } else if (!decode_running() && t0 - s_last_field_us > PATTERN_AFTER_US && ++s_pattern_ticks % PATTERN_EVERY == 0) {
            render(seq);
            raw = s_raw;
        } else {
            continue;
        }
        if (!field) {
            gray_to_yuv420(raw);
            raw = s_yuv420;
        }
        lcd_offer(raw);
        int64_t t1 = esp_timer_get_time();
        if (!s_encode) continue;
        encode(raw, true, seq, t0);
        int64_t t2 = esp_timer_get_time();
        s_render_us = t1 - t0;
        s_encode_us = t2 - t1;
        s_frames = seq;

        ++window_frames;
        if (t2 - window_start >= 1000000) {
            s_fps_tenths = window_frames * 10000000LL / (t2 - window_start);
            window_start = t2;
            window_frames = 0;
        }
    }
}

void video_init(void)
{
    s_raw_size = VIDEO_WIDTH * VIDEO_HEIGHT;
    s_raw = heap_caps_aligned_calloc(128, 1, s_raw_size, MALLOC_CAP_SPIRAM);
    assert(s_raw);
    uint32_t actual;
    s_yuv420 = esp_h264_aligned_calloc(128, 1, VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2, &s_yuv420_size, ESP_H264_MEM_SPIRAM);
    assert(s_yuv420);
    for (int i = 0; i < 2; ++i) {
        uint32_t size;
        /* The decoder's previous-field copies read whole cache lines, a little past the last row. */
        s_field[i] = esp_h264_aligned_calloc(128, 1, VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2 + 1024, &size, ESP_H264_MEM_SPIRAM);
        assert(s_field[i]);
        /* The decoder's DMA fills these, so no cached copy may be written back over it. */
        esp_cache_msync(s_field[i], size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    }
    for (int i = 0; i < SLOTS; ++i) {
        s_slots[i].base = esp_h264_aligned_calloc(128, 1, SLOT_BYTES, &actual, ESP_H264_MEM_SPIRAM);
        assert(s_slots[i].base);
        s_slots[i].cap = actual;
    }

    const esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = H264_GOP,
        .fps = VIDEO_FPS,
        .res = {.width = VIDEO_WIDTH, .height = VIDEO_HEIGHT},
        .rc = {.bitrate = s_bitrate, .qp_min = H264_QP_MIN, .qp_max = H264_QP_MAX},
    };
    ESP_ERROR_CHECK(esp_h264_enc_hw_new(&cfg, &s_enc) == ESP_H264_ERR_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_h264_enc_open(s_enc) == ESP_H264_ERR_OK ? ESP_OK : ESP_FAIL);
    esp_h264_enc_hw_get_param_hd(s_enc, &s_enc_param);

    s_published = xSemaphoreCreateBinary();

    /* On core 1, below the demodulator: the encoder driver's cache write-back of each frame is the
     * biggest CPU cost here, and core 0 draws the decoder's lines. */
    xTaskCreatePinnedToCore(video_task, "video", 4096, NULL, 4, &s_task, 1);
    const esp_timer_create_args_t timer = {.callback = tick, .name = "video"};
    esp_timer_handle_t h;
    ESP_ERROR_CHECK(esp_timer_create(&timer, &h));
    ESP_ERROR_CHECK(esp_timer_start_periodic(h, 1000000 / VIDEO_FPS));
}

void video_request_keyframe(void)
{
    s_want_key = true;
}

uint8_t *video_field_buffer(void)
{
    return s_field[s_fill];
}

void video_field_done(void)
{
    s_ready = s_fill;
    s_fill ^= 1;
    xTaskNotify(s_task, NOTIFY_FIELD, eSetBits);
}

static bool take(video_reader_t who, video_frame_t *f, bool only_new)
{
    bool ok = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_newest >= 0 && (!only_new || s_slots[s_newest].frame.seq != s_taken[who])) {
        s_held[who] = s_newest;
        *f = s_slots[s_newest].frame;
        s_taken[who] = f->seq;
        ok = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

bool video_next(video_reader_t who, video_frame_t *f, TickType_t wait)
{
    TickType_t start = xTaskGetTickCount();
    while (!take(who, f, true)) {
        TickType_t waited = xTaskGetTickCount() - start;
        if (waited >= wait || xSemaphoreTake(s_published, wait - waited) != pdTRUE) return false;
    }
    return true;
}

bool video_newest(video_reader_t who, video_frame_t *f)
{
    return take(who, f, false);
}

void video_release(video_reader_t who)
{
    taskENTER_CRITICAL(&s_lock);
    s_held[who] = -1;
    taskEXIT_CRITICAL(&s_lock);
}

static void grab(void)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    video_frame_t f;
    if (!video_newest(VIDEO_READER_CONSOLE, &f)) {
        say("no frame yet\n");
        return;
    }
    const uint8_t *p = f.data;
    size_t len = f.len;
    say("-----BEGIN H264 %u-----\n", (unsigned)len);
    char line[80];
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < len ? p[i + 1] << 8 : 0) | (i + 2 < len ? p[i + 2] : 0);
        line[n++] = b64[v >> 18 & 63];
        line[n++] = b64[v >> 12 & 63];
        line[n++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        line[n++] = i + 2 < len ? b64[v & 63] : '=';
        if (n == 76 || i + 3 >= len) {
            line[n++] = '\n';
            host_write(line, n);
            n = 0;
        }
    }
    video_release(VIDEO_READER_CONSOLE);
    say("-----END H264-----\n");
}

static const char RAW_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The newest decoded frame before encoding, as base64 between "video raw begin" and "video raw end". */
static void raw(void)
{
    const size_t bytes = VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2;
    uint8_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    int ready = s_ready;
    if (!copy || ready < 0) {
        say("err %s\n", copy ? "no decoded frame yet" : "no memory");
        free(copy);
        return;
    }
    /* The decoder's DMA wrote it; drop any stale cached lines before reading. */
    esp_cache_msync(s_field[ready], (bytes + 127) & ~127u, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    memcpy(copy, s_field[ready], bytes);
    say("video raw begin %d %d packed420\n", VIDEO_WIDTH, VIDEO_HEIGHT);
    char line[77];
    for (size_t off = 0; off < bytes; off += 57) {
        size_t chunk = bytes - off < 57 ? bytes - off : 57, o = 0;
        for (size_t k = 0; k < chunk; k += 3) {
            uint32_t v = copy[off + k] << 16 | (k + 1 < chunk ? copy[off + k + 1] << 8 : 0) | (k + 2 < chunk ? copy[off + k + 2] : 0);
            line[o++] = RAW_B64[v >> 18 & 63];
            line[o++] = RAW_B64[v >> 12 & 63];
            line[o++] = k + 1 < chunk ? RAW_B64[v >> 6 & 63] : '=';
            line[o++] = k + 2 < chunk ? RAW_B64[v & 63] : '=';
        }
        line[o++] = '\n';
        host_write(line, o);
    }
    say("video raw end\n");
    free(copy);
}

void video_command(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "grab") == 0) {
        grab();
        return;
    }
    if (argc > 2 && strcmp(argv[1], "encode") == 0) {
        s_encode = strcmp(argv[2], "off") != 0;
        return;
    }
    if (argc > 1 && strcmp(argv[1], "raw") == 0) {
        raw();
        return;
    }
    if (argc > 2 && strcmp(argv[1], "bitrate") == 0) {
        uint32_t bps = strtoul(argv[2], NULL, 10);
        if (bps >= 500000 && esp_h264_enc_set_bitrate(&s_enc_param->base, bps) == ESP_H264_ERR_OK) s_bitrate = bps;
        return;
    }
    say("%lu frames at %lu.%lu fps, %lu errors", (unsigned long)s_frames, (unsigned long)(s_fps_tenths / 10),
        (unsigned long)(s_fps_tenths % 10), (unsigned long)s_errors);
    say(s_errors ? " (last error %d)\n" : "\n", (int)s_last_error);
    say("convert %lu us, render %lu us, encode %lu us (both include convert), h264 %u bytes at %lu bit/s, %lu keyframes\n", (unsigned long)s_convert_us, (unsigned long)s_render_us,
        (unsigned long)s_encode_us, (unsigned)newest_len(), (unsigned long)s_bitrate, (unsigned long)s_keyframes);
    uvc_info();
}
