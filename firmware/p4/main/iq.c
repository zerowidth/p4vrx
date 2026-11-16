// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/iq.c), by Twotoz and the C5VRX contributors.
#include "iq.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/parlio_rx.h"
#include "esp_private/gdma_link.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/parlio_periph.h"
#include "soc/axi_dma_struct.h"
#include "soc/gpio_pins.h"
#include "soc/gpio_sig_map.h"

#include "c5.h"
#include "console.h"
#include "decode.h"
#include "pins.h"

#define BUS_HZ 80000000
/* Two NTSC fields at 40 MS/s, so a whole field is in every capture. */
#define CAPTURE_SAMPLES (1400 * 1000)
/* The most `iq N` takes, 200 ms at 40 MS/s. The receive driver keeps one 32-byte descriptor per 4 KB of buffer,
 * and __wrap_gdma_new_link_list puts a long capture's in PSRAM, since internal RAM held only 2.4M samples' worth. */
#define MAX_SAMPLES 8000000
#define CAPTURE_BYTES (MAX_SAMPLES * 2)
/* The receive call puts 16 bytes of descriptor list per 4 KB on its caller's stack. */
#define BIG_STACK (256 * 1024)
/* The DMA keeps writing, circularly, until the task stops it; this absorbs that latency, which into
 * PSRAM has exceeded 64 KB. */
#define MARGIN_BYTES (64 * 1024)
#define PSRAM_MARGIN_BYTES (256 * 1024)
/* The soft delimiter's limit; with partial receive it only sets the EOF interrupt period. */
#define EOF_BYTES 0xfc00
/* Enough to compare edges: about 100 lines. */
#define PROBE_SAMPLES (256 * 1024)
#define SKIP 64

static uint16_t *s_psram;
/* The buffer the last capture went to. */
static uint16_t *s_buf;
static size_t s_len;
static parlio_sample_edge_t s_edge = PARLIO_SAMPLE_EDGE_POS;
/* The C5 keeps one of every s_every samples of its 80 MS/s bus. */
static int s_every = 2;
static char s_tuned[8];
static unsigned s_mhz;
static SemaphoreHandle_t s_full;
static size_t s_want;
static size_t s_got;
/* The companded phase table bs_phase.bsasm looks up (its layout is there): 32 log-spaced magnitude codes a lane. */
#define BS_CODES 32
#define BS_KNEE 6.0f
#define BS_MAG_MAX 160.0f

static float bs_code_scale(void)
{
    return BS_CODES / log2f(1 + BS_MAG_MAX / BS_KNEE);
}

/* A lane's 7 bits as wired, around its offset, to a magnitude code with the sign in bit 5. */
static uint8_t bs_lane_entry(int bits, float dc)
{
    float v = 2 * ((int8_t)(bits << 1) >> 1) + 1 - dc;
    int code = (int)(bs_code_scale() * log2f(1 + fabsf(v) / BS_KNEE));
    int raw = 2 * ((int8_t)(bits << 1) >> 1) + 1;
    return (code >= BS_CODES ? BS_CODES - 1 : code) | (v < 0 ? 0x20 : 0) | (raw >= 125 || raw <= -125 ? 0x40 : 0);
}

float iq_bs_code_centre(int code)
{
    float lo = BS_KNEE * (exp2f(code / bs_code_scale()) - 1), hi = BS_KNEE * (exp2f((code + 1) / bs_code_scale()) - 1);
    return (lo + hi) / 2;
}

void iq_bs_build_lut(uint16_t *lut, float dc_i, float dc_q)
{
    for (int k = 0; k < 1024; ++k) {
        int angle = (int)lrintf(atan2f(iq_bs_code_centre(k & 31), iq_bs_code_centre(k >> 5)) * (float)(64 / M_PI_2));
        lut[k] = (angle > 63 ? 63 : angle) << 8;
    }
    /* Q's table comes twice, for I positive and negative: bit 5 is set where the signs differ (the odd
     * quadrants) and bit 7 is Q's sign. */
    for (int k = 0; k < 128; ++k) {
        uint8_t q = bs_lane_entry(k, dc_q), neg = q & 0x20;
        lut[k] |= bs_lane_entry(k, dc_i);
        lut[128 + k] |= q | neg << 2;
        lut[384 + k] |= (q ^ 0x20) | neg << 2;
    }
}

