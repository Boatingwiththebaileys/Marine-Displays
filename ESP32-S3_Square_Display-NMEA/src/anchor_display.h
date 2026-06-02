#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Create the anchor alarm display on the given screen (0-4).
void anchor_display_create(int screen_num);

// Update display: own-boat lat/lon, COG in degrees, SOG in m/s, depth in metres (NAN = no data).
// Called every loop tick when this display type is active.
void anchor_display_update(int screen_num, float own_lat, float own_lon,
                           float cog_deg, float sog_ms, float depth_m);

// Remove all LVGL objects for this screen.
void anchor_display_destroy(int screen_num);

// Called by main loop when a touch event hits this screen.
// x, y are raw screen coordinates (0..479).
void anchor_display_touch(int screen_num, int x, int y);

#ifdef __cplusplus
}
#endif
