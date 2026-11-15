// Pin mapping test: drives each of the C5's 15 Nano-facing pins high, then low, for STEP_MS each, with the
// rest pulled down, in Nano bit order (clock, Q7..Q1, I7..I1), forever. Comparing the two phases works on
// lines with pull-ups too. It prints each phase, and any other pin of its own that follows the driven one.
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"

#define MARK_MS 600
#define STEP_MS 250

static const int PINS[] = {0, 1, 2, 3, 4, 23, 24, 25, 6, 7, 8, 9, 10, 13, 14};
static const char *const NAMES[] = {"CLK", "Q7", "Q6", "Q5", "Q4", "Q3", "Q2", "Q1",
                                    "I7",  "I6", "I5", "I4", "I3", "I2", "I1"};
#define N_PINS (sizeof PINS / sizeof PINS[0])

static void all_inputs(void)
{
    for (size_t i = 0; i < N_PINS; ++i) {
        gpio_set_direction(PINS[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(PINS[i], GPIO_PULLDOWN_ONLY);
    }
}

void app_main(void)
{
    /* GPIO 13/14 are the USB pair; the pads must leave the USB PHY to work as GPIO. */
    usb_serial_jtag_ll_enable_bus_clock(true);
    usb_serial_jtag_ll_phy_enable_pad(false);
    for (size_t i = 0; i < N_PINS; ++i) gpio_reset_pin(PINS[i]);

    for (;;) {
        printf("pinwalk mark\n");
        for (size_t i = 0; i < N_PINS; ++i) {
            gpio_set_level(PINS[i], 1);
            gpio_set_direction(PINS[i], GPIO_MODE_OUTPUT);
        }
        vTaskDelay(pdMS_TO_TICKS(MARK_MS));
        for (size_t k = 0; k < N_PINS; ++k) {
            all_inputs();
            int high[N_PINS];
            for (int level = 1; level >= 0; --level) {
                gpio_set_level(PINS[k], level);
                gpio_set_direction(PINS[k], GPIO_MODE_INPUT_OUTPUT);
                vTaskDelay(pdMS_TO_TICKS(5));
                printf("pinwalk %u %s %s", (unsigned)k, NAMES[k], level ? "high" : "low");
                for (size_t j = 0; j < N_PINS; ++j) {
                    int v = gpio_get_level(PINS[j]);
                    if (level) high[j] = v;
                    else if (j != k && v != high[j]) printf(" follower=%s", NAMES[j]);
                }
                printf("\n");
                vTaskDelay(pdMS_TO_TICKS(STEP_MS - 5));
            }
        }
        all_inputs();
    }
}
