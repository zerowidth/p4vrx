// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/usb.c), by Twotoz and the C5VRX contributors.
#include "usb.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "bridge.h"
#include "console.h"

#define USB_VID 0x303a
/* Not the ROM loader's 0x0012 or USB-Serial/JTAG's 0x1001, so esptool resets it with DTR and RTS. */
#define USB_PID 0x8000
#define EP_CDC_NOTIF 0x81
#define EP_CDC_OUT 0x02
#define EP_CDC_IN 0x82
/* A closed or stalled reader should cost the console a moment, not hang it; any progress restarts it. */
#define WRITE_TIMEOUT_US (200 * 1000)

enum { ITF_CDC, ITF_CDC_DATA, ITF_COUNT };
enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_CONSOLE };

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LEN, 0, 500),
    TUD_CDC_DESCRIPTOR(ITF_CDC, STR_CONSOLE, EP_CDC_NOTIF, 8, EP_CDC_OUT, EP_CDC_IN, 512),
};
_Static_assert(sizeof s_config == CONFIG_LEN, "configuration descriptor length");

static char s_serial[13];
static const char *const s_strings[] = {
    [STR_MANUFACTURER] = "p4vrx",
    [STR_PRODUCT] = "p4vrx Receiver",
    [STR_SERIAL] = s_serial,
    [STR_CONSOLE] = "p4vrx Console",
};

static usb_phy_handle_t s_phy;
static SemaphoreHandle_t s_rx_ready;

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    return s_config;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t desc[32];
    size_t n;
    if (index == STR_LANG) {
        desc[1] = 0x0409;
        n = 1;
    } else if (index < sizeof s_strings / sizeof s_strings[0] && s_strings[index]) {
        const char *s = s_strings[index];
        for (n = 0; s[n] && n < 31; ++n) desc[1 + n] = s[n];
    } else {
        return NULL;
    }
    desc[0] = TUSB_DESC_STRING << 8 | (2 * n + 2);
    return desc;
}

void tud_cdc_rx_cb(uint8_t itf)
{
    xSemaphoreGive(s_rx_ready);
}

/* esptool's reset holds RTS (EN) with DTR (IO0) released, then releases RTS with DTR held; the P4 answers
 * by rebooting into its own ROM loader. */
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    static bool in_reset;
    if (bridge_active()) return;
    if (rts && !dtr) {
        in_reset = true;
    } else if (!rts) {
        if (in_reset && dtr) console_reboot(true);
        in_reset = false;
    }
}

static void usb_task(void *arg)
{
    for (;;) tud_task();
}

void usb_init(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_rx_ready = xSemaphoreCreateBinary();

    const usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .target = USB_PHY_TARGET_INT,
        .otg_speed = USB_PHY_SPEED_HIGH,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy, &s_phy));
    if (!tusb_init()) abort();
    xTaskCreate(usb_task, "usb", 4096, NULL, 5, NULL);
}

void usb_console_info(void)
{
    say("usb console: mounted %d, suspended %d, DTR %d\n", tud_mounted(), tud_suspended(), tud_cdc_connected());
}

bool usb_console_connected(void)
{
    return tud_cdc_connected();
}

void usb_console_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    int64_t deadline = esp_timer_get_time() + WRITE_TIMEOUT_US;
    while (len && tud_cdc_connected()) {
        uint32_t n = tud_cdc_write(p, len);
        tud_cdc_write_flush();
        p += n;
        len -= n;
        if (n) deadline = esp_timer_get_time() + WRITE_TIMEOUT_US;
        if (len) {
            if (esp_timer_get_time() > deadline) return;
            vTaskDelay(1);
        }
    }
}

size_t usb_console_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (!tud_cdc_available()) xSemaphoreTake(s_rx_ready, pdMS_TO_TICKS(timeout_ms));
    return tud_cdc_read(buf, len);
}
