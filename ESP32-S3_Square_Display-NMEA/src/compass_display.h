#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Create a compass display on the given screen (screen_num 0-4).
// After creation, call compass_display_update() each time new heading data arrives.
void compass_display_create(int screen_num);

// Update the compass display with a new heading in degrees (0-359.9).
// heading_true: 1 = True heading (°T label), 0 = Magnetic (°M label)
void compass_display_update(int screen_num, float heading_deg, int heading_true);

// Update the bottom-left / bottom-right extra data fields.
// value: NaN shows "---"; unit/description may be NULL or empty.
void compass_display_update_bl(int screen_num, float value, const char* unit, const char* description);
void compass_display_update_br(int screen_num, float value, const char* unit, const char* description);

// Remove all LVGL objects owned by the compass display for the given screen.
void compass_display_destroy(int screen_num);

#ifdef __cplusplus
}
#endif
