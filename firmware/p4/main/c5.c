// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/c5.c), by Twotoz and the C5VRX contributors.
#include "c5.h"

#include <string.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge.h"
#include "console.h"
#include "pins.h"

#define C5_BAUD 115200
/* The C5's EN has 10k and 1 uF, so it takes about 10 ms to rise after release. */
#define EN_LOW_MS 50
#define BOOT_AFTER_EN_MS 100

/* Level 1 releases the pin (high-Z); the C5's own pull-ups hold EN and BOOT high. */
static void open_drain_released(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    gpio_set_level(pin, 1);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}

static void relay_task(void *arg)
{
    uint8_t buf[128];
    for (;;) {
        size_t n = uart_read_some(C5_UART, buf, sizeof buf, 20);
        if (n && bridge_active()) bridge_c5_bytes(buf, n);
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
    /* Keeps the line idle rather than floating while the C5 is in reset. */
    gpio_pullup_en(PIN_C5_RX);
    xTaskCreate(relay_task, "c5_relay", 3072, NULL, 5, NULL);
}

void c5_tx_attach(void)
{
    ESP_ERROR_CHECK(uart_set_pin(C5_UART, PIN_C5_TX, PIN_C5_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Open-drain, so a C5 that is already running its app can't fight the P4.
     * gpio_set_direction would unroute the UART signal, so only flip the driver. */
    gpio_od_enable(PIN_C5_TX);
    gpio_set_pull_mode(PIN_C5_TX, GPIO_PULLUP_ONLY);
}

void c5_tx_detach(void)
{
    gpio_reset_pin(PIN_C5_TX);
    gpio_set_direction(PIN_C5_TX, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_C5_TX, GPIO_FLOATING);
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

void c5_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "reset") == 0) {
        c5_run();
    } else if (strcmp(sub, "boot") == 0) {
        c5_download();
    } else {
        say("c5 reset | boot\n");
        return;
    }
    say("ok\n");
}
