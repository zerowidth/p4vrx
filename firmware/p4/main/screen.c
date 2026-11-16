#include "screen.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "decode.h"
#include "lcd.h"
#include "ui.h"
#include "uvc.h"

/* Feeds the UI the receiver's state ten times a second and redraws the strip when it asks. */
static void screen_task(void *arg)
{
    for (;;) {
        ui_state_t st = {0};
        unsigned mhz;
        st.decoding = decode_running();
        strlcpy(st.channel, decode_channel(&mhz), sizeof st.channel);
        st.mhz = (uint16_t)mhz;
        st.locked = decode_signal(&st.level);
        st.usb = uvc_streaming() ? UI_USB_STREAMING : uvc_mounted() ? UI_USB_CONNECTED : UI_USB_NONE;
        if (ui_update(&st)) {
            canvas_t *c = lcd_strip_begin();
            if (c) {
                ui_draw(c);
                lcd_strip_end();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void screen_init(void)
{
    xTaskCreatePinnedToCore(screen_task, "screen", 4096, NULL, 2, NULL, 0);
}
