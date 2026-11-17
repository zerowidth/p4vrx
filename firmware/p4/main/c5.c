// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/c5.c), by Twotoz and the C5VRX contributors.
#include "c5.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bridge.h"
#include "console.h"
#include "pins.h"

#define C5_BAUD 115200
/* The C5's EN has 10k and 1 uF, so it takes about 10 ms to rise after release. */
#define EN_LOW_MS 50
#define BOOT_AFTER_EN_MS 100

/* Off by default: unknown C5 firmware may drive GPIO 11 with anything. */
static volatile bool s_log;
/* Set once c5rx announces itself after a reset, so GPIO 12 is known to be its UART RX. */
static volatile bool s_c5rx;
static SemaphoreHandle_t s_ready;
static SemaphoreHandle_t s_reply;
static SemaphoreHandle_t s_request;
static char s_reply_line[96];
/* Whether c5_request turns the Nano's bus off for each exchange: even on twisted pairs, commands sent while
 * the bus ran were lost often enough to fail the decoder's start. */
static bool s_pause_bus = true;

/* Level 1 releases the pin (high-Z); the C5's own pull-ups hold EN and BOOT high. */
static void open_drain_released(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}

/* Noise on the UART can put stray characters ahead of a reply, so a reply is found where it starts in the line. */
static const char *reply_start(const char *line)
{
    if (strncmp(line, "ok", 2) == 0 || strncmp(line, "err", 3) == 0) return line;
    const char *ok = strstr(line, "ok "), *err = strstr(line, "err ");
    return ok && (!err || ok < err) ? ok : err;
}

static void on_line(const char *line)
{
    const char *reply;
    if (strstr(line, "c5rx ready")) {
        c5_tx_attach();
        s_c5rx = true;
        xSemaphoreGive(s_ready);
    } else if ((reply = reply_start(line))) {
        strlcpy(s_reply_line, reply, sizeof s_reply_line);
        xSemaphoreGive(s_reply);
    }
}

/* What the C5's UART brought in: lines, bytes that weren't printable, and lines that weren't a reply or c5rx's banner. */
static uint32_t s_rx_lines, s_rx_junk, s_rx_other;

static void split_lines(const uint8_t *buf, size_t n)
{
    static char line[96];
    static size_t len;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            line[len] = '\0';
            if (len > 0) {
                ++s_rx_lines;
                if (!strstr(line, "ok ") && !strstr(line, "err") && !strstr(line, "c5rx")) ++s_rx_other;
                on_line(line);
            }
            len = 0;
        } else if (buf[i] >= 0x20 && buf[i] < 0x7f && len < sizeof line - 1) {
            line[len++] = (char)buf[i];
        } else if (buf[i] < 0x20 || buf[i] >= 0x7f) {
            ++s_rx_junk;
        }
    }
}

static void relay_task(void *arg)
{
    uint8_t buf[128];
    for (;;) {
        size_t n = uart_read_some(C5_UART, buf, sizeof buf, 20);
        if (n == 0) continue;
        if (bridge_active()) {
            bridge_c5_bytes(buf, n);
            continue;
        }
        if (s_log) host_write(buf, n);
        split_lines(buf, n);
    }
}

