#pragma once

/* Each of these loops must come round every few seconds; one that stops is recorded and the P4 restarts. */
typedef enum { STALL_CONSOLE, STALL_USB, STALL_UVC, STALL_TASKS } stall_task_t;

/* Starts watching the calling task. */
void stall_watch(stall_task_t task);
void stall_beat(stall_task_t task);
void stall_init(void);
/* Prints the last stall, which is kept across the restart it caused. */
void stall_info(void);
