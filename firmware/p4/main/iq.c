// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/iq.c), by Twotoz and the C5VRX contributors.
#include "iq.h"

#include <stdlib.h>
#include <string.h>

#include "driver/parlio_rx.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/parlio_periph.h"
#include "soc/axi_dma_struct.h"
#include "soc/gpio_pins.h"

#include "console.h"
#include "pins.h"

#define BUS_HZ 40000000
#define MAX_SAMPLES (1600 * 1000)
/* The DMA keeps writing, circularly, until the task stops it; this absorbs that latency into PSRAM. */
#define MARGIN_BYTES (256 * 1024)
/* The soft delimiter's limit; with partial receive it only sets the EOF interrupt period. */
#define EOF_BYTES 0xfc00
#define SKIP 64

static uint16_t *s_buf;
static size_t s_len;
static SemaphoreHandle_t s_full;
static size_t s_want;
static volatile size_t s_got;

static bool IRAM_ATTR on_partial(parlio_rx_unit_handle_t rx, const parlio_rx_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    size_t before = s_got;
    s_got += e->recv_bytes;
    if (before < s_want && s_got >= s_want) xSemaphoreGiveFromISR(s_full, &woken);
    return woken == pdTRUE;
}

/* I7..I1 on data lines 7..1 and Q7..Q1 on 15..9; lines 0 and 8 read 1, so each byte is 2v+1. */
static esp_err_t new_rx(size_t max_recv_size, parlio_rx_unit_handle_t *ret)
{
    parlio_rx_unit_config_t cfg = {
        .trans_queue_depth = 1,
        .max_recv_size = max_recv_size,
        .data_width = 16,
        .clk_src = PARLIO_CLK_SRC_EXTERNAL,
        .ext_clk_freq_hz = BUS_HZ,
        .exp_clk_freq_hz = BUS_HZ,
        .clk_in_gpio_num = bus_pclk_gpio(),
        .clk_out_gpio_num = -1,
        .valid_gpio_num = -1,
        .flags.free_clk = 1,
    };
    for (int k = 0; k < 16; ++k) cfg.data_gpio_nums[k] = -1;
    for (int k = 1; k <= 7; ++k) {
        cfg.data_gpio_nums[k] = bus_lane_gpio(14 - k);     /* D0 (I1) .. D6 (I7) */
        cfg.data_gpio_nums[8 + k] = bus_lane_gpio(7 - k);  /* D7 (Q1) .. D13 (Q7) */
    }
    esp_err_t err = parlio_new_rx_unit(&cfg, ret);
    if (err != ESP_OK) return err;
    for (int k = 0; k < 16; k += 8) {
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, soc_parlio_signals[0].rx_units[0].data_sigs[k],
                                       false);
    }
    return ESP_OK;
}

/* A stopped receive channel still owes the descriptor it was on a write-back, which lands in the freed list
 * when the next receive starts, so the channel is reset before its list is freed. */
static esp_err_t del_rx(parlio_rx_unit_handle_t rx)
{
    for (int ch = 0; ch < 3; ++ch) {
        if (AXI_DMA.in[ch].conf.in_peri_sel.peri_in_sel_chn == 3) {
            AXI_DMA.in[ch].conf.in_conf0.in_rst_chn = 1;
            AXI_DMA.in[ch].conf.in_conf0.in_rst_chn = 0;
        }
    }
    return parlio_del_rx_unit(rx);
}

static esp_err_t capture(size_t samples, parlio_sample_edge_t edge)
{
    if (!s_buf) {
        s_buf = heap_caps_aligned_alloc(64, MAX_SAMPLES * 2 + MARGIN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        s_full = xSemaphoreCreateBinary();
        if (!s_buf) return ESP_ERR_NO_MEM;
    }
    parlio_rx_unit_handle_t rx = NULL;
    parlio_rx_delimiter_handle_t delim = NULL;
    esp_err_t err = new_rx(samples * 2 + MARGIN_BYTES, &rx);
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
        err = parlio_rx_unit_receive(rx, s_buf, samples * 2 + MARGIN_BYTES, &rcfg);
        if (err == ESP_OK) err = parlio_rx_soft_delimiter_start_stop(rx, delim, true);
        if (err == ESP_OK && xSemaphoreTake(s_full, pdMS_TO_TICKS(1000)) != pdTRUE) err = ESP_ERR_TIMEOUT;
        parlio_rx_soft_delimiter_start_stop(rx, delim, false);
        parlio_rx_unit_disable(rx);
    }
    if (delim) parlio_del_rx_delimiter(delim);
    del_rx(rx);
    if (err == ESP_OK && s_got > samples * 2 + MARGIN_BYTES) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        esp_cache_msync(s_buf, samples * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        s_len = samples;
    }
    return err;
}

const uint16_t *iq_samples(size_t *count)
{
    *count = s_len;
    return s_buf;
}

static void stats(void)
{
    int64_t sum_i = 0, sum_q = 0;
    uint64_t power = 0, rough = 0;
    int min_i = 127, max_i = -128, min_q = 127, max_q = -128;
    uint32_t clipped = 0, bad_ones = 0, toggles[16] = {0};
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
        uint16_t diff = s_buf[n] ^ s_buf[n - 1];
        for (int b = 0; b < 16; ++b) toggles[b] += diff >> b & 1;
        rough += abs(i - (int8_t)s_buf[n - 1]) + abs(q - (int8_t)(s_buf[n - 1] >> 8));
    }
    size_t n = s_len - SKIP;
    uint32_t rms = 0;
    for (uint64_t p = power / n; (uint64_t)(rms + 1) * (rms + 1) <= p; ++rms) {
    }
    say("%u samples: I %d..%d mean %d, Q %d..%d mean %d, rms %lu, clipped %lu, roughness %lu\n", (unsigned)n, min_i,
        max_i, (int)(sum_i / (int64_t)n), min_q, max_q, (int)(sum_q / (int64_t)n), (unsigned long)rms,
        (unsigned long)clipped, (unsigned long)(rough / n));
    /* Toggles per thousand samples for each lane, MSB first. */
    say("toggles/1000: Q7..Q1");
    for (int b = 15; b >= 9; --b) say(" %lu", (unsigned long)(toggles[b] * 1000ull / n));
    say("  I7..I1");
    for (int b = 7; b >= 1; --b) say(" %lu", (unsigned long)(toggles[b] * 1000ull / n));
    say("\n");
    if (bad_ones) say("constant-1 lines read low %lu times\n", (unsigned long)bad_ones);
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Sends samples as base64 lines between "iq dump begin N" and "iq dump end". */
static void dump(size_t count)
{
    if (count > s_len) count = s_len;
    say("iq dump begin %u\n", (unsigned)count);
    const uint8_t *p = (const uint8_t *)s_buf;
    size_t bytes = count * 2;
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

void iq_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "cap") == 0) {
        size_t samples = argc > 2 ? strtoul(argv[2], NULL, 0) : 262144;
        parlio_sample_edge_t edge = argc > 3 && strcmp(argv[3], "neg") == 0 ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS;
        if (samples < 1024 || samples > MAX_SAMPLES) {
            say("err samples 1024..%u\n", (unsigned)MAX_SAMPLES);
            return;
        }
        esp_err_t err = capture(samples, edge);
        if (err != ESP_OK) {
            say("err %s\n", esp_err_to_name(err));
            return;
        }
        stats();
    } else if (strcmp(sub, "dump") == 0) {
        dump(argc > 2 ? strtoul(argv[2], NULL, 0) : s_len);
    } else {
        say("iq cap [samples] [pos|neg] | dump [samples]\n");
    }
}
