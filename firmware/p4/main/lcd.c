// SPDX-License-Identifier: Apache-2.0
// Display bring-up adapted from the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 BSP
// (esp32_p4_wifi6_touch_lcd_4_3.c), Apache-2.0, by Waveshare and Espressif.
#include "lcd.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_cache.h"
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
#include "font.h"
#include "ui.h"
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
#define PIC_W UI_PIC_W
#define STRIP_W UI_STRIP_W
#define STRIP_BYTES (STRIP_W * LCD_W * 2)

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

/* The UI strip right of the picture, drawn in landscape and turned into each frame buffer once after it
 * changes: the picture's scaling writes only its own block, so the buffers keep the strip in between. */
static canvas_t s_strip;
static SemaphoreHandle_t s_strip_lock;
static volatile uint8_t s_strip_stale;
/* Per frame buffer, the rows of it [lo, hi) not yet copied there. */
static uint16_t s_stale_lo[3], s_stale_hi[3];

static void strip_dirty(int lo, int hi)
{
    for (int k = 0; k < 3; ++k) {
        if (!(s_strip_stale & 1 << k) || lo < s_stale_lo[k]) s_stale_lo[k] = lo;
        if (!(s_strip_stale & 1 << k) || hi > s_stale_hi[k]) s_stale_hi[k] = hi;
    }
    s_strip_stale = 0x7;
}
static uint32_t s_strip_copies, s_strip_us;

static bool turned_270(void)
{
    return s_rotation == PPA_SRM_ROTATION_ANGLE_270;
}

/* The panel row a landscape span of width w from column x starts at: one way round, columns are rows. */
static uint32_t row_of(int x, int w)
{
    return turned_270() ? x : LCD_H - x - w;
}

/* The PPA turns the strip as it turns the picture, into the rows after the picture's or before them. */
static void copy_strip(int k)
{
    if (!(s_strip_stale & 1 << k)) return;
    int lo = s_stale_lo[k], hi = s_stale_hi[k];
    s_strip_stale &= ~(1 << k);
    if (hi <= lo) return;
    int64_t t0 = esp_timer_get_time();
    /* Strip row y lands in panel column y turned one way, LCD_W - 1 - y the other. */
    const ppa_srm_oper_config_t srm = {
        .in = {.buffer = s_strip.px, .pic_w = STRIP_W, .pic_h = LCD_W, .block_w = STRIP_W, .block_h = hi - lo,
               .block_offset_y = lo, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = s_fb[k], .buffer_size = FRAME_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                .block_offset_x = turned_270() ? LCD_W - hi : lo, .block_offset_y = row_of(ui_strip_x(), STRIP_W),
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = s_rotation,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_srm, &srm) != ESP_OK) return;
    s_strip_us = esp_timer_get_time() - t0;
    ++s_strip_copies;
}

/* The whole panel in landscape, for UI screens that cover the picture; only drawn while the picture is held. */
static canvas_t s_full;
static volatile bool s_full_on;
static volatile uint8_t s_full_stale;

