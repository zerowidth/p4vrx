// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/console.c), by Twotoz and the C5VRX contributors.
#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/lp_system_reg.h"
#include "soc/uart_pins.h"

#include "bridge.h"
#include "c5.h"
#include "cpu.h"
#include "decode.h"
#include "lcd.h"
#include "screen.h"
#include "selftest.h"
#include "hdz.h"
#include "iq.h"
#include "still.h"
#include "pins.h"
#include "usb.h"
#include "video.h"

#define MAX_ARGS 8

typedef struct {
    const char *name;
    void (*run)(int argc, char **argv);
    const char *help;
} command_t;

static void cmd_help(int argc, char **argv);

static void cmd_info(int argc, char **argv)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    static const char *const reasons[] = {
        [ESP_RST_POWERON] = "power-on", [ESP_RST_SW] = "restart",      [ESP_RST_PANIC] = "panic",
        [ESP_RST_INT_WDT] = "interrupt watchdog", [ESP_RST_TASK_WDT] = "task watchdog",
        [ESP_RST_WDT] = "watchdog", [ESP_RST_BROWNOUT] = "brownout",
    };
    esp_reset_reason_t r = esp_reset_reason();
    const char *reason = r < sizeof reasons / sizeof reasons[0] && reasons[r] ? reasons[r] : "other";
    say("p4vrx %s, chip rev v%d.%d, uptime %lld ms, last reset: %s (%d)\n", esp_app_get_description()->version,
        chip.revision / 100, chip.revision % 100, esp_timer_get_time() / 1000, reason, (int)r);
    selftest_info();
    hdz_info();
    bridge_info();
    usb_console_info();
}

void console_reboot(bool download)
{
    /* The ROM loader then listens on UART0 and the high-speed USB port. */
    if (download) REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

static void cmd_reboot(int argc, char **argv)
{
    say("rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    console_reboot(argc > 1 && strcmp(argv[1], "download") == 0);
}

static const command_t s_commands[] = {
    {"help", cmd_help, "list commands"},
    {"info", cmd_info, "firmware version, chip revision, uptime and bridge counters"},
    {"reboot", cmd_reboot, "restart the P4; reboot download enters its ROM loader"},
    {"iq", iq_command, "iq [samples] captures the bus and prints levels; grab [samples] takes raw 40 MS/s I/Q from the Nano while decoding carries on; start [channel [every]] sets the C5 up; edge judges the sampling; dump sends the capture"},
    {"decode", decode_command, "decode on [channel] tunes the C5 and decodes NTSC continuously; off stops; alone shows lock and load"},
    {"video", video_command, "frame and H.264 stats; grab sends the newest access unit, raw the newest decoded 720x480 frame; bitrate N; encode on|off|auto (auto, the default, only while a UVC host streams); stagger 0|1|2|auto orders the LCD and encoder; lcd N|auto gives the LCD every Nth field (auto: every other one while UVC streams)"},
    {"lcd", lcd_command, "the panel's frame rate and timings; rotate turns the picture half a turn"},
    {"ui", screen_command, "ui touch shows the touch controller and the last touch; ui open 0|1|2 opens the main strip, channel picker or band grid; ui pick <channel> tunes as a tap would"},
    {"still", still_command, "decodes one field of the last capture (iq 1400000) to 320x240 grey and sends it; inv flips sync polarity"},
    {"usb", usb_command, "the console's USB state; usb suspend reports a suspend the host never sent, to test the recovery"},
    {"pins", pins_command, "pins read lists the Nano-facing pins (lanes pulled down); drive <name> <0|1>, release, or bus on|off"},
    {"selftest", selftest_command, "stops decoding, checks the C5, its EN and BOOT wires, the bus enable, PCLK and the 14 lanes, and resumes"},
    {"hdz", hdz_command, "the HDZero detector, one key=value line: hdzero, score, peak_bin, decisions, ch; changes also print `event hdzero=...`; hdz fake 1|0|off holds the state for testing"},
    {"nano", nano_command, "nano read <reg> [n] | nano write <reg> <byte>...: the Nano's I2C registers, in hex"},
    {"cpu", cpu_command, "cpu [ms] shows each task's share of its core over a window, and each core's idle time"},
    {"c5", c5_command, "c5 hold keeps it in reset; c5 reset runs its app; c5 start also waits for c5rx; c5 boot resets it into its ROM loader; c5 pins reads EN, BOOT, RX and TX with pull-downs; c5 log on|off relays its UART; c5 send talks to c5rx"},
};

static void cmd_help(int argc, char **argv)
{
    for (size_t i = 0; i < sizeof s_commands / sizeof s_commands[0]; ++i) {
        say("  %-8s %s\n", s_commands[i].name, s_commands[i].help);
    }
    say("esptool on this port with --before no-reset reaches the C5 through the bridge\n");
}

typedef enum { FROM_ANY, FROM_UART, FROM_USB } source_t;

/* Replies go where the last input came from; until then, to both. */
static volatile source_t s_source;

void console_init(void)
{
    ESP_ERROR_CHECK(uart_driver_install(HOST_UART, 4096, 4096, 0, NULL, 0));
    /* With the USB-C unplugged the RX line floats, and its noise would steal replies from USB. */
    gpio_set_pull_mode(U0RXD_GPIO_NUM, GPIO_PULLUP_ONLY);
}

bool host_is_uart(void)
{
    return s_source == FROM_UART;
}

void host_write(const void *data, size_t len)
{
    if (s_source != FROM_UART) usb_console_write(data, len);
    if (s_source != FROM_USB) uart_write_bytes(HOST_UART, data, len);
}

void say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (n > 0) host_write(buf, n);
}

size_t uart_read_some(uart_port_t port, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    /* uart_read_bytes waits for all len bytes, so block for one and take what else is buffered. */
    if (len == 0 || uart_read_bytes(port, buf, 1, pdMS_TO_TICKS(timeout_ms)) != 1) return 0;
    size_t more = 0;
    uart_get_buffered_data_len(port, &more);
    if (more > len - 1) more = len - 1;
    int n = more ? uart_read_bytes(port, buf + 1, more, 0) : 0;
    return 1 + (n > 0 ? (size_t)n : 0);
}

size_t host_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t n = uart_read_some(HOST_UART, buf, len, 0);
    if (n) {
        s_source = FROM_UART;
        return n;
    }
    n = usb_console_read(buf, len, timeout_ms);
    if (n) s_source = FROM_USB;
    return n;
}

