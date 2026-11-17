// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX c5rx (reference/c5rx/main/main.c), by Twotoz and the C5VRX contributors.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "hal/misc.h"
#include "soc/pcr_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lanes.h"
#include "link.h"
#include "esp_timer.h"
#include "radio.h"
#include "sweep.h"

/* Waveshare C5-Zero antenna switch: low selects the on-board antenna, high the U.FL. */
#define ANTENNA_SEL_GPIO GPIO_NUM_26
#define CONSOLE_UART UART_NUM_0

static bool s_external;

static void antenna_set(bool external)
{
    s_external = external;
    gpio_set_level(ANTENNA_SEL_GPIO, external);
}

/* Saved here because the P4 resets the C5 on every decoder start. */
static void antenna_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t nvs;
    uint8_t external = 0;
    if (err == ESP_OK && nvs_open("c5rx", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "antenna", &external);
        nvs_close(nvs);
    }
    gpio_set_direction(ANTENNA_SEL_GPIO, GPIO_MODE_OUTPUT);
    antenna_set(external);
}

static esp_err_t antenna_save(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("c5rx", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "antenna", s_external);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static const char *antenna_name(void)
{
    return s_external ? "ext" : "int";
}

static int64_t s_line_at_us;
static uint32_t s_slowest_reply_us;

static void reply(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    uint32_t took = (uint32_t)(esp_timer_get_time() - s_line_at_us);
    if (took > s_slowest_reply_us) s_slowest_reply_us = took;
}

static uint32_t *s_uart_lines, *s_uart_junk;
static uint32_t s_pings;
/* Spare CPU: a lowest-priority task's loops in the last second, and the slowest line-to-reply time. */
static volatile uint32_t s_spin, s_spin_per_s;

static void spin_task(void *arg)
{
    int64_t window = esp_timer_get_time();
    for (;;) {
        ++s_spin;
        if (esp_timer_get_time() - window >= 1000000) {
            s_spin_per_s = s_spin;
            s_spin = 0;
            window = esp_timer_get_time();
        }
    }
}

/* Strips the P4's " #hh" check byte, the XOR of the command's bytes; false if it's missing or wrong. */
static bool checked(char *line)
{
    char *mark = strrchr(line, '#');
    if (!mark || mark == line || mark[-1] != ' ' || strlen(mark) != 3) return false;
    char *end;
    unsigned long want = strtoul(mark + 1, &end, 16);
    if (*end) return false;
    mark[-1] = '\0';
    uint8_t check = 0;
    for (const char *p = line; *p; ++p) check ^= (uint8_t)*p;
    return check == want;
}

static uint32_t s_bad_lines;

static void run_command(char *line)
{
    if (!*line) return;
    if (!checked(line)) {
        ++s_bad_lines;
        reply("err check");
        return;
    }
    char *argv[52];
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 52; tok = strtok(NULL, " ")) argv[argc++] = tok;
    if (argc == 0) return;

    if (strcmp(argv[0], "ping") == 0) {
        ++s_pings;
        reply("ok c5rx");
    } else if (strcmp(argv[0], "link") == 0 && argc == 2 && strcmp(argv[1], "off") == 0) {
        link_stop();
        reply("ok link off");
    } else if (strcmp(argv[0], "link") == 0 && argc == 3) {
        radio_iq_stop();
        esp_err_t err = link_start(argv[1][0], (uint32_t)atoi(argv[2]) * 1000000u);
        if (err == ESP_OK) reply("ok link %s", argv[1]);
        else reply("err %s", esp_err_to_name(err));
    } else if (strcmp(argv[0], "tune") == 0 && argc == 2) {
        esp_err_t err = radio_tune(argv[1]);
        if (err == ESP_OK) reply("ok tune %s %u", radio_channel(), radio_mhz());
        else reply("err %s", esp_err_to_name(err));
    } else if (strcmp(argv[0], "gain") == 0 && argc == 2) {
        radio_set_gain((uint8_t)atoi(argv[1]));
        reply("ok gain %u", radio_gain());
    } else if (strcmp(argv[0], "iq") == 0 && (argc == 2 || argc == 3 || argc == 5) && strcmp(argv[1], "on") == 0) {
        link_stop();
        int every = argc >= 3 ? atoi(argv[2]) : 2;
        int q_top = argc == 5 ? atoi(argv[3]) : 9, i_top = argc == 5 ? atoi(argv[4]) : 19;
        esp_err_t err = radio_iq_start(every, q_top, i_top);
        if (err == ESP_OK) reply("ok iq on %d %d %d", every, q_top, i_top);
        else reply("err %s", esp_err_to_name(err));
    } else if (strcmp(argv[0], "iq") == 0 && argc == 2 && strcmp(argv[1], "off") == 0) {
        radio_iq_stop();
        reply("ok iq off");
    } else if (strcmp(argv[0], "clk") == 0 && argc == 3 && strcmp(argv[1], "slip") == 0) {
        radio_clock_slip(atoi(argv[2]));
        reply("ok clk slip %d", atoi(argv[2]));
    } else if (strcmp(argv[0], "antenna") == 0 && argc == 1) {
        reply("ok antenna %s", antenna_name());
    } else if (strcmp(argv[0], "antenna") == 0 && argc == 2 &&
               (strcmp(argv[1], "int") == 0 || strcmp(argv[1], "ext") == 0)) {
        bool external = argv[1][0] == 'e';
        esp_err_t err = ESP_OK;
        if (external != s_external) {
            antenna_set(external);
            err = antenna_save();
        }
        if (err == ESP_OK) reply("ok antenna %s", antenna_name());
        else reply("err antenna %s not saved: %s", antenna_name(), esp_err_to_name(err));
    } else if (strcmp(argv[0], "clk") == 0 && argc == 3 && strcmp(argv[1], "slip") != 0) {
        PCR.parl_clk_tx_conf.parl_clk_tx_sel = atoi(argv[1]);
        HAL_FORCE_MODIFY_U32_REG_FIELD(PCR.parl_clk_tx_conf, parl_clk_tx_div_num, atoi(argv[2]) - 1);
        reply("ok clk parl_tx=%08lx", (unsigned long)PCR.parl_clk_tx_conf.val);
    } else if (strcmp(argv[0], "drive") == 0 && argc == 3) {
        int data = atoi(argv[1]), clk = atoi(argv[2]);
        radio_set_drive(data, clk);
        reply("ok drive %d %d", data, clk);
    } else if (strcmp(argv[0], "clk") == 0 && argc == 1) {
        uint32_t xtal = 0, f240 = 0, cpu = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &xtal);
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_PLL_F240M, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &f240);
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpu);
        reply("ok clk xtal=%lu f240=%lu cpu=%lu parl_tx=%08lx", (unsigned long)xtal, (unsigned long)f240, (unsigned long)cpu,
              (unsigned long)PCR.parl_clk_tx_conf.val);
    } else if (strcmp(argv[0], "hoptime") == 0 && argc >= 2) {
        uint32_t a0, a1, b0, b1;
        radio_hop_time((uint16_t)atoi(argv[1]), argc > 2 ? atoi(argv[2]) : 20, argc > 3 ? atoi(argv[3]) : 0, &a0, &a1, &b0, &b1);
        reply("ok hoptime out %lu-%lu back %lu-%lu us", (unsigned long)a0, (unsigned long)a1, (unsigned long)b0, (unsigned long)b1);
    } else if (strcmp(argv[0], "uart") == 0) {
        reply("ok uart lines=%lu junk=%lu pings=%lu bad=%lu spare=%lu slowest=%luus", (unsigned long)*s_uart_lines, (unsigned long)*s_uart_junk,
              (unsigned long)s_pings, (unsigned long)s_bad_lines, (unsigned long)s_spin_per_s, (unsigned long)s_slowest_reply_us);
        s_slowest_reply_us = 0;
    } else if (strcmp(argv[0], "renew") == 0) {
        reply(sweep_renew() ? "ok renew" : "err renew: no scan");
    } else if (strcmp(argv[0], "sweep") == 0) {
        int n = sweep_set((const char *const *)argv + 1, argc - 1);
        uint32_t hops, passes;
        sweep_counts(&hops, &passes);
        if (n < 0) reply("err sweep");
        else reply("ok sweep %d, %lu hops, %lu passes", n, (unsigned long)hops, (unsigned long)passes);
    } else if (strcmp(argv[0], "walk") == 0 && argc == 2 && strcmp(argv[1], "off") == 0) {
        radio_iq_stop();
        reply("ok walk off");
    } else if (strcmp(argv[0], "walk") == 0 && argc == 2) {
        unsigned pattern = (unsigned)strtoul(argv[1], NULL, 16) & 0x3fff;
        esp_err_t err = radio_walk((uint16_t)pattern);
        if (err == ESP_OK) reply("ok walk %04x", pattern);
        else reply("err %s", esp_err_to_name(err));
    } else if (strcmp(argv[0], "status") == 0) {
        int rssi = 0, noise = 0;
        bool have_rssi = radio_rssi(&rssi), have_noise = radio_noise_floor(&noise);
        reply("ok status ch=%s freq=%u gain=%u rssi=%d%s noise=%d%s ant=%s", radio_channel(), radio_mhz(), radio_gain(),
              rssi, have_rssi ? "" : "?", noise, have_noise ? "" : "?", antenna_name());
    } else {
        reply("err unknown command %s", argv[0]);
    }
}

void app_main(void)
{
    /* GPIO 13/14 carry I2 and I1; their pads must leave the USB PHY. */
    usb_serial_jtag_ll_enable_bus_clock(true);
    usb_serial_jtag_ll_phy_enable_pad(false);
    antenna_load();

    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    /* Lane noise keeps the line from going idle, so the idle timeout never fires and a command would wait in
     * the FIFO for 120 bytes; take every byte as it comes. */
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(CONSOLE_UART, 1));
    xTaskCreate(spin_task, "spin", 2048, NULL, tskIDLE_PRIORITY, NULL);
    lanes_drive_low();
    printf("c5rx ready\n");

    char line[200];
    size_t len = 0;
    /* The UART picks up bytes from the lanes beside it, most of them not printable; those are skipped. */
    static uint32_t lines, junk;
    s_uart_lines = &lines, s_uart_junk = &junk;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            if (len) ++lines;
            s_line_at_us = esp_timer_get_time();
            run_command(line);
            len = 0;
        } else if (c >= 0x20 && c <= 0x7e && len < sizeof line - 1) {
            line[len++] = (char)c;
        } else if (c < 0x20 || c > 0x7e) {
            ++junk;
        }
    }
}
