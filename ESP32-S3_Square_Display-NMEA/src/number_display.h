#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Create or update number display on a screen
// screen_num: 0-4 (Screen1-Screen5)
// value: The numeric value to display
// unit: Optional unit string (e.g., "RPM", "°C", "%")
// description: Optional description shown in top left corner
void number_display_create(int screen_num);
void number_display_update(int screen_num, float value, const char* unit, const char* description);
void number_display_destroy(int screen_num);

#ifdef __cplusplus
}
#endif
