// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/c5.c), by Twotoz and the C5VRX contributors.
#include "c5.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "soc/gpio_sig_map.h"
#include "hal/uart_periph.h"
#include "freertos/FreeRTOS.h"
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

/* Level 1 releases the pin (high-Z); the C5's own pull-ups hold EN and BOOT high. */
static void open_drain_released(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}

/* UART1 listens to the C5 only while a bridge session or the log needs it: firmware that drives GPIO 11
 * with something fast floods the UART with interrupts and starves the console. */
static void rx_route(bool on)
{
    if (on) {
        ESP_ERROR_CHECK(uart_set_pin(C5_UART, UART_PIN_NO_CHANGE, PIN_C5_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                       UART_PERIPH_SIGNAL(C5_UART, SOC_UART_PERIPH_SIGNAL_RX), false);
    }
    gpio_pullup_en(PIN_C5_RX);
}

static void relay_task(void *arg)
{
    uint8_t buf[128];
    for (;;) {
        size_t n = uart_read_some(C5_UART, buf, sizeof buf, 20);
        if (n == 0) continue;
        if (bridge_active()) {
            bridge_c5_bytes(buf, n);
        } else if (s_log) {
            host_write(buf, n);
        }
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
    gpio_reset_pin(PIN_C5_RX);
    gpio_set_direction(PIN_C5_RX, GPIO_MODE_INPUT);
    rx_route(false);
    xTaskCreate(relay_task, "c5_relay", 3072, NULL, 5, NULL);
}

void c5_tx_attach(void)
{
    ESP_ERROR_CHECK(uart_set_pin(C5_UART, PIN_C5_TX, PIN_C5_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(PIN_C5_RX);
    /* Open-drain, so a C5 that is already running its app can't fight the P4.
     * gpio_set_direction would unroute the UART signal, so only flip the driver. */
    gpio_od_enable(PIN_C5_TX);
    gpio_set_pull_mode(PIN_C5_TX, GPIO_PULLUP_ONLY);
}

void c5_tx_detach(void)
{
    if (!s_log) rx_route(false);
    gpio_reset_pin(PIN_C5_TX);
    gpio_set_direction(PIN_C5_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_C5_TX, GPIO_FLOATING);
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

void c5_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "hold") == 0) {
        c5_hold();
    } else if (strcmp(sub, "reset") == 0) {
        c5_run();
    } else if (strcmp(sub, "boot") == 0) {
        c5_download();
    } else if (strcmp(sub, "pins") == 0) {
        c5_pins();
        return;
    } else if (strcmp(sub, "log") == 0 && argc > 2) {
        s_log = strcmp(argv[2], "on") == 0;
        rx_route(s_log || bridge_active());
    } else {
        say("c5 hold | reset | boot | pins | log on|off\n");
        return;
    }
    say("ok\n");
}
