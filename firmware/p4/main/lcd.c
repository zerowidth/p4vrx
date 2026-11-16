// SPDX-License-Identifier: Apache-2.0
// Display bring-up adapted from the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 BSP
// (esp32_p4_wifi6_touch_lcd_4_3.c), Apache-2.0, by Waveshare and Espressif.
#include "lcd.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "console.h"
#include "video.h"

#define LCD_W 480
#define LCD_H 800
#define PIN_BACKLIGHT 26
#define PIN_RESET 27
#define DSI_PHY_LDO 3
#define DSI_PHY_MV 2500
#define FRAME_BYTES (LCD_W * LCD_H * 2)
/* The decoded 720x480 is 4:3 with narrow pixels: 630x480 on the panel's square ones. */
#define SCALE_X 0.875f
#define PIC_W 630

static const st7701_lcd_init_cmd_t s_st7701_init[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09, 0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},

    {0xB1, (uint8_t[]){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08, 0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},

    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x4E}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},

    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},

    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},

    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},

    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},

    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static esp_lcd_panel_handle_t s_panel;
static ppa_client_handle_t s_srm, s_fill;
/* Three, so the next frame goes into one the panel neither shows nor is about to. */
static void *s_fb[3];
static int s_back;
static volatile bool s_queued;
static volatile bool s_paused;
static SemaphoreHandle_t s_vsync;
static TaskHandle_t s_task;
static const uint8_t *volatile s_pending;
static ppa_srm_rotation_angle_t s_rotation = PPA_SRM_ROTATION_ANGLE_270;
static uint32_t s_shown, s_offered, s_late, s_ppa_us, s_fps_tenths;

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *e, void *arg)
{
    BaseType_t woken = pdFALSE;
    s_queued = false;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static void backlight_on(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT, .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    /* The backlight driver is active low. */
    const ledc_channel_config_t ch = {
        .gpio_num = PIN_BACKLIGHT, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_1, .duty = 1023, .flags.output_invert = 1};
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

static void panel_init(void)
{
    static esp_ldo_channel_handle_t phy;
    const esp_ldo_channel_config_t ldo = {.chan_id = DSI_PHY_LDO, .voltage_mv = DSI_PHY_MV};
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo, &phy));

    esp_lcd_dsi_bus_handle_t bus;
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0, .num_data_lanes = 2, .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = 500};
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &bus));
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_dbi_io_config_t dbi = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dbi, &io));

    /* 30 MHz over 576 x 870 clocks a frame is 59.9 Hz. */
    esp_lcd_dpi_panel_config_t dpi = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 30,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 3,
        .video_timing = {
            .h_size = LCD_W, .v_size = LCD_H,
            .hsync_back_porch = 42, .hsync_pulse_width = 12, .hsync_front_porch = 42,
            .vsync_back_porch = 2, .vsync_pulse_width = 8, .vsync_front_porch = 60,
        },
    };
    st7701_vendor_config_t vendor = {
        .init_cmds = s_st7701_init,
        .init_cmds_size = sizeof s_st7701_init / sizeof s_st7701_init[0],
        .flags.use_mipi_interface = 1,
        .mipi_config = {.dsi_bus = bus, .dpi_config = &dpi},
    };
    const esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 16, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .reset_gpio_num = PIN_RESET, .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &dev, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, &s_fb[0], &s_fb[1], &s_fb[2]));
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_vsync = on_vsync};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL));
}

static void clear(void *fb)
{
    const ppa_fill_oper_config_t fill = {
        .out = {.buffer = fb, .buffer_size = FRAME_BYTES, .pic_w = LCD_W, .pic_h = LCD_H, .fill_cm = PPA_FILL_COLOR_MODE_RGB565},
        .fill_block_w = LCD_W, .fill_block_h = LCD_H, .fill_argb_color.val = 0xff000000, .mode = PPA_TRANS_MODE_BLOCKING};
    ESP_ERROR_CHECK(ppa_do_fill(s_fill, &fill));
}

/* Scales and turns a frame into the back buffer, then shows it from the next refresh on. */
static void show(const uint8_t *yuv)
{
    void *fb = s_fb[s_back];
    int64_t t0 = esp_timer_get_time();
    const ppa_srm_oper_config_t srm = {
        .in = {.buffer = yuv, .pic_w = VIDEO_WIDTH, .pic_h = VIDEO_HEIGHT, .block_w = VIDEO_WIDTH, .block_h = VIDEO_HEIGHT,
               .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_FULL, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .out = {.buffer = fb, .buffer_size = FRAME_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                .block_offset_x = 0, .block_offset_y = (LCD_H - PIC_W) / 2, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = s_rotation,
        .scale_x = SCALE_X,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_srm, &srm) != ESP_OK) return;
    s_ppa_us = esp_timer_get_time() - t0;
    /* Two frames queued within one refresh would leave the next target on screen. */
    if (s_queued && xSemaphoreTake(s_vsync, pdMS_TO_TICKS(40)) != pdTRUE) ++s_late;
    xSemaphoreTake(s_vsync, 0);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, fb);
    s_queued = true;
    s_back = (s_back + 1) % 3;
    ++s_shown;
}

static void lcd_task(void *arg)
{
    int64_t window = esp_timer_get_time();
    uint32_t frames = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint8_t *yuv = s_pending;
        if (!yuv) continue;
        show(yuv);
        ++frames;
        int64_t now = esp_timer_get_time();
        if (now - window >= 1000000) {
            s_fps_tenths = frames * 10000000LL / (now - window);
            window = now;
            frames = 0;
        }
    }
}

void lcd_init(void)
{
    s_vsync = xSemaphoreCreateBinary();
    backlight_on();
    panel_init();
    const ppa_client_config_t srm = {.oper_type = PPA_OPERATION_SRM};
    const ppa_client_config_t fill = {.oper_type = PPA_OPERATION_FILL};
    ESP_ERROR_CHECK(ppa_register_client(&srm, &s_srm));
    ESP_ERROR_CHECK(ppa_register_client(&fill, &s_fill));
    clear(s_fb[0]);
    clear(s_fb[1]);
    clear(s_fb[2]);
    xTaskCreatePinnedToCore(lcd_task, "lcd", 4096, NULL, 3, &s_task, 0);
}

void lcd_offer(const uint8_t *yuv)
{
    if (!s_task || s_paused) return;
    ++s_offered;
    s_pending = yuv;
    xTaskNotifyGive(s_task);
}

void lcd_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "";
    if (strcmp(sub, "backlight") == 0 && argc > 2) {
        int pct = atoi(argv[2]);
        pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * pct / 100);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        say("ok, backlight %d%%\n", pct);
        return;
    }
    if (strcmp(sub, "show") == 0 && argc > 2) {
        s_paused = strcmp(argv[2], "off") == 0;
        say("ok\n");
        return;
    }
    if (strcmp(sub, "rotate") == 0) {
        s_rotation = s_rotation == PPA_SRM_ROTATION_ANGLE_270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_270;
        say("ok, turned %d degrees\n", s_rotation == PPA_SRM_ROTATION_ANGLE_270 ? 270 : 90);
        return;
    }
    say("lcd %lu.%lu fps, %lu shown of %lu offered, %lu missed a refresh, last scale and turn %lu us\n",
        (unsigned long)(s_fps_tenths / 10), (unsigned long)(s_fps_tenths % 10), (unsigned long)s_shown,
        (unsigned long)s_offered, (unsigned long)s_late, (unsigned long)s_ppa_us);
}
