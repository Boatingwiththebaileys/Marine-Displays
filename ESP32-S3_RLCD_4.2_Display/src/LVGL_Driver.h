#ifndef LVGL_DRIVER_H
#define LVGL_DRIVER_H

#include "lvgl.h"

#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 50

void LVGL_Init();
bool LVGL_Lock(int timeout_ms = -1);
void LVGL_Unlock();

#endif // LVGL_DRIVER_H
