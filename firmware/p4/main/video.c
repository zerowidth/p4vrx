#include "video.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "console.h"

#define FRAME_BYTES (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)

static uint8_t *s_frames[2];
static int s_filling;
static volatile uint32_t s_fields;
static int64_t s_first_us, s_last_us;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_lock;

void video_init(void)
{
    for (int k = 0; k < 2; ++k) s_frames[k] = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_done = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
}

uint8_t *video_field_buffer(void)
{
    return s_frames[s_filling];
}

void video_field_done(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_filling ^= 1;
    xSemaphoreGive(s_lock);
    int64_t now = esp_timer_get_time();
    if (!s_fields++) s_first_us = now;
    s_last_us = now;
    xSemaphoreGive(s_done);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The newest whole frame, copied out of the decoder's way, as base64 between "video grab begin" and "video grab end". */
static void grab(void)
{
    uint8_t *copy = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!copy) {
        say("err no memory\n");
        return;
    }
    xSemaphoreTake(s_done, 0);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        say("err no frame in 500 ms; is the decoder on?\n");
        free(copy);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(copy, s_frames[s_filling ^ 1], FRAME_BYTES);
    xSemaphoreGive(s_lock);
    say("video grab begin %d %d packed420\n", VIDEO_WIDTH, VIDEO_HEIGHT);
    char line[77];
    for (size_t off = 0; off < FRAME_BYTES; off += 57) {
        size_t chunk = FRAME_BYTES - off < 57 ? FRAME_BYTES - off : 57, o = 0;
        for (size_t k = 0; k < chunk; k += 3) {
            uint32_t v = copy[off + k] << 16 | (k + 1 < chunk ? copy[off + k + 1] << 8 : 0) | (k + 2 < chunk ? copy[off + k + 2] : 0);
            line[o++] = B64[v >> 18 & 63];
            line[o++] = B64[v >> 12 & 63];
            line[o++] = k + 1 < chunk ? B64[v >> 6 & 63] : '=';
            line[o++] = k + 2 < chunk ? B64[v & 63] : '=';
        }
        line[o++] = '\n';
        host_write(line, o);
    }
    say("video grab end\n");
    free(copy);
}

void video_command(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "grab") == 0) {
        grab();
        return;
    }
    double secs = (s_last_us - s_first_us) / 1e6;
    say("%lu frames, %.1f fps\n", (unsigned long)s_fields, s_fields > 1 && secs > 0 ? (s_fields - 1) / secs : 0.0);
}
