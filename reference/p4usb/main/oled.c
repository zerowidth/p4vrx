#include "oled.h"

#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"

#include "big_font.h"
#include "console.h"
#include "menu_font.h"
#include "pins.h"

#define ADDRESS 0x3c
#define TIMEOUT_MS 50
#define COMMANDS 0x00
#define DATA 0x40
#define PANEL_WIDTH OLED_HEIGHT
#define PANEL_HEIGHT OLED_WIDTH

static i2c_master_dev_handle_t s_dev;
/* The data prefix, then the frame in the panel's order: a byte is eight rows of one column, top row in bit 0. */
static uint8_t s_out[1 + PANEL_WIDTH * PANEL_HEIGHT / 8] = {DATA};
static uint8_t *const s_frame = s_out + 1;

static void commands(const uint8_t *bytes, size_t n)
{
    uint8_t buf[32] = {COMMANDS};
    if (!s_dev || n > sizeof buf - 1) return;
    memcpy(buf + 1, bytes, n);
    i2c_master_transmit(s_dev, buf, n + 1, TIMEOUT_MS);
}

bool oled_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) return false;
    i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDRESS, .scl_speed_hz = 400000};
    if (i2c_master_probe(bus, ADDRESS, TIMEOUT_MS) != ESP_OK || i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        i2c_del_master_bus(bus);
        return false;
    }
    static const uint8_t init[] = {
        0xae,       /* off */
        0xd5, 0x80, /* clock */
        0xa8, PANEL_HEIGHT - 1,
        0xd3, 0x00, /* no vertical offset */
        0x40,       /* start at row 0 */
        0x8d, 0x14, /* charge pump on */
        0x20, 0x00, /* a write runs along a page, then to the next */
        0xda, 0x02, /* the 32-row panel's COM wiring */
        0x81, 0x8f, /* contrast */
        0xd9, 0xf1, /* precharge */
        0xdb, 0x40, /* VCOMH */
        0xa4, 0xa6, /* show the frame, not inverted */
        0xaf,       /* on */
    };
    commands(init, sizeof init);
    oled_flip(false);
    return true;
}

void oled_flip(bool flipped)
{
    const uint8_t cmd[] = {flipped ? 0xa1 : 0xa0, flipped ? 0xc8 : 0xc0};
    commands(cmd, sizeof cmd);
}

void oled_clear(void)
{
    memset(s_frame, 0, PANEL_WIDTH * PANEL_HEIGHT / 8);
}

static bool lit(int x, int y)
{
    return s_frame[(PANEL_HEIGHT - 1 - x) / 8 * PANEL_WIDTH + y] >> ((PANEL_HEIGHT - 1 - x) & 7) & 1;
}

/* Portrait x runs up the panel's rows and y along its columns. */
static void set(int x, int y, int how)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t *byte = s_frame + (PANEL_HEIGHT - 1 - x) / 8 * PANEL_WIDTH + y, bit = 1 << ((PANEL_HEIGHT - 1 - x) & 7);
    if (how < 0) *byte ^= bit;
    else if (how) *byte |= bit;
    else *byte &= ~bit;
}

void oled_box(int x, int y, int w, int h, bool on)
{
    for (int r = y; r < y + h; ++r) {
        for (int c = x; c < x + w; ++c) set(c, r, on);
    }
}

void oled_invert(int x, int y, int w, int h)
{
    for (int r = y; r < y + h; ++r) {
        for (int c = x; c < x + w; ++c) set(c, r, -1);
    }
}

void oled_text(int x, int y, int scale, const char *s)
{
    for (; *s; ++s, x += 8 * scale) {
        const uint8_t *rows = s_font8x8[*s < 32 || *s > 126 ? '?' - 32 : *s - 32];
        for (int r = 0; r < 8; ++r) {
            for (int b = 0; b < 8; ++b) {
                if (rows[r] & (0x80 >> b)) oled_box(x + b * scale, y + r * scale, scale, scale, true);
            }
        }
    }
}

void oled_big(int x, int y, int size, char c)
{
    const char *at = strchr(BIG_FONT_CHARS, c);
    if (!c || !at) return;
    for (int r = 0; r < size; ++r) {
        uint32_t row = size == 32 ? s_big32[at - BIG_FONT_CHARS][r] : s_big16[at - BIG_FONT_CHARS][r];
        for (int b = 0; b < size; ++b) {
            if (row >> (size - 1 - b) & 1) set(x + b, y + r, 1);
        }
    }
}

void oled_show(void)
{
    if (!s_dev) return;
    static const uint8_t whole[] = {0x21, 0, PANEL_WIDTH - 1, 0x22, 0, PANEL_HEIGHT / 8 - 1};
    commands(whole, sizeof whole);
    i2c_master_transmit(s_dev, s_out, sizeof s_out, TIMEOUT_MS);
}

void oled_dump(void)
{
    static const char *const cells[] = {" ", "▀", "▄", "█"};
    for (int y = 0; y < OLED_HEIGHT; y += 2) {
        char line[OLED_WIDTH * 3 + 2];
        size_t len = 0;
        for (int x = 0; x < OLED_WIDTH; ++x) {
            len += strlcpy(line + len, cells[lit(x, y) | lit(x, y + 1) << 1], 4);
        }
        line[len++] = '\n';
        host_write(line, len);
    }
}