static void run_line(char *line)
{
    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *tok = strtok(line, " \t"); tok && argc < MAX_ARGS; tok = strtok(NULL, " \t")) {
        argv[argc++] = tok;
    }
    if (argc == 0) return;
    for (size_t i = 0; i < sizeof s_commands / sizeof s_commands[0]; ++i) {
        if (strcmp(argv[0], s_commands[i].name) == 0) {
            s_commands[i].run(argc, argv);
            return;
        }
    }
    say("unknown command '%s', try help\n", argv[0]);
}

void console_feed(uint8_t c)
{
    static char line[128];
    static size_t len;

    if (c == '\r' || c == '\n') {
        say("\n");
        line[len] = '\0';
        len = 0;
        run_line(line);
        say("> ");
    } else if (c == 0x08 || c == 0x7f) {
        if (len > 0) {
            --len;
            say("\b \b");
        }
    } else if (c >= 0x20 && c < 0x7f && len < sizeof line - 1) {
        line[len++] = (char)c;
        host_write(&c, 1);
    }
}

void console_base64(const void *data, size_t len)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = data;
    char line[77];
    for (size_t off = 0; off < len; off += 57) {
        size_t chunk = len - off < 57 ? len - off : 57, o = 0;
        for (size_t k = 0; k < chunk; k += 3) {
            uint32_t v = p[off + k] << 16 | (k + 1 < chunk ? p[off + k + 1] << 8 : 0) | (k + 2 < chunk ? p[off + k + 2] : 0);
            line[o++] = b64[v >> 18 & 63];
            line[o++] = b64[v >> 12 & 63];
            line[o++] = k + 1 < chunk ? b64[v >> 6 & 63] : '=';
            line[o++] = k + 2 < chunk ? b64[v & 63] : '=';
        }
        line[o++] = '\n';
        host_write(line, o);
    }
}
