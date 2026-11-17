#include "selftest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "c5.h"
#include "console.h"
#include "decode.h"
#include "pins.h"

#define LANES 14
#define ALL_LANES 0x3fff
/* The C5's bus clock in walk mode, as for `iq on 2`: the PARLIO clock at 80 MHz / 2, which the Nano passes on. */
#define PCLK_MIN_HZ 12000000
#define PCLK_MAX_HZ 14700000

typedef enum { CHECK_SKIPPED, CHECK_OK, CHECK_FAILED } state_t;

typedef struct {
    const char *name;
    state_t state;
    char detail[40];
} check_t;

enum { C5_LINK, C5_WIRES, BUS_OFF, PCLK, LANE_WALK, N_CHECKS };

static check_t s_checks[N_CHECKS] = {
    {.name = "c5"}, {.name = "en/boot"}, {.name = "bus enable"}, {.name = "pclk"}, {.name = "lanes"},
};
static bool s_ran;
static int64_t s_took_us, s_at_us;

static bool fail(int k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool fail(int k, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_checks[k].detail, sizeof s_checks[k].detail, fmt, ap);
    va_end(ap);
    s_checks[k].state = CHECK_FAILED;
    return false;
}

static bool pass(int k)
{
    s_checks[k].state = CHECK_OK;
    s_checks[k].detail[0] = '\0';
    return true;
}

static const char *lane_name(int lane)
{
    static const char *const names[LANES] = {"D13", "D12", "D11", "D10", "D9", "D8", "D7",
                                             "D6",  "D5",  "D4",  "D3",  "D2", "D1", "D0"};
    return names[lane];
}

/* The bus pins as inputs with one pull, so an undriven wire reads that pull. */
static void bus_pins_pull(bool up)
{
    for (int i = -1; i < LANES; ++i) {
        int pin = i < 0 ? bus_pclk_gpio() : bus_lane_gpio(i);
        gpio_reset_pin(pin);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin, up ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
    }
    /* A weak pull takes a while to move a ribbon wire across. */
    esp_rom_delay_us(300);
}

/* D13..D0 as a word, bit 13 for D13. */
static uint16_t read_lanes(void)
{
    uint16_t v = 0;
    for (int lane = 0; lane < LANES; ++lane) v |= (uint16_t)gpio_get_level(bus_lane_gpio(lane)) << (13 - lane);
    return v;
}

static bool c5_say(const char *cmd)
{
    char reply[64];
    for (int tries = 0; tries < 3; ++tries)
        if (c5_request(cmd, reply, sizeof reply, 300)) return true;
    return false;
}

static bool walk(uint16_t pattern)
{
    char cmd[16];
    snprintf(cmd, sizeof cmd, "walk %04x", pattern);
    return c5_say(cmd);
}

static bool check_c5(void)
{
    if (!c5_run_c5rx(2000)) return fail(C5_LINK, "c5rx did not start");
    if (!c5_say("ping")) return fail(C5_LINK, "no reply to ping");
    return pass(C5_LINK);
}

/* With the C5 running, its own pull-ups hold EN and BOOT high against the P4's pull-downs. */
static bool check_wires(void)
{
    int en, boot;
    c5_read_wires(&en, &boot);
    if (!en || !boot) return fail(C5_WIRES, "%s%s%s low", en ? "" : "EN", !en && !boot ? " and " : "", boot ? "" : "BOOT");
    return pass(C5_WIRES);
}

/* With the bus off, the Nano leaves every lane undriven, so each follows the P4's pull. PCLK always reads low from
 * the P4 (fpga-toolchain.md), so it's left out. The Nano's enable is clocked by the C5's clock, which has to run. */
static bool check_bus_off(void)
{
    bus_enable(false);
    vTaskDelay(1);
    char stuck[40] = "";
    for (int up = 0; up < 2; ++up) {
        bus_pins_pull(up);
        uint16_t v = read_lanes();
        uint16_t want = up ? ALL_LANES : 0;
        for (int lane = 0; lane < LANES && !stuck[0]; ++lane)
            if ((v ^ want) >> (13 - lane) & 1) snprintf(stuck, sizeof stuck, "%s driven %s", lane_name(lane), up ? "low" : "high");
    }
    if (stuck[0]) return fail(BUS_OFF, "bus off but %s", stuck);
    return pass(BUS_OFF);
}

static int pclk_hz(void)
{
    pcnt_unit_handle_t unit = NULL;
    pcnt_channel_handle_t chan = NULL;
    const pcnt_unit_config_t uc = {.low_limit = -1, .high_limit = 32767};
    const pcnt_chan_config_t cc = {.edge_gpio_num = bus_pclk_gpio(), .level_gpio_num = -1};
    int count = -1;
    if (pcnt_new_unit(&uc, &unit) != ESP_OK) return -1;
    if (pcnt_new_channel(unit, &cc, &chan) == ESP_OK &&
        pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD) == ESP_OK &&
        pcnt_unit_enable(unit) == ESP_OK) {
        pcnt_unit_clear_count(unit);
        int64_t t0 = esp_timer_get_time();
        pcnt_unit_start(unit);
        esp_rom_delay_us(1000);
        pcnt_unit_stop(unit);
        int64_t dt = esp_timer_get_time() - t0;
        pcnt_unit_get_count(unit, &count);
        count = (int)((int64_t)count * 1000000 / dt);
        pcnt_unit_disable(unit);
    }
    if (chan) pcnt_del_channel(chan);
    pcnt_del_unit(unit);
    return count;
}

