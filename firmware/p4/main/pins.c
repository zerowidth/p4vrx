#include "pins.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gpio_filter.h"
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

static gpio_glitch_filter_handle_t s_pclk_filter;

/* Ignores pulses on PCLK shorter than thres_ns within a window_ns sample window; 0 turns it off. */
static esp_err_t pclk_filter(uint32_t window_ns, uint32_t thres_ns)
{
    if (s_pclk_filter) {
        gpio_glitch_filter_disable(s_pclk_filter);
        gpio_del_glitch_filter(s_pclk_filter);
        s_pclk_filter = NULL;
    }
    if (!window_ns) return ESP_OK;
    const gpio_flex_glitch_filter_config_t cfg = {
        .clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT,
        .gpio_num = s_pins[0].pin,
        .window_width_ns = window_ns,
        .window_thres_ns = thres_ns,
    };
    esp_err_t err = gpio_new_flex_glitch_filter(&cfg, &s_pclk_filter);
    return err == ESP_OK ? gpio_glitch_filter_enable(s_pclk_filter) : err;
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
    } else if (strcmp(sub, "float") == 0) {
        /* A pin that follows both pulls is undriven; PCLK and the 14 lanes, the ones the bitstream drives. */
        int driven = 0;
        say("bus pins following the pulls (undriven):");
        for (int i = 0; i < 15; ++i) {
            const nano_pin_t *p = &s_pins[i];
            gpio_set_pull_mode(p->pin, GPIO_PULLUP_ONLY);
            vTaskDelay(pdMS_TO_TICKS(2));
            int up = gpio_get_level(p->pin);
            gpio_set_pull_mode(p->pin, GPIO_PULLDOWN_ONLY);
            vTaskDelay(pdMS_TO_TICKS(2));
            int down = gpio_get_level(p->pin);
            if (up == 1 && down == 0) say(" %s", p->name);
            else ++driven, say(" [%s driven %d]", p->name, down);
        }
        say("\n%d of 15 driven\n", driven);
    } else if (strcmp(sub, "release") == 0) {
        pins_init();
        say("ok\n");
    } else if (strcmp(sub, "pclkfilter") == 0 && argc > 2) {
        esp_err_t err = pclk_filter(strtoul(argv[2], NULL, 0), argc > 3 ? strtoul(argv[3], NULL, 0) : strtoul(argv[2], NULL, 0));
        say(err == ESP_OK ? "ok\n" : "err %s\n", esp_err_to_name(err));
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
