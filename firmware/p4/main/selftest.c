#include "selftest.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "touch.h"

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

enum { C5_LINK, C5_WIRES, NANO, BUS_OFF, PCLK, LANE_WALK, NANO_WALK, N_CHECKS };

static check_t s_checks[N_CHECKS] = {
    {.name = "c5"},   {.name = "en/boot"}, {.name = "nano"},
    {.name = "bus enable"}, {.name = "pclk"}, {.name = "lanes"}, {.name = "nano lanes"},
};
static bool s_ran;
/* Bus-off checks that failed once and passed on a second look, since power-up. */
static int s_retries;
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

#define NANO_ADDR 0x42
#define NANO_BUILD 0x01
#define REG_ID 0x00
#define REG_TAKEN 0x04
#define REG_CONTROL 0x08
#define CONTROL_PATTERN 1
#define CONTROL_OWN_CLOCK 2

static i2c_master_dev_handle_t s_nano;

static bool nano_read(uint8_t reg, uint8_t *data, size_t n)
{
    return s_nano && i2c_master_transmit_receive(s_nano, &reg, 1, data, n, 20) == ESP_OK;
}

static bool nano_write(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[4] = {reg};
    memcpy(buf + 1, data, n);
    return s_nano && i2c_master_transmit(s_nano, buf, n + 1, 20) == ESP_OK;
}

/* C5 clock edges per ms the Nano took and saw, or false without the register file. */
static bool nano_clock(int *taken, int *seen)
{
    uint8_t c[4];
    if (!nano_read(REG_TAKEN, c, 4)) return false;
    *taken = c[0] | c[1] << 8, *seen = c[2] | c[3] << 8;
    return true;
}

static bool check_nano(void)
{
    i2c_master_bus_handle_t bus = touch_i2c_bus();
    if (!bus) return fail(NANO, "no I2C bus");
    if (!s_nano) {
        const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = NANO_ADDR, .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(bus, &dev, &s_nano) != ESP_OK) return fail(NANO, "can't add 0x42");
    }
    uint8_t id[4];
    if (!nano_read(REG_ID, id, 4)) return fail(NANO, "no answer at 0x42 (old bitstream?)");
    if (id[0] != 'P' || id[1] != 'V') return fail(NANO, "0x42 is not the Nano: %02x %02x", id[0], id[1]);
    if (id[2] < NANO_BUILD) return fail(NANO, "Nano build %u, want %u or later", id[2], NANO_BUILD);
    if (!(id[3] & 1)) return fail(NANO, "Nano PLL not locked");
    return pass(NANO);
}

static bool nano_pattern(uint16_t pattern)
{
    const uint8_t p[2] = {pattern & 0xff, pattern >> 8};
    return nano_write(0x09, p, 2);
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
    if (stuck[0]) {
        uint8_t st = 0xff;
        int taken = -1, seen = -1;
        nano_read(0x03, &st, 1);
        nano_clock(&taken, &seen);
        say("selftest: bus off but %s; Nano status %02x, clock %d/%d per ms\n", stuck, st, taken, seen);
        return fail(BUS_OFF, "%s; Nano %02x %d/%d", stuck, st, taken, seen);
    }
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
    if (hz < PCLK_MIN_HZ || hz > PCLK_MAX_HZ) {
        int taken, seen;
        if (nano_clock(&taken, &seen)) return fail(PCLK, "PCLK %d Hz, Nano took %d/ms of %d", hz, taken, seen);
        return fail(PCLK, "PCLK at %d Hz", hz);
    }
    return pass(PCLK);
}

/* Walking ones read with pull-downs and walking zeros with pull-ups, so an open lane reads wrong in both. */
static bool check_lanes(int k_check, bool (*drive)(uint16_t))
{
    uint16_t got[2][LANES];
    for (int zeros = 0; zeros < 2; ++zeros) {
        bus_pins_pull(zeros);
        for (int lane = 0; lane < LANES; ++lane) {
            uint16_t bit = 1u << (13 - lane), pattern = zeros ? ALL_LANES & ~bit : bit;
            if (!drive(pattern)) return fail(k_check, "pattern %04x not taken", pattern);
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
        if (!high) return fail(k_check, "%s stuck low", lane_name(lane));
        if (!low) return fail(k_check, "%s stuck high", lane_name(lane));
        /* Reads that follow the pull rather than the pattern: an open wire. */
        bool open = true;
        for (int k = 0; k < LANES; ++k) open &= !(got[0][k] & bit) && (got[1][k] & bit);
        if (open) return fail(k_check, "%s open", lane_name(lane));
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
            if (follows) return fail(k_check, "%s carries %s", lane_name(lane), lane_name(other));
        }
        return fail(k_check, "%s wrong in %d of 28", lane_name(lane), wrong);
    }
    return pass(k_check);
}

