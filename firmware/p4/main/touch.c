#include "touch.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SDA 7
#define PIN_SCL 8
#define PIN_RST 23
/* The GT911 answers at one of two addresses, chosen by its INT pin at reset; this board leaves INT unconnected. */
static const uint8_t ADDRESSES[] = {0x5D, 0x14};
#define REG_PRODUCT 0x8140
#define REG_STATUS 0x814E
#define REG_POINT 0x8150

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static char s_status[40] = "touch: not started";

static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t len)
{
    const uint8_t at[2] = {reg >> 8, reg & 0xff};
    return i2c_master_transmit_receive(s_dev, at, 2, buf, len, 20);
}

static esp_err_t write_reg8(uint16_t reg, uint8_t value)
{
    const uint8_t data[3] = {reg >> 8, reg & 0xff, value};
    return i2c_master_transmit(s_dev, data, 3, 20);
}

bool touch_init(void)
{
    gpio_reset_pin(PIN_RST);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));

    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
    if (i2c_new_master_bus(&bus, &s_bus) != ESP_OK) {
        snprintf(s_status, sizeof s_status, "touch: no I2C bus");
        return false;
    }
    for (size_t k = 0; k < sizeof ADDRESSES; ++k) {
        if (i2c_master_probe(s_bus, ADDRESSES[k], 50) != ESP_OK) continue;
        const i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDRESSES[k], .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) break;
        uint8_t id[4] = {0};
        if (read_reg(REG_PRODUCT, id, 4) != ESP_OK) break;
        snprintf(s_status, sizeof s_status, "touch: GT%.4s at 0x%02X", (const char *)id, ADDRESSES[k]);
        return true;
    }
    snprintf(s_status, sizeof s_status, "touch: no GT911 at 0x5D or 0x14");
    return false;
}

bool touch_read(bool *down, uint16_t *x, uint16_t *y)
{
    if (!s_dev) return false;
    uint8_t status;
    if (read_reg(REG_STATUS, &status, 1) != ESP_OK) return false;
    /* Bit 7 says a new report is ready; until then the last one stands. */
    if (!(status & 0x80)) return false;
    int points = status & 0x0f;
    *down = points > 0;
    if (points) {
        /* The first point's X then Y, each low byte first; its track id is the byte before. */
        uint8_t p[4];
        if (read_reg(REG_POINT, p, sizeof p) != ESP_OK) return false;
        *x = (uint16_t)(p[0] | p[1] << 8);
        *y = (uint16_t)(p[2] | p[3] << 8);
    }
    write_reg8(REG_STATUS, 0);
    return true;
}

const char *touch_status(void)
{
    return s_status;
}

i2c_master_bus_handle_t touch_i2c_bus(void)
{
    return s_bus;
}
