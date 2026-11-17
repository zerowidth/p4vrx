#include "cpu.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console.h"

#define MAX_TASKS 48

typedef struct {
    TaskStatus_t tasks[MAX_TASKS];
    UBaseType_t n;
    configRUN_TIME_COUNTER_TYPE total;
} snapshot_t;

static snapshot_t s_a, s_b;

static void take(snapshot_t *s)
{
    s->n = uxTaskGetSystemState(s->tasks, MAX_TASKS, &s->total);
}

static configRUN_TIME_COUNTER_TYPE before(TaskHandle_t h)
{
    for (UBaseType_t i = 0; i < s_a.n; ++i)
        if (s_a.tasks[i].xHandle == h) return s_a.tasks[i].ulRunTimeCounter;
    return 0;
}

/* Run time is esp_timer microseconds, so each core has the window's length to spend. */
void cpu_command(int argc, char **argv)
{
    uint32_t ms = argc > 1 ? (uint32_t)atoi(argv[1]) : 2000;
    if (ms < 100 || ms > 10000) ms = 2000;
    take(&s_a);
    vTaskDelay(pdMS_TO_TICKS(ms));
    take(&s_b);
    double window = (double)(s_b.total - s_a.total);
    if (window <= 0) {
        say("err no run time\n");
        return;
    }
    double busy[3] = {0};
    for (UBaseType_t i = 0; i < s_b.n; ++i) {
        const TaskStatus_t *t = &s_b.tasks[i];
        double pct = 100.0 * (double)(t->ulRunTimeCounter - before(t->xHandle)) / window;
        BaseType_t core = xTaskGetCoreID(t->xHandle);
        bool idle = strncmp(t->pcTaskName, "IDLE", 4) == 0;
        if (!idle) busy[core == 0 ? 0 : core == 1 ? 1 : 2] += pct;
        if (pct >= 0.1) say("  %-14s core %s prio %2u %5.1f%%\n", t->pcTaskName, core == 0 ? "0" : core == 1 ? "1" : "-",
                            (unsigned)t->uxCurrentPriority, pct);
    }
    for (UBaseType_t i = 0; i < s_b.n; ++i) {
        const TaskStatus_t *t = &s_b.tasks[i];
        if (strncmp(t->pcTaskName, "IDLE", 4) != 0) continue;
        double pct = 100.0 * (double)(t->ulRunTimeCounter - before(t->xHandle)) / window;
        say("core %d idle %.1f%%\n", (int)xTaskGetCoreID(t->xHandle), pct);
    }
    say("pinned busy: core 0 %.1f%%, core 1 %.1f%%; unpinned %.1f%% over %lu ms\n", busy[0], busy[1], busy[2],
        (unsigned long)ms);
}
