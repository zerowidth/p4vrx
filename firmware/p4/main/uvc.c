// SPDX-License-Identifier: GPL-3.0-only
// Adapted from C5VRX p4usb (reference/p4usb/main/uvc.c), by Twotoz and the C5VRX contributors.
#include "uvc.h"

#include <string.h>

#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"

#include "console.h"
#include "usb.h"
#include "video.h"

/* In PSRAM: the controller's slave mode copies it into the FIFO by CPU, and a keyframe of a noisy picture
 * reached 40 KB, which dropped it and left the host without a reference for half a second. */
#define UVC_BUFFER_BYTES (256 * 1024)
#define PAYLOAD_HEADER 2
#define USB_HS_PACKET 512
/* Frames arrive every 16.7 ms, so this only runs out if the encoder fails. */
#define NEXT_WAIT_MS 100
/* A frame takes about 1 ms on the bus; a host that stops reading leaves it unfinished. */
#define XFER_WAIT_MS 500

static uint8_t *s_buffer;
static TaskHandle_t s_task;
static uint32_t s_starts;
static uint32_t s_sent;
static uint32_t s_skipped;
static uint32_t s_oversize;
static size_t s_largest;
static volatile bool s_restart;
/* Frames the class refused because the one before never finished on the bus. */
static uint32_t s_refused;
static volatile uint32_t s_refused_run;
/* A frame the class has refused this many times running is looked at, and again each time it adds up. */
#define STUCK_FRAMES 12
/* A host that holds off this long without asking is reported; it also looks like this while it has the stream closed. */
#define SILENT_FRAMES 120
#define DWC2_GRSTCTL (*(volatile uint32_t *)0x50000010)
#define GRSTCTL_FLUSH_TX (1u << 5)
#define DWC2_IN_EP(n, reg) (*(volatile uint32_t *)(0x50000900 + 0x20 * (n) + (reg)))
#define DIEPCTL_ENABLED (1u << 31)
#define DIEPCTL_DISABLE (1u << 30)
#define DIEPCTL_SET_NAK (1u << 27)
#define DIEPCTL_FIFO(ctl) ((ctl) >> 22 & 0x0f)
#define DIEPINT_NAK_EFFECTIVE (1u << 6)
#define DIEPINT_DISABLED (1u << 1)
#define DIEPTSIZ_BYTES(size) ((size)&0x7ffff)
#define DIEPTSIZ_PACKETS(size) ((size) >> 19 & 0x3ff)
/* The endpoint's control, interrupts, size left and FIFO space when it was last found stuck. */
static uint32_t s_stuck[4];
static uint32_t s_stuck_count;
static bool s_stuck_new;
/* Stuck frames whose transfer the controller had lost, and which were finished by hand. */
static uint32_t s_revived;
static size_t s_len;
static volatile bool s_accepted;
static SemaphoreHandle_t s_submitted;

/* On the USB task, since the class changes the same state there when the host starts or stops the stream. */
static void submit(void *arg)
{
    s_accepted = tud_video_n_streaming(0, 0) && tud_video_n_frame_xfer(0, 0, s_buffer, s_len);
    xSemaphoreGive(s_submitted);
}

static bool wait_for(volatile uint32_t *reg, uint32_t bit, bool set)
{
    int64_t give_up = esp_timer_get_time() + 2000;
    while (!(*reg & bit) == set) {
        if (esp_timer_get_time() > give_up) return false;
    }
    return true;
}

/* The class waits for a transfer to finish before it takes another frame, and two things stop one for good.
 * The endpoint was reset under it. Or the controller counts every byte as loaded and packets as still to
 * send, with its FIFO empty: it answers the host's requests with NAK and nothing will refill it. Either way
 * the endpoint is cleared and the transfer reported finished, which costs that one frame. An endpoint still
 * holding data is left alone: there the host has stopped asking. */
static void unstick(void *arg)
{
    int ep = USB_EP_VIDEO_IN & 0x0f;
    uint32_t ctl = DWC2_IN_EP(ep, 0x00), size = DWC2_IN_EP(ep, 0x10);
    bool reset = !(ctl & DIEPCTL_ENABLED);
    bool drained = !reset && DIEPTSIZ_BYTES(size) == 0 && DIEPTSIZ_PACKETS(size) != 0 &&
                   (DWC2_IN_EP(ep, 0x18) & 0xffff) * 4 >= USB_HS_PACKET;
    if (!reset && !drained && s_refused_run != SILENT_FRAMES) return;
    s_stuck[0] = ctl;
    s_stuck[1] = DWC2_IN_EP(ep, 0x08);
    s_stuck[2] = size;
    s_stuck[3] = DWC2_IN_EP(ep, 0x18);
    ++s_stuck_count;
    s_stuck_new = true;
    if (!reset && !drained) return;
    if (drained) {
        DWC2_IN_EP(ep, 0x00) |= DIEPCTL_SET_NAK;
        wait_for(&DWC2_IN_EP(ep, 0x08), DIEPINT_NAK_EFFECTIVE, true);
        DWC2_IN_EP(ep, 0x00) |= DIEPCTL_DISABLE;
        wait_for(&DWC2_IN_EP(ep, 0x08), DIEPINT_DISABLED, true);
        DWC2_IN_EP(ep, 0x08) = DIEPINT_DISABLED;
        DWC2_GRSTCTL = GRSTCTL_FLUSH_TX | DIEPCTL_FIFO(ctl) << 6;
        wait_for(&DWC2_GRSTCTL, GRSTCTL_FLUSH_TX, false);
    }
    ++s_revived;
    dcd_event_xfer_complete(TUD_OPT_RHPORT, USB_EP_VIDEO_IN, 0, XFER_RESULT_SUCCESS, false);
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, const video_probe_and_commit_control_t *parameters)
{
    ++s_starts;
    s_restart = true;
    /* A host that closed without SET_INTERFACE can leave the last frame's transfer queued. */
    usbd_edpt_stall(TUD_OPT_RHPORT, USB_EP_VIDEO_IN);
    usbd_edpt_clear_stall(TUD_OPT_RHPORT, USB_EP_VIDEO_IN);
    usbd_edpt_release(TUD_OPT_RHPORT, USB_EP_VIDEO_IN);
    xTaskNotifyGive(s_task);
    return VIDEO_ERROR_NONE;
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    xTaskNotifyGive(s_task);
}

