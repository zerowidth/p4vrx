#include "stall.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "freertos/freertos_debug.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "riscv/rvruntime-frames.h"

#include "console.h"
#include "decode.h"

#define CHECK_MS 500
#define CALLERS 14
#define MAGIC 0x5374616c

/* The console's limit covers its longest command, a channel change that probes the C5 twice over. */
static const struct {
    const char *name;
    uint32_t limit_ms;
} s_tasks[STALL_TASKS] = {
    [STALL_CONSOLE] = {"console", 60000},
    [STALL_USB] = {"usb", 3000},
    [STALL_UVC] = {"uvc", 3000},
};

/* Where the stalled task was waiting: its saved pc and ra, then the code addresses on its stack. */
typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t task;
    uint32_t uptime_ms;
    uint32_t pc;
    uint32_t ra;
    uint32_t callers[CALLERS];
} record_t;

static RTC_NOINIT_ATTR record_t s_record;
static TaskHandle_t s_handles[STALL_TASKS];
static volatile uint32_t s_beat_ms[STALL_TASKS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void stall_watch(stall_task_t task)
{
    s_beat_ms[task] = now_ms();
    s_handles[task] = xTaskGetCurrentTaskHandle();
}

void stall_beat(stall_task_t task)
{
    s_beat_ms[task] = now_ms();
}

static void record(stall_task_t task)
{
    if (s_record.magic != MAGIC) s_record.count = 0;
    s_record.magic = MAGIC;
    ++s_record.count;
    s_record.task = task;
    s_record.uptime_ms = now_ms();
    s_record.pc = s_record.ra = 0;
    memset(s_record.callers, 0, sizeof s_record.callers);
    TaskSnapshot_t snap;
    if (vTaskGetSnapshot(s_handles[task], &snap) != pdTRUE) return;
    const RvExcFrame *frame = (const RvExcFrame *)snap.pxTopOfStack;
    s_record.pc = frame->mepc;
    s_record.ra = frame->ra;
    int n = 0;
    for (const uint32_t *p = (const uint32_t *)(frame + 1); p < (const uint32_t *)snap.pxEndOfStack && n < CALLERS; ++p) {
        if (esp_ptr_executable((const void *)*p)) s_record.callers[n++] = *p;
    }
}

static void watch_task(void *arg)
{
    bool recorded[STALL_TASKS] = {0};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
        bool restart = false;
        for (int t = 0; t < STALL_TASKS; ++t) {
            if (!s_handles[t] || now_ms() - s_beat_ms[t] < s_tasks[t].limit_ms) {
                recorded[t] = false;
                continue;
            }
            if (!recorded[t]) record(t);
            recorded[t] = true;
            /* A stuck console still leaves the picture, so it waits until nobody is flying. */
            int level;
            if (t != STALL_CONSOLE || !decode_signal(&level)) restart = true;
        }
        if (restart) esp_restart();
    }
}

void stall_init(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON) s_record.magic = 0;
    xTaskCreate(watch_task, "stall", 3072, NULL, configMAX_PRIORITIES - 2, NULL);
}

void stall_info(void)
{
    if (s_record.magic != MAGIC) return;
    say("stall %lu: %s stopped at uptime %lu ms, pc %08lx ra %08lx, stack", (unsigned long)s_record.count,
        s_record.task < STALL_TASKS ? s_tasks[s_record.task].name : "?", (unsigned long)s_record.uptime_ms,
        (unsigned long)s_record.pc, (unsigned long)s_record.ra);
    for (int i = 0; i < CALLERS && s_record.callers[i]; ++i) say(" %08lx", (unsigned long)s_record.callers[i]);
    say("\n");
}