bool selftest_run(void)
{
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < N_CHECKS; ++k) s_checks[k].state = CHECK_SKIPPED, s_checks[k].detail[0] = '\0';
    bool c5 = check_c5() && check_wires();
    bool ok = check_nano() && c5;
    /* The Nano keeps its mode across a P4 reset; the walks need the lanes passed through. */
    if (s_checks[NANO].state == CHECK_OK) {
        const uint8_t raw = 0;
        nano_write(REG_CONTROL, &raw, 1);
    }
    if (c5) {
        /* The walk's clock first: the Nano's enable only changes on the C5's clock edges. */
        if (!walk(0)) ok = fail(BUS_OFF, "C5 did not start its clock");
        else {
            /* Twice the first run after a flash saw D10 driven with the bus off, and later runs passed; a
             * second look 20 ms on rides over that, and the first's detail goes to the console. */
            bool off = check_bus_off();
            if (!off) {
                vTaskDelay(pdMS_TO_TICKS(20));
                off = check_bus_off();
                s_retries += off;
            }
            ok = off && check_pclk() && check_lanes(LANE_WALK, walk);
        }
        c5_say("walk off");
    }
    /* The Nano drives the same patterns itself, on its own clock: with the C5's walk, it places a fault on a hop. */
    if (s_checks[NANO].state == CHECK_OK) {
        const uint8_t on = CONTROL_PATTERN | CONTROL_OWN_CLOCK, off = 0;
        bus_enable(true);
        if (!nano_write(REG_CONTROL, &on, 1)) ok = fail(NANO_WALK, "Nano refused pattern mode");
        else ok = check_lanes(NANO_WALK, nano_pattern) && ok;
        nano_write(REG_CONTROL, &off, 1);
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
        say("self-check ok in %lld ms, %lld s ago%s\n", s_took_us / 1000, (esp_timer_get_time() - s_at_us) / 1000000,
            s_retries ? ", after a second look at the bus enable" : "");
        return;
    }
    say("self-check FAILED %lld s ago:", (esp_timer_get_time() - s_at_us) / 1000000);
    for (int k = 0; k < N_CHECKS; ++k)
        if (s_checks[k].state == CHECK_FAILED) say(" %s: %s;", s_checks[k].name, s_checks[k].detail);
    say("\n");
}

bool nano_pll_phase(int phase)
{
    const uint8_t p = (uint8_t)(phase & 15);
    return nano_write(0x0b, &p, 1);
}

bool nano_hdz(uint32_t *peak, uint32_t *total, uint16_t *bin, uint16_t *decisions)
{
    uint8_t r[12], id[3];
    if (!s_nano) check_nano();
    if (!nano_read(REG_ID, id, 3) || id[0] != 'P' || id[1] != 'V' || id[2] < 3 || !nano_read(0x14, r, 12)) return false;
    *peak = r[0] | r[1] << 8 | r[2] << 16 | (uint32_t)r[3] << 24;
    *total = r[4] | r[5] << 8 | r[6] << 16 | (uint32_t)r[7] << 24;
    *bin = r[8] | r[9] << 8;
    *decisions = r[10] | r[11] << 8;
    return true;
}

int nano_build(void)
{
    uint8_t id[3];
    if (!s_nano) check_nano();
    return nano_read(REG_ID, id, 3) && id[0] == 'P' && id[1] == 'V' ? id[2] : -1;
}

bool nano_field_vsync(bool *vsync)
{
    uint8_t st;
    if (!nano_read(0x21, &st, 1)) return false;
    *vsync = st & 1;
    return true;
}

bool nano_has_fm(void)
{
    uint8_t id[3];
    if (!s_nano) check_nano();
    return nano_read(REG_ID, id, 3) && id[0] == 'P' && id[1] == 'V' && id[2] >= 2;
}

bool nano_magnitude(int *magnitude)
{
    uint8_t m[2];
    if (!nano_read(0x12, m, 2)) return false;
    *magnitude = m[0] | m[1] << 8;
    return true;
}

bool nano_control(uint8_t control)
{
    if (!s_nano) check_nano();
    return nano_write(REG_CONTROL, &control, 1);
}

/* nano read <reg> [n] | nano write <reg> <byte>...: the Nano's registers, in hex. */
void nano_command(int argc, char **argv)
{
    if (!s_nano) check_nano();
    if (!s_nano) {
        say("err no Nano at 0x42\n");
        return;
    }
    if (argc > 2 && strcmp(argv[1], "read") == 0) {
        uint8_t reg = (uint8_t)strtoul(argv[2], NULL, 16), data[32];
        int n = argc > 3 ? atoi(argv[3]) : 1;
        n = n < 1 ? 1 : n > 32 ? 32 : n;
        if (!nano_read(reg, data, n)) {
            say("err read\n");
            return;
        }
        for (int k = 0; k < n; ++k) say("%02x%s", data[k], k + 1 < n ? " " : "\n");
    } else if (argc > 3 && strcmp(argv[1], "write") == 0) {
        uint8_t reg = (uint8_t)strtoul(argv[2], NULL, 16), data[3];
        int n = argc - 3 > 3 ? 3 : argc - 3;
        for (int k = 0; k < n; ++k) data[k] = (uint8_t)strtoul(argv[3 + k], NULL, 16);
        say(nano_write(reg, data, n) ? "ok\n" : "err write\n");
    } else {
        say("nano read <reg> [n] | nano write <reg> <byte>...\n");
    }
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