static bool check_pclk(void)
{
    bus_enable(true);
    vTaskDelay(1);
    int hz = pclk_hz();
    if (hz < PCLK_MIN_HZ || hz > PCLK_MAX_HZ) return fail(PCLK, "PCLK at %d Hz", hz);
    return pass(PCLK);
}

/* Walking ones read with pull-downs and walking zeros with pull-ups, so an open lane reads wrong in both. */
static bool check_lanes(void)
{
    uint16_t got[2][LANES];
    for (int zeros = 0; zeros < 2; ++zeros) {
        bus_pins_pull(zeros);
        for (int lane = 0; lane < LANES; ++lane) {
            uint16_t bit = 1u << (13 - lane), pattern = zeros ? ALL_LANES & ~bit : bit;
            if (!walk(pattern)) return fail(LANE_WALK, "C5 did not take walk %04x", pattern);
            esp_rom_delay_us(50);
            got[zeros][lane] = read_lanes();
        }
    }
    /* For each lane, the patterns where it read wrong. */
    for (int lane = 0; lane < LANES; ++lane) {
        uint16_t bit = 1u << (13 - lane);
        int high = 0, low = 0, wrong = 0;
        for (int zeros = 0; zeros < 2; ++zeros)
            for (int k = 0; k < LANES; ++k) {
                uint16_t pattern = zeros ? ALL_LANES & ~(1u << (13 - k)) : 1u << (13 - k);
                bool v = got[zeros][k] & bit, want = pattern & bit;
                high += v, low += !v, wrong += v != want;
            }
        if (!wrong) continue;
        if (!high) return fail(LANE_WALK, "%s stuck low", lane_name(lane));
        if (!low) return fail(LANE_WALK, "%s stuck high", lane_name(lane));
        /* Reads that follow the pull rather than the pattern: an open wire. */
        bool open = true;
        for (int k = 0; k < LANES; ++k) open &= !(got[0][k] & bit) && (got[1][k] & bit);
        if (open) return fail(LANE_WALK, "%s open", lane_name(lane));
        /* Reads that follow another lane's bit: crossed or shorted to it. */
        for (int other = 0; other < LANES; ++other) {
            if (other == lane) continue;
            uint16_t ob = 1u << (13 - other);
            bool follows = true;
            for (int zeros = 0; zeros < 2; ++zeros)
                for (int k = 0; k < LANES; ++k) {
                    uint16_t pattern = zeros ? ALL_LANES & ~(1u << (13 - k)) : 1u << (13 - k);
                    follows &= !!(got[zeros][k] & bit) == !!(pattern & ob);
                }
            if (follows) return fail(LANE_WALK, "%s carries %s", lane_name(lane), lane_name(other));
        }
        return fail(LANE_WALK, "%s wrong in %d of 28", lane_name(lane), wrong);
    }
    return pass(LANE_WALK);
}

bool selftest_run(void)
{
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < N_CHECKS; ++k) s_checks[k].state = CHECK_SKIPPED, s_checks[k].detail[0] = '\0';
    bool ok = check_c5() && check_wires();
    if (ok) {
        /* The walk's clock first: the Nano's enable only changes on the C5's clock edges. */
        if (!walk(0)) ok = fail(BUS_OFF, "C5 did not start its clock");
        else ok = check_bus_off() && check_pclk() && check_lanes();
        c5_say("walk off");
    }
    bus_enable(false);
    s_took_us = esp_timer_get_time() - t0;
    s_at_us = t0;
    s_ran = true;
    return ok;
}

bool selftest_failed(void)
{
    for (int k = 0; k < N_CHECKS; ++k)
        if (s_checks[k].state == CHECK_FAILED) return true;
    return false;
}

int selftest_lines(char lines[][32], int max)
{
    int n = 0;
    for (int k = 0; k < N_CHECKS && n < max; ++k)
        if (s_checks[k].state == CHECK_FAILED) snprintf(lines[n++], 32, "%s", s_checks[k].detail);
    return n;
}

void selftest_info(void)
{
    if (!s_ran) {
        say("self-check not run\n");
        return;
    }
    if (!selftest_failed()) {
        say("self-check ok in %lld ms, %lld s ago\n", s_took_us / 1000, (esp_timer_get_time() - s_at_us) / 1000000);
        return;
    }
    say("self-check FAILED %lld s ago:", (esp_timer_get_time() - s_at_us) / 1000000);
    for (int k = 0; k < N_CHECKS; ++k)
        if (s_checks[k].state == CHECK_FAILED) say(" %s: %s;", s_checks[k].name, s_checks[k].detail);
    say("\n");
}

void selftest_command(int argc, char **argv)
{
    bool was = decode_running();
    if (was) decode_stop();
    selftest_run();
    for (int k = 0; k < N_CHECKS; ++k) {
        const check_t *c = &s_checks[k];
        say("%-10s %s%s%s\n", c->name, c->state == CHECK_OK ? "ok" : c->state == CHECK_FAILED ? "FAIL" : "skipped",
            c->detail[0] ? ": " : "", c->detail);
    }
    say("%lld ms\n", s_took_us / 1000);
    if (was && !selftest_failed()) decode_resume();
}