static bool IRAM_ATTR on_partial(parlio_rx_unit_handle_t rx, const parlio_rx_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    size_t before = s_got;
    s_got += e->recv_bytes;
    if (before < s_want && s_got >= s_want) xSemaphoreGiveFromISR(s_full, &woken);
    return woken == pdTRUE;
}

esp_err_t iq_new_rx(size_t max_recv_size, parlio_rx_unit_handle_t *ret)
{
    parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1,
        .max_recv_size = max_recv_size,
        .data_width = 16,
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = BUS_HZ / s_every,
        .exp_clk_freq_hz = BUS_HZ / s_every,
        .clk_in_gpio_num = bus_pclk_gpio(),
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .flags.free_clk = 1,
    };
    for (int k = 0; k < 16; ++k) cfg.data_gpio_nums[k] = -1;
    for (int k = 1; k <= 7; ++k) {
        cfg.data_gpio_nums[k] = bus_lane_gpio(14 - k);    /* D0 (I1) .. D6 (I7) */
        cfg.data_gpio_nums[8 + k] = bus_lane_gpio(7 - k); /* D7 (Q1) .. D13 (Q7) */
    }
    esp_err_t err = parlio_new_rx_unit(&cfg, ret);
    if (err != ESP_OK) return err;
    for (int k = 0; k < 16; k += 8) {
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                       soc_parlio_signals[0].rx_units[0].data_sigs[k], false);
    }
    return ESP_OK;
}

/* A stopped receive channel still owes the descriptor it was on a write-back, which lands in the freed list
 * when the next receive starts, so the channel is reset before its list is freed. */
esp_err_t iq_del_rx(parlio_rx_unit_handle_t rx)
{
    for (int ch = 0; ch < 3; ++ch) {
        if (AXI_DMA.in[ch].conf.in_peri_sel.peri_in_sel_chn == 3) {
            AXI_DMA.in[ch].conf.in_conf0.in_rst_chn = 1;
            AXI_DMA.in[ch].conf.in_conf0.in_rst_chn = 0;
        }
    }
    return parlio_del_rx_unit(rx);
}

static bool s_ext_descriptors;

esp_err_t __real_gdma_new_link_list(const gdma_link_list_config_t *config, gdma_link_list_handle_t *ret);

esp_err_t __wrap_gdma_new_link_list(const gdma_link_list_config_t *config, gdma_link_list_handle_t *ret)
{
    gdma_link_list_config_t cfg = *config;
    if (s_ext_descriptors) cfg.flags.items_in_ext_mem = 1;
    return __real_gdma_new_link_list(&cfg, ret);
}

