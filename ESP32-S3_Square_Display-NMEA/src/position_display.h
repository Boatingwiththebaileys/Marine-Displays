#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Create a Position & Time display on screen_num (0-4).
void position_display_create(int screen_num);

// Update position display with current lat/lon (decimal degrees) and ISO-8601
// datetime string (e.g. "2026-03-04T14:30:07.000Z").  Pass NaN for unknown.
void position_display_update(int screen_num, float lat, float lon,
                             const char* datetime_str);

// Destroy all LVGL objects for the given screen.
void position_display_destroy(int screen_num);

#ifdef __cplusplus
}
#endif