void c5_init(void)
{
    open_drain_released(PIN_C5_BOOT);
    open_drain_released(PIN_C5_EN);
    c5_tx_detach();

    /* RX only until a bridge session: the C5's GPIO 12 may be an output under unknown firmware. */
    const uart_config_t cfg = {
        .baud_rate = C5_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(C5_UART, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(C5_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(C5_UART, UART_PIN_NO_CHANGE, PIN_C5_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* As on the C5: if lane noise keeps the line from going idle, the idle timeout never fires and a short reply
     * waits in the FIFO for 120 bytes. */
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(C5_UART, 1));
    /* Keeps the line idle rather than floating while the C5 is in reset. */
    gpio_pullup_en(PIN_C5_RX);
    s_ready = xSemaphoreCreateBinary();
    s_reply = xSemaphoreCreateBinary();
    s_request = xSemaphoreCreateMutex();
    xTaskCreate(relay_task, "c5_relay", 3072, NULL, 5, NULL);
}

void c5_tx_attach(void)
{
    /* Push-pull: only called once c5rx or the ROM loader owns the C5's GPIO 12 as its UART RX. An open-drain
     * line on the internal pull-up lost characters to the 40 MHz lanes beside it. */
    ESP_ERROR_CHECK(uart_set_pin(C5_UART, PIN_C5_TX, PIN_C5_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(PIN_C5_RX);
}

void c5_tx_detach(void)
{
    s_c5rx = false;
    gpio_reset_pin(PIN_C5_TX);
    gpio_set_direction(PIN_C5_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_C5_TX, GPIO_FLOATING);
}

bool c5_run_c5rx(uint32_t timeout_ms)
{
    xSemaphoreTake(s_ready, 0);
    c5_run();
    return xSemaphoreTake(s_ready, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool c5_request(const char *cmd, char *reply, size_t reply_len, uint32_t timeout_ms)
{
    if (!s_c5rx) {
        strlcpy(reply, "err c5rx not running", reply_len);
        return false;
    }
    xSemaphoreTake(s_request, portMAX_DELAY);
    xSemaphoreTake(s_reply, 0);
    /* With the UART run beside the bus lanes, it lost characters both ways; `c5 pause on` turns the bus off for
     * each exchange, a few milliseconds. */
    bool bus = s_pause_bus && bus_enabled();
    if (bus) bus_enable(false);
    /* A newline first ends any noise the C5 has picked up since the last request, and a check byte lets the C5
     * refuse a command that noise changed into another one. */
    uint8_t check = 0;
    for (const char *p = cmd; *p; ++p) check ^= (uint8_t)*p;
    char tail[8];
    snprintf(tail, sizeof tail, " #%02x\n", check);
    uart_write_bytes(C5_UART, "\n", 1);
    uart_write_bytes(C5_UART, cmd, strlen(cmd));
    uart_write_bytes(C5_UART, tail, strlen(tail));
    /* A reply that came after an earlier request gave up names that request's command; skip it. */
    const char *name = strncmp(cmd, "ping", 4) == 0 ? "c5rx" : cmd;
    size_t word = strcspn(name, " ");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    bool got = false;
    while (!got && xSemaphoreTake(s_reply, deadline - xTaskGetTickCount()) == pdTRUE) {
        got = strncmp(s_reply_line, "err", 3) == 0 ||
              (strncmp(s_reply_line + 3, name, word) == 0 && (s_reply_line[3 + word] == ' ' || !s_reply_line[3 + word]));
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) break;
    }
    if (bus) bus_enable(true);
    strlcpy(reply, got ? s_reply_line : "err no reply", reply_len);
    xSemaphoreGive(s_request);
    return got && strncmp(reply, "ok", 2) == 0;
}

void c5_hold(void)
{
    c5_tx_detach();
    gpio_set_level(PIN_C5_EN, 0);
}

void c5_run(void)
{
    c5_tx_detach();
    gpio_set_level(PIN_C5_BOOT, 1);
    gpio_set_level(PIN_C5_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(EN_LOW_MS));
    gpio_set_level(PIN_C5_EN, 1);
}

void c5_download(void)
{
    c5_tx_detach();
    gpio_set_level(PIN_C5_BOOT, 0);
    gpio_set_level(PIN_C5_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(EN_LOW_MS));
    gpio_set_level(PIN_C5_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(BOOT_AFTER_EN_MS));
    gpio_set_level(PIN_C5_BOOT, 1);
}

/* With a pull-down, a pin reads 1 only when a powered C5 pulls or drives it high. */
static void c5_pins(void)
{
    static const struct { const char *name; int pin; } pins[] = {
        {"EN", PIN_C5_EN}, {"BOOT", PIN_C5_BOOT}, {"RX", PIN_C5_RX}, {"TX", PIN_C5_TX}};
    for (size_t i = 0; i < sizeof pins / sizeof pins[0]; ++i) {
        gpio_pulldown_en(pins[i].pin);
        gpio_pullup_dis(pins[i].pin);
        vTaskDelay(pdMS_TO_TICKS(5));
        say("%s (P4 %d) %d\n", pins[i].name, pins[i].pin, gpio_get_level(pins[i].pin));
        gpio_pulldown_dis(pins[i].pin);
    }
    gpio_pullup_en(PIN_C5_RX);
    int last = gpio_get_level(PIN_C5_RX), edges = 0;
    int64_t end = esp_timer_get_time() + 20000;
    while (esp_timer_get_time() < end) {
        int v = gpio_get_level(PIN_C5_RX);
        edges += v != last;
        last = v;
    }
    say("RX edges in 20 ms: %d\n", edges);
}

static uint32_t s_send_ms = 500;

void c5_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "uart") == 0) {
        say("from the C5: %lu lines, %lu not replies, %lu bytes not printable\n", (unsigned long)s_rx_lines,
            (unsigned long)s_rx_other, (unsigned long)s_rx_junk);
        return;
    } else if (strcmp(sub, "hold") == 0) {
        c5_hold();
    } else if (strcmp(sub, "reset") == 0) {
        c5_run();
    } else if (strcmp(sub, "start") == 0) {
        say(c5_run_c5rx(5000) ? "ok c5rx ready\n" : "err c5rx did not start\n");
        return;
    } else if (strcmp(sub, "boot") == 0) {
        c5_download();
    } else if (strcmp(sub, "pins") == 0) {
        c5_pins();
        return;
    } else if (strcmp(sub, "log") == 0 && argc > 2) {
        s_log = strcmp(argv[2], "on") == 0;
    } else if (strcmp(sub, "pause") == 0 && argc > 2) {
        s_pause_bus = strcmp(argv[2], "off") != 0;
    } else if (strcmp(sub, "send") == 0 && argc > 2) {
        char cmd[96] = "";
        for (int i = 2; i < argc; ++i) {
            if (i > 2) strlcat(cmd, " ", sizeof cmd);
            strlcat(cmd, argv[i], sizeof cmd);
        }
        char reply[96];
        int64_t t0 = esp_timer_get_time();
        c5_request(cmd, reply, sizeof reply, s_send_ms);
        say("%s (%lld ms)\n", reply, (esp_timer_get_time() - t0) / 1000);
        return;
    } else if (strcmp(sub, "wait") == 0 && argc > 2) {
        /* How long `c5 send` waits for a reply. */
        s_send_ms = atoi(argv[2]);
        say("ok\n");
        return;
    } else {
        say("c5 hold | reset | start | boot | pins | log on|off | send <command>\n");
        return;
    }
    say("ok\n");
}