/* Same line map as the link test: I in the low byte, Q in the high byte, lines 0 and 8 at 1. */
static esp_err_t capture_here(uint16_t *buf, size_t samples, parlio_sample_edge_t edge)
{
    s_buf = buf;
    const size_t margin = (buf >= s_psram && buf < s_psram + MAX_SAMPLES) ? PSRAM_MARGIN_BYTES : MARGIN_BYTES;
    parlio_rx_unit_handle_t rx = NULL;
    parlio_rx_delimiter_handle_t delim = NULL;
    s_ext_descriptors = samples > CAPTURE_SAMPLES;
    esp_err_t err = iq_new_rx(samples * 2 + margin, &rx);
    s_ext_descriptors = false;
    if (err != ESP_OK) return err;
    const parlio_rx_soft_delimiter_config_t dcfg = {
        .sample_edge = edge,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
        .eof_data_len = EOF_BYTES,
    };
    const parlio_rx_event_callbacks_t cbs = {.on_partial_receive = on_partial};
    s_want = samples * 2;
    s_got = 0;
    xSemaphoreTake(s_full, 0);
    err = parlio_new_rx_soft_delimiter(&dcfg, &delim);
    if (err == ESP_OK) err = parlio_rx_unit_register_event_callbacks(rx, &cbs, NULL);
    if (err == ESP_OK) err = parlio_rx_unit_enable(rx, true);
    if (err == ESP_OK) {
        const parlio_receive_config_t rcfg = {.delimiter = delim, .flags.partial_rx_en = 1};
        err = parlio_rx_unit_receive(rx, s_buf, samples * 2 + margin, &rcfg);
        if (err == ESP_OK) err = parlio_rx_soft_delimiter_start_stop(rx, delim, true);
        if (err == ESP_OK && xSemaphoreTake(s_full, pdMS_TO_TICKS(1000)) != pdTRUE) err = ESP_ERR_TIMEOUT;
        parlio_rx_soft_delimiter_start_stop(rx, delim, false);
        parlio_rx_unit_disable(rx);
    }
    if (delim) parlio_del_rx_delimiter(delim);
    iq_del_rx(rx);
    if (err == ESP_OK && s_got > samples * 2 + margin) {
        say("iq: DMA wrapped %u bytes past the end\n", (unsigned)(s_got - samples * 2 - margin));
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        esp_cache_msync(s_buf, samples * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        s_len = samples;
    }
    return err;
}

typedef struct {
    uint16_t *buf;
    size_t samples;
    parlio_sample_edge_t edge;
    esp_err_t err;
    TaskHandle_t waiter;
} capture_job_t;

static void capture_task(void *arg)
{
    capture_job_t *job = arg;
    job->err = capture_here(job->buf, job->samples, job->edge);
    xTaskNotifyGive(job->waiter);
    vTaskDelete(NULL);
}

/* A long capture's descriptor list outgrows the console task's stack, so it runs on one in PSRAM. The task and
 * its stack are made once, since a task made with caps is not freed by its own exit. */
static esp_err_t capture(uint16_t *buf, size_t samples, parlio_sample_edge_t edge)
{
    static StackType_t *stack;
    static StaticTask_t tcb;
    if (samples <= CAPTURE_SAMPLES) return capture_here(buf, samples, edge);
    if (!stack) stack = heap_caps_malloc(BIG_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) {
        say("iq: no memory for the capture task's stack\n");
        return ESP_ERR_NO_MEM;
    }
    capture_job_t job = {buf, samples, edge, ESP_FAIL, xTaskGetCurrentTaskHandle()};
    xTaskCreateStatic(capture_task, "iqcap", BIG_STACK / sizeof(StackType_t), &job, 5, stack, &tcb);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.err;
}

/* An edge that lands on a bus transition mixes two samples' bits, which shows as large jumps. */
static uint32_t roughness(size_t samples)
{
    uint64_t sum = 0;
    for (size_t n = SKIP + 1; n < samples; ++n) {
        sum += abs((int8_t)s_buf[n] - (int8_t)s_buf[n - 1]);
        sum += abs((int8_t)(s_buf[n] >> 8) - (int8_t)(s_buf[n - 1] >> 8));
    }
    return (uint32_t)(sum * 100 / (samples - SKIP - 1));
}

static void stats(void)
{
    int64_t sum_i = 0, sum_q = 0;
    uint64_t power = 0;
    int min_i = 127, max_i = -128, min_q = 127, max_q = -128;
    uint32_t clipped = 0, bad_ones = 0;
    for (size_t n = SKIP; n < s_len; ++n) {
        int i = (int8_t)s_buf[n], q = (int8_t)(s_buf[n] >> 8);
        sum_i += i;
        sum_q += q;
        power += i * i + q * q;
        if (i < min_i) min_i = i;
        if (i > max_i) max_i = i;
        if (q < min_q) min_q = q;
        if (q > max_q) max_q = q;
        if (i <= -127 || i >= 127 || q <= -127 || q >= 127) ++clipped;
        if ((s_buf[n] & 0x0101) != 0x0101) ++bad_ones;
    }
    size_t n = s_len - SKIP;
    uint32_t rms = 0;
    for (uint64_t p = power / n; (uint64_t)(rms + 1) * (rms + 1) <= p; ++rms) {
    }
    say("%u samples: I %d..%d mean %d, Q %d..%d mean %d, rms %lu, clipped %lu, roughness %lu.%02lu\n",
        (unsigned)n, min_i, max_i, (int)(sum_i / (int64_t)n), min_q, max_q, (int)(sum_q / (int64_t)n),
        (unsigned long)rms, (unsigned long)clipped, (unsigned long)(roughness(s_len) / 100),
        (unsigned long)(roughness(s_len) % 100));
    if (bad_ones) say("constant-1 lines read low %lu times\n", (unsigned long)bad_ones);
}

/* A PSRAM capture's first 64,512 bytes hold the capture before it. */
#define STALE 32256
/* Enough fresh samples to set the gain by. */
#define GAIN_SAMPLES (STALE + 65536)
#define PROBE_GAIN_MIN 22
#define PROBE_GAIN_MAX 70

static int s_probe_gain = 52;

typedef struct {
    uint32_t rms, clip;
    /* Per mille of samples within half the mean power of it: most of an FM carrier's, 4 in 10 of noise's. */
    uint32_t ring;
    /* Power the fourth difference passes, over the total, each less its share of rounding. */
    float high;
    int mean[2];
} quality_t;

/* The fourth difference's power gain for white noise, and the rounding's variance on both lanes (2v+1 steps). */
#define DIFF4_GAIN 70.0
#define ROUNDING (2.0 / 3)

/* A lane sampled while the C5's bus is changing takes some bits from one sample and some from the next. At
 * 40 MS/s the receive filter leaves nothing above about 10 MHz, so a clean read has almost no power there
 * and a mixed one spreads it evenly: a carrier read 0.02 to 0.05 clean and 19 to 44 mixed, noise under 7
 * and over 25. */
static quality_t quality(size_t samples)
{
    quality_t q = {0};
    size_t n = samples - STALE;
    uint64_t power = 0;
    double total = 0, high = 0;
    int *means = q.mean;
    for (int lane = 0; lane < 2; ++lane) {
        int shift = lane * 8;
        int64_t sum = 0, sq = 0;
        uint64_t diff = 0;
        for (size_t k = STALE; k < samples; ++k) {
            int v = (int8_t)(s_buf[k] >> shift);
            sum += v;
            sq += v * v;
            if (v >= 125 || v <= -125) ++q.clip;
            if (k >= STALE + 4) {
                int d = v - 4 * (int8_t)(s_buf[k - 1] >> shift) + 6 * (int8_t)(s_buf[k - 2] >> shift) -
                        4 * (int8_t)(s_buf[k - 3] >> shift) + (int8_t)(s_buf[k - 4] >> shift);
                diff += (uint32_t)(d * d);
            }
        }
        int mean = means[lane] = sum / (int64_t)n;
        power += sq / n - (int64_t)mean * mean;
        total += (double)sq / n - ((double)sum / n) * ((double)sum / n);
        high += (double)diff / (n - 4);
    }
    /* Under a step of signal there is only rounding to judge by. */
    q.high = total > 3 * ROUNDING ? (float)((high - DIFF4_GAIN * ROUNDING) / (total - ROUNDING)) : 0;
    while ((uint64_t)(q.rms + 1) * (q.rms + 1) <= power) ++q.rms;
    uint32_t near = 0;
    for (size_t k = STALE; k < samples; ++k) {
        int i = (int8_t)s_buf[k] - means[0], v = (int8_t)(s_buf[k] >> 8) - means[1];
        uint32_t p = i * i + v * v;
        near += p > power / 2 && p < power * 3 / 2;
    }
    q.ring = (uint32_t)((uint64_t)near * 1000 / n);
    q.clip = (uint32_t)((uint64_t)q.clip * 1000 / n);
    return q;
}

static bool set_probe_gain(int gain)
{
    char cmd[16], reply[48];
    s_probe_gain = gain;
    snprintf(cmd, sizeof cmd, "gain %d", gain);
    /* Right after `iq on` the C5 has taken over half a second to answer. */
    return c5_request(cmd, reply, sizeof reply, 2000);
}

/* The C5 starts at a gain that clips a near transmitter flat, which hides everything a probe looks for. */
static bool settle_gain(void)
{
    for (int pass = 0; pass < 8; ++pass) {
        if (capture(s_psram, GAIN_SAMPLES, s_edge) != ESP_OK) return false;
        quality_t q = quality(GAIN_SAMPLES);
        int gain = s_probe_gain;
        if (q.clip > 20 || q.rms > 80) gain -= 6;
        else if (q.rms < 30) gain += 6;
        gain = gain < PROBE_GAIN_MIN ? PROBE_GAIN_MIN : gain > PROBE_GAIN_MAX ? PROBE_GAIN_MAX : gain;
        if (gain == s_probe_gain) break;
        if (!set_probe_gain(gain)) return false;
    }
    return true;
}

#define HIGH_MAX 12.0f
#define RING_MIN 600

static int s_dc[2];

/* Captures on the rising edge and reports whether it reads the lanes clean. The Nano re-registers the bus, so
 * the P4's falling edge falls on its output's changes; what a capture judges is where the Nano's register
 * samples the C5's bus. */
static bool pick_edge(bool *clean)
{
    esp_err_t err = capture(s_psram, PROBE_SAMPLES, PARLIO_SAMPLE_EDGE_POS);
    if (err != ESP_OK) {
        say("iq: capture failed, %s\n", esp_err_to_name(err));
        return false;
    }
    quality_t q = quality(PROBE_SAMPLES);
    *clean = q.high <= HIGH_MAX;
    s_edge = PARLIO_SAMPLE_EDGE_POS;
    s_dc[0] = q.mean[0];
    s_dc[1] = q.mean[1];
    say("power past the receive filter over the total %.2f; gain %d, RMS %lu%s%s\n", (double)q.high, s_probe_gain,
        (unsigned long)q.rms, q.ring >= RING_MIN ? "" : ", no carrier", *clean ? "" : ", read mid-change");
    return true;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The capture as base64 lines between "iq dump begin N" and "iq dump end". */
static void dump(void)
{
    if (!s_len) {
        say("iq: nothing captured\n");
        return;
    }
    say("iq dump begin %u\n", (unsigned)s_len);
    const uint8_t *p = (const uint8_t *)s_buf;
    size_t bytes = s_len * 2;
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
    say("iq dump end\n");
}

const uint16_t *iq_samples(size_t *count)
{
    *count = s_len;
    return s_buf;
}

static bool prepare(void)
{
    if (!s_psram) {
        s_psram = heap_caps_aligned_calloc(128, 1, CAPTURE_BYTES + PSRAM_MARGIN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        s_full = xSemaphoreCreateBinary();
        if (!s_psram) {
            say("iq: no memory\n");
            return false;
        }
    }
    /* Lines 0 and 8 have no pin, which the driver warns about on every capture. */
    esp_log_level_set("parlio", ESP_LOG_ERROR);
    return true;
}

static void note_tune(const char *reply, const char *asked)
{
    /* A bare frequency tunes with no label, so fall back to what was asked for. */
    if (sscanf(reply, "ok tune %7s %u", s_tuned, &s_mhz) != 2) {
        s_mhz = (unsigned)atoi(reply + 8);
        strlcpy(s_tuned, asked, sizeof s_tuned);
    }
}

static bool start_c5(const char *channel, int every)
{
    char cmd[24], reply[96];
    if (!c5_run_c5rx(2000)) {
        say("iq: c5rx did not start\n");
        return false;
    }
    snprintf(cmd, sizeof cmd, "tune %s", channel);
    char iq_on[16];
    snprintf(iq_on, sizeof iq_on, "iq on %d", every);
    s_every = every;
    const char *steps[] = {cmd, iq_on};
    for (int i = 0; i < 2; ++i) {
        if (!c5_request(steps[i], reply, sizeof reply, 5000)) {
            say("iq: %s: %s\n", steps[i], reply);
            return false;
        }
        say("%s\n", reply);
        if (i == 0) note_tune(reply, channel);
    }
    return true;
}

bool iq_retune(const char *channel)
{
    char cmd[24], reply[96];
    snprintf(cmd, sizeof cmd, "tune %s", channel);
    if (!c5_request(cmd, reply, sizeof reply, 5000)) {
        say("iq: %s: %s\n", cmd, reply);
        return false;
    }
    note_tune(reply, channel);
    return true;
}

/* The probe's rate, at which the receive filter leaves the top of the band empty. */
#define PROBE_EVERY 2

/* How the C5's clock edge falls against its bus is settled at its reset, and wrong one time in three with
 * the 802.11p setting and two in three without. Running the clock a tick slow for a moment moves the edge
 * some ticks later, so this slips it until both of the P4's edges read clean. */
#define SLIP_TRIES 16

static bool place_clock(void)
{
    bool clean = false;
    for (int try = 0; try < SLIP_TRIES; ++try) {
        if (!pick_edge(&clean)) return false;
        if (clean) return true;
        char reply[64];
        if (!c5_request("clk slip 1", reply, sizeof reply, 500)) {
            say("iq: the C5 can't slip its clock: %s\n", reply);
            return true;
        }
    }
    say("iq: no clean clock position found\n");
    return true;
}

bool iq_start(const char *channel, int every)
{
    if (!prepare()) return false;
    decode_stop();
    bus_enable(true);
    if (!start_c5(channel, PROBE_EVERY) || !set_probe_gain(s_probe_gain) || !settle_gain() || !place_clock()) return false;
    if (every == PROBE_EVERY) return true;
    /* The clock keeps its place against the bus across a change of rate. */
    char cmd[16], reply[64];
    snprintf(cmd, sizeof cmd, "iq on %d", every);
    if (!c5_request(cmd, reply, sizeof reply, 5000)) {
        say("iq: %s: %s\n", cmd, reply);
        return false;
    }
    s_every = every;
    return true;
}

int iq_gain(void)
{
    return s_probe_gain;
}

void iq_dc(float *i, float *q)
{
    *i = s_dc[0];
    *q = s_dc[1];
}

const char *iq_channel(unsigned *mhz)
{
    *mhz = s_mhz;
    return s_tuned;
}

parlio_sample_edge_t iq_edge(void)
{
    return s_edge;
}

void iq_set_edge(parlio_sample_edge_t edge)
{
    s_edge = edge;
}

void iq_command(int argc, char **argv)
{
    if (!prepare()) return;
    const char *sub = argc > 1 ? argv[1] : "";
    /* The decoder owns the one PARLIO RX unit while it runs. */
    if (strcmp(sub, "dump") != 0) decode_stop();
    if (strcmp(sub, "start") == 0) {
        iq_start(argc > 2 ? argv[2] : "R4", argc > 3 ? atoi(argv[3]) : 2);
    } else if (strcmp(sub, "edge") == 0) {
        bool clean;
        pick_edge(&clean);
    } else if (strcmp(sub, "dump") == 0) {
        dump();
    } else if (strcmp(sub, "") == 0 || atoi(sub) > 0) {
        size_t n = atoi(sub) > 0 && atoi(sub) <= MAX_SAMPLES ? atoi(sub) : CAPTURE_SAMPLES;
        bus_enable(true);
        esp_err_t err = capture(s_psram, n, s_edge);
        if (err == ESP_OK) stats();
        else say("iq: capture failed, %s\n", esp_err_to_name(err));
    } else {
        say("iq [samples] | start [channel [every]] | edge | dump\n");
    }
}
