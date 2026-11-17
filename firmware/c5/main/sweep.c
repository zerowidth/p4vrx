#include "sweep.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radio.h"

/* While the P4's decoding is paused, the C5 steps its LO through the list once a tick and marks each step on the BOOT
 * line, which it drives meanwhile: low for MARK_US + MARK_STEP_US x entry, the hop starting as the line rises. The
 * width names the entry, so a lost or extra mark can't shift the reads after it. */
#define BOOT_GPIO 28
#define MARK_US 20
#define MARK_STEP_US 8

static uint16_t s_list[SWEEP_MAX];
static volatile int s_count;
static uint16_t s_home;
static TaskHandle_t s_task;
static volatile uint32_t s_hops, s_passes;
/* The BOOT wire is also a strapping pin of the P4's: marks during a P4 reset boot it wrong. So the scan stops by
 * itself unless the P4 renews it. */
#define RENEW_US 1500000
static volatile int64_t s_renewed_us;

static void release_line(void)
{
    gpio_set_direction(BOOT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_GPIO, GPIO_PULLUP_ONLY);
}

static void sweep_task(void *arg)
{
    for (;;) {
        if (!s_count) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        for (int entry = 0; entry < s_count; ++entry) {
            gpio_set_level(BOOT_GPIO, 0);
            esp_rom_delay_us(MARK_US + MARK_STEP_US * entry);
            gpio_set_level(BOOT_GPIO, 1);
            radio_lo_hop(s_list[entry]);
            ++s_hops;
            vTaskDelay(1);
            if (!s_count) break;
            if (esp_timer_get_time() - s_renewed_us > RENEW_US) {
                s_count = 0;
                radio_lo_hop(s_home);
                release_line();
                break;
            }
        }
        ++s_passes;
    }
}

bool sweep_renew(void)
{
    s_renewed_us = esp_timer_get_time();
    return s_count;
}

int sweep_set(const char *const *names, int count)
{
    if (!s_task) xTaskCreate(sweep_task, "sweep", 3072, NULL, configMAX_PRIORITIES - 1, &s_task);
    s_renewed_us = esp_timer_get_time();
    bool was = s_count;
    s_count = 0;
    vTaskDelay(3);
    if (was) {
        radio_lo_hop(s_home);
        /* Back to an input held high, as the P4's open-drain BOOT control expects. */
        release_line();
    }
    uint16_t list[SWEEP_MAX];
    int n = 0;
    for (int k = 0; k < count && k < SWEEP_MAX; ++k)
        if (!radio_channel_mhz(names[k], &list[n++])) return -1;
    if (!n) return 0;
    for (int k = 0; k < n; ++k) s_list[k] = list[k];
    s_home = radio_mhz();
    gpio_reset_pin(BOOT_GPIO);
    gpio_set_level(BOOT_GPIO, 1);
    gpio_set_direction(BOOT_GPIO, GPIO_MODE_OUTPUT);
    s_count = n;
    xTaskNotifyGive(s_task);
    return n;
}

void sweep_counts(uint32_t *hops, uint32_t *passes)
{
    *hops = s_hops;
    *passes = s_passes;
}
