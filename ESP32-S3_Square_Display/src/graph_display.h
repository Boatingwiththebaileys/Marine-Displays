#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Create or update graph display on a screen
// screen_num: 0-4 (Screen1-Screen5)
// value: The numeric value to add to the graph (series 1)
// unit: Optional unit string (e.g., "RPM", "°C", "%")
// description: Optional description shown in top left corner
// value2: The numeric value for series 2 (optional, use NAN if not available)
// unit2: Optional unit string for series 2
// description2: Optional description for series 2
void graph_display_create(int screen_num);
void graph_display_update(int screen_num, float value, const char* unit, const char* description,
                          float value2, const char* unit2, const char* description2);
void graph_display_destroy(int screen_num);

#ifdef __cplusplus
}
#endif
