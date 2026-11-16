#include "pins.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"

typedef struct {
    const char *name;
    int pin;
    bool i2c; /* pulled up on the board and shared with the touch controller: only ever driven low */
} nano_pin_t;

/* The P4 header pins wired to the Nano, per docs/wiring.md. */
static const nano_pin_t s_pins[] = {
    {"PCLK", 4, false},  {"D13", 2, false},  {"D12", 3, false},  {"D11", 21, false}, {"D10", 22, false}, {"D9", 5, false},
    {"D8", 49, false},   {"D7", 28, false},  {"D6", 29, false},  {"D5", 30, false},  {"D4", 31, false},  {"D3", 32, false},
    {"D2", 46, false},   {"D1", 47, false},  {"D0", 48, false},  {"IRQ", 52, false}, {"SCL", 8, true}, {"SDA", 7, true},
};
#define N_PINS (sizeof s_pins / sizeof s_pins[0])

/* Lanes are inputs with pull-downs, so an undriven wire reads 0; I2C keeps the board's pull-ups. */
static void release(const nano_pin_t *p)
{
    gpio_reset_pin(p->pin);
    gpio_set_direction(p->pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(p->pin, p->i2c ? GPIO_FLOATING : GPIO_PULLDOWN_ONLY);
}

static const nano_pin_t *find(const char *name)
{
    for (size_t i = 0; i < N_PINS; ++i)
        if (strcasecmp(name, s_pins[i].name) == 0) return &s_pins[i];
    return NULL;
}

/* The bus as the pass-through bitstream drives it: lane 0 is D13 (Q7) .. lane 13 is D0 (I1). */
int bus_pclk_gpio(void)
{
    return s_pins[0].pin;
}

int bus_lane_gpio(int lane)
{
    return s_pins[1 + lane].pin;
}

/* The Nano drives the bus only while the P4 holds the IRQ wire high. */
static bool s_bus;

bool bus_enabled(void)
{
    return s_bus;
}

void bus_enable(bool on)
{
    s_bus = on;
    const nano_pin_t *irq = find("IRQ");
    gpio_set_level(irq->pin, on);
    gpio_set_direction(irq->pin, on ? GPIO_MODE_INPUT_OUTPUT : GPIO_MODE_INPUT);
}

void pins_init(void)
{
    /* GPIO 39-48 run from LDO channel 4, the board's SD card IO supply; without it their high is too low
     * for the Nano. */
    static esp_ldo_channel_handle_t ldo;
    if (!ldo) {
        const esp_ldo_channel_config_t cfg = {.chan_id = 4, .voltage_mv = 3300};
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&cfg, &ldo));
    }
    for (size_t i = 0; i < N_PINS; ++i) release(&s_pins[i]);
}

void pins_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "read";
    if (strcmp(sub, "read") == 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
        say("pins");
        for (size_t i = 0; i < N_PINS; ++i) say(" %s=%d", s_pins[i].name, gpio_get_level(s_pins[i].pin));
        say("\n");
    } else if (strcmp(sub, "release") == 0) {
        pins_init();
        say("ok\n");
    } else if (strcmp(sub, "bus") == 0 && argc > 2) {
        bus_enable(strcmp(argv[2], "on") == 0);
        say("ok\n");
    } else if (strcmp(sub, "drive") == 0 && argc > 3) {
        const nano_pin_t *p = find(argv[2]);
        int level = atoi(argv[3]);
        if (!p || (p->i2c && level)) {
            say("err %s\n", argv[2]);
            return;
        }
        gpio_set_level(p->pin, level);
        gpio_set_direction(p->pin, p->i2c ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT);
        say("ok\n");
    } else {
        say("pins read | release | drive <name> <0|1> | bus on|off\n");
    }
}