static void copy_full(void *fb)
{
    const ppa_srm_oper_config_t srm = {
        .in = {.buffer = s_full.px, .pic_w = UI_W, .pic_h = UI_H, .block_w = UI_W, .block_h = UI_H, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = {.buffer = fb, .buffer_size = FRAME_BYTES, .pic_w = LCD_W, .pic_h = LCD_H, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = s_rotation,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ppa_do_scale_rotate_mirror(s_srm, &srm);
}

canvas_t *lcd_full_begin(void)
{
    if (!s_strip_lock) return NULL;
    if (!s_full.px) {
        s_full = (canvas_t){heap_caps_aligned_calloc(128, 1, UI_W * UI_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA), UI_W, UI_H, UI_W};
        if (!s_full.px) return NULL;
    }
    xSemaphoreTake(s_strip_lock, portMAX_DELAY);
    return &s_full;
}

void lcd_full_end(void)
{
    esp_cache_msync(s_full.px, UI_W * UI_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    s_full_on = true;
    s_full_stale = 7;
    xSemaphoreGive(s_strip_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

void lcd_full_off(void)
{
    if (!s_full_on) return;
    s_full_on = false;
    /* The picture comes back with the next frame; the strip has to be put back beside it. */
    strip_dirty(0, LCD_W);
}

/* Landscape coordinates for a point given as the panel's own (480 across, 800 down), turned as the picture is. */
void lcd_landscape(int px, int py, int *x, int *y)
{
    if (turned_270()) {
        *x = py;
        *y = LCD_W - 1 - px;
    } else {
        *x = LCD_H - 1 - py;
        *y = px;
    }
}

canvas_t *lcd_strip_begin(void)
{
    if (!s_strip_lock) return NULL;
    xSemaphoreTake(s_strip_lock, portMAX_DELAY);
    return &s_strip;
}

void lcd_strip_end(int lo, int hi)
{
    bool paused = s_paused;
    esp_cache_msync(s_strip.px, STRIP_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (hi > lo) strip_dirty(lo, hi);
    xSemaphoreGive(s_strip_lock);
    if (paused && s_task) xTaskNotifyGive(s_task);
}

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
static SemaphoreHandle_t s_scaled;
/* A field offered and not yet scaled into a frame buffer. */
static volatile bool s_scaling;

static void show(const uint8_t *yuv)
{
    void *fb = s_fb[s_back];
    int64_t t0 = esp_timer_get_time();
    const ppa_srm_oper_config_t srm = {
        .in = {.buffer = yuv, .pic_w = VIDEO_WIDTH, .pic_h = VIDEO_HEIGHT, .block_w = VIDEO_WIDTH, .block_h = VIDEO_HEIGHT,
               .srm_cm = PPA_SRM_COLOR_MODE_YUV420, .yuv_range = PPA_COLOR_RANGE_FULL, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .out = {.buffer = fb, .buffer_size = FRAME_BYTES, .pic_w = LCD_W, .pic_h = LCD_H,
                .block_offset_x = 0, .block_offset_y = row_of(ui_picture_x(), PIC_W), .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .rotation_angle = s_rotation,
        .scale_x = SCALE_X,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &srm);
    s_scaling = false;
    xSemaphoreGive(s_scaled);
    if (err != ESP_OK) return;
    s_ppa_us = esp_timer_get_time() - t0;
    if (s_strip_stale & 1 << s_back && xSemaphoreTake(s_strip_lock, 0) == pdTRUE) {
        copy_strip(s_back);
        xSemaphoreGive(s_strip_lock);
    }
    /* Two frames queued within one refresh would leave the next target on screen. */
    if (s_queued && xSemaphoreTake(s_vsync, pdMS_TO_TICKS(40)) != pdTRUE) ++s_late;
    xSemaphoreTake(s_vsync, 0);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, fb);
    s_queued = true;
    s_back = (s_back + 1) % 3;
    ++s_shown;
}

/* The pause sign over a held picture, once into each buffer: the next frame after the hold paints over it. */
static bool s_pause_drawn;

static uint16_t blend(uint16_t px, int halo, int fill)
{
    int r = px >> 11, g = px >> 5 & 63, b = px & 31;
    /* The halo darkens to 85% at most; the fill is a light grey, 240 of 255. */
    int keep = 255 - halo * 217 / 255;
    r = r * keep / 255, g = g * keep / 255, b = b * keep / 255;
    r = (r * (255 - fill) + 29 * fill) / 255;
    g = (g * (255 - fill) + 59 * fill) / 255;
    b = (b * (255 - fill) + 29 * fill) / 255;
    return (uint16_t)(r << 11 | g << 5 | b);
}

static void draw_pause(uint16_t *fb)
{
    const overlay_t *o = &pause_overlay;
    /* Centred on the picture, in landscape; each landscape point mapped into the panel's rows as the PPA turns it. */
    int x0 = ui_picture_x() + PIC_W / 2 - o->w / 2, y0 = LCD_W / 2 - o->h / 2;
    for (int y = 0; y < o->h; ++y)
        for (int x = 0; x < o->w; ++x) {
            int i = y * o->w + x, halo = o->halo[i], fill = o->fill[i];
            if (!halo && !fill) continue;
            int lx = x0 + x, ly = y0 + y;
            int row = turned_270() ? lx : LCD_H - 1 - lx, col = turned_270() ? LCD_W - 1 - ly : ly;
            uint16_t *p = &fb[row * LCD_W + col];
            *p = blend(*p, halo, fill);
        }
    esp_cache_msync(fb, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void lcd_task(void *arg)
{
    int64_t window = esp_timer_get_time();
    uint32_t frames = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_paused) {
            /* No frames come while the picture is held, so a changed strip, or the whole-panel UI, goes into every
             * buffer, the one on screen too. */
            if (s_full_on && s_full_stale && xSemaphoreTake(s_strip_lock, 0) == pdTRUE) {
                for (int k = 0; k < 3; ++k) copy_full(s_fb[k]);
                s_full_stale = 0;
                xSemaphoreGive(s_strip_lock);
                continue;
            }
            if (!s_full_on && s_strip_stale && xSemaphoreTake(s_strip_lock, 0) == pdTRUE) {
                for (int k = 0; k < 3; ++k) copy_strip(k);
                xSemaphoreGive(s_strip_lock);
            }
            if (!s_full_on && !s_pause_drawn) {
                for (int k = 0; k < 3; ++k) draw_pause(s_fb[k]);
                s_pause_drawn = true;
            }
            continue;
        }
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
    s_scaled = xSemaphoreCreateBinary();
    backlight_on();
    panel_init();
    const ppa_client_config_t srm = {.oper_type = PPA_OPERATION_SRM};
    const ppa_client_config_t fill = {.oper_type = PPA_OPERATION_FILL};
    ESP_ERROR_CHECK(ppa_register_client(&srm, &s_srm));
    ESP_ERROR_CHECK(ppa_register_client(&fill, &s_fill));
    clear(s_fb[0]);
    clear(s_fb[1]);
    clear(s_fb[2]);
    s_strip = (canvas_t){heap_caps_aligned_calloc(128, 1, STRIP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA), STRIP_W, LCD_W, STRIP_W};
    if (s_strip.px) s_strip_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(lcd_task, "lcd", 4096, NULL, 3, &s_task, 0);
}

void lcd_blank(void)
{
    if (!s_paused) return;
    for (int k = 0; k < 3; ++k) clear(s_fb[k]);
    strip_dirty(0, LCD_W);
    if (s_task) xTaskNotifyGive(s_task);
}

void lcd_hold(bool hold)
{
    s_paused = hold;
    s_pause_drawn = false;
    if (hold && s_task) xTaskNotifyGive(s_task);
}

bool lcd_busy(void)
{
    return s_scaling;
}

bool lcd_wait_scaled(uint32_t ms)
{
    return s_scaled && xSemaphoreTake(s_scaled, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void lcd_offer(const uint8_t *yuv)
{
    if (!s_task || s_paused) return;
    if (s_scaled) xSemaphoreTake(s_scaled, 0);
    s_scaling = true;
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
    if (strcmp(sub, "grab") == 0) {
        /* The buffer last handed to the panel, as the panel scans it: 480 wide, 800 tall. */
        bool paused = s_paused;
        s_paused = true;
        vTaskDelay(pdMS_TO_TICKS(50));
        const void *fb = s_fb[(s_back + 2) % 3];
        esp_cache_msync((void *)fb, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        say("lcd grab begin %d %d\n", LCD_W, LCD_H);
        console_base64(fb, FRAME_BYTES);
        say("lcd grab end\n");
        s_paused = paused;
        return;
    }
    if (strcmp(sub, "rotate") == 0) {
        s_rotation = s_rotation == PPA_SRM_ROTATION_ANGLE_270 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_270;
        clear(s_fb[0]);
        clear(s_fb[1]);
        clear(s_fb[2]);
        strip_dirty(0, LCD_W);
        say("ok, turned %d degrees\n", s_rotation == PPA_SRM_ROTATION_ANGLE_270 ? 270 : 90);
        return;
    }
    say("lcd %lu.%lu fps, %lu shown of %lu offered, %lu missed a refresh, last scale and turn %lu us\n",
        (unsigned long)(s_fps_tenths / 10), (unsigned long)(s_fps_tenths % 10), (unsigned long)s_shown,
        (unsigned long)s_offered, (unsigned long)s_late, (unsigned long)s_ppa_us);
    say("strip copied %lu times, the last in %lu us\n", (unsigned long)s_strip_copies, (unsigned long)s_strip_us);
}