/* Blocks on the producer rather than a timer, so each frame goes out once and none repeat. */
static void uvc_task(void *arg)
{
    uint32_t last_seq = 0;
    bool need_key = true;
    for (;;) {
        if (!tud_video_n_streaming(0, 0)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (s_restart) {
            s_restart = false;
            last_seq = 0;
            need_key = true;
            video_request_keyframe();
        }
        video_frame_t f;
        if (!video_next(VIDEO_READER_UVC, &f, pdMS_TO_TICKS(NEXT_WAIT_MS))) continue;
        if (last_seq && f.seq != last_seq + 1) s_skipped += f.seq - last_seq - 1;
        last_seq = f.seq;
        if (need_key && !f.key) {
            video_release(VIDEO_READER_UVC);
            continue;
        }
        need_key = false;
        size_t len = f.len;
        if (len > s_largest) s_largest = len;
        if (len >= UVC_BUFFER_BYTES) {
            ++s_oversize;
            video_release(VIDEO_READER_UVC);
            continue;
        }
        memcpy(s_buffer, f.data, len);
        video_release(VIDEO_READER_UVC);
        /* The class sends no zero-length packet, so a last payload filling whole packets would run into the
         * next one; a zero byte after the last NAL ends it short, and decoders take it as trailing_zero_8bits. */
        size_t data = CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE - PAYLOAD_HEADER;
        if (len % data && (len % data + PAYLOAD_HEADER) % USB_HS_PACKET == 0) s_buffer[len++] = 0;
        ulTaskNotifyTake(pdTRUE, 0);
        s_len = len;
        xSemaphoreTake(s_submitted, 0);
        usbd_defer_func(submit, NULL, false);
        if (xSemaphoreTake(s_submitted, pdMS_TO_TICKS(XFER_WAIT_MS)) != pdTRUE) continue;
        if (!s_accepted) {
            ++s_refused;
            if (++s_refused_run % STUCK_FRAMES == 0) usbd_defer_func(unstick, NULL, false);
            continue;
        }
        s_refused_run = 0;
        ++s_sent;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(XFER_WAIT_MS));
    }
}

void uvc_init(void)
{
    s_buffer = heap_caps_aligned_alloc(64, UVC_BUFFER_BYTES, MALLOC_CAP_SPIRAM);
    assert(s_buffer);
    s_submitted = xSemaphoreCreateBinary();
    xTaskCreate(uvc_task, "uvc", 4096, NULL, 5, &s_task);
}

void uvc_counts(uint32_t *sent, uint32_t *skipped)
{
    *sent = s_sent;
    *skipped = s_skipped;
}

void uvc_info(void)
{
    say("usb %s%s, uvc %s, %lu start(s), %lu sent, %lu skipped, %lu too large (largest %u bytes), %lu refused, %lu revived\n",
        tud_mounted() ? "mounted" : "not mounted", tud_suspended() ? " (suspended)" : "",
        tud_video_n_streaming(0, 0) ? "streaming" : "idle", (unsigned long)s_starts, (unsigned long)s_sent,
        (unsigned long)s_skipped, (unsigned long)s_oversize, (unsigned)s_largest, (unsigned long)s_refused, (unsigned long)s_revived);
    if (s_stuck_new) {
        s_stuck_new = false;
        say("uvc stuck %lu, %lu revived: endpoint ctl %08lx int %08lx size %08lx fifo %08lx\n",
            (unsigned long)s_stuck_count, (unsigned long)s_revived, (unsigned long)s_stuck[0], (unsigned long)s_stuck[1],
            (unsigned long)s_stuck[2], (unsigned long)s_stuck[3]);
    }
}

bool uvc_mounted(void)
{
    return tud_mounted();
}

/* macOS can leave the stream open with nobody reading, which looks like every frame refused. */
bool uvc_taking(void)
{
    return uvc_streaming() && s_refused_run < SILENT_FRAMES / 4;
}

bool uvc_streaming(void)
{
    return tud_mounted() && tud_video_n_streaming(0, 0);
}
