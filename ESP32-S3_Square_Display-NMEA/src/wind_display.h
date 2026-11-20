#pragma once
// wind_display.h — built-in wind screen (apparent + true wind, drift).
// The same file is used by the Signal K and NMEA 2000 Square firmware; each
// firmware fills a WindInputs from its own data and calls wind_display_update().

#include "lvgl.h"
#include <stdint.h>

// Data older than this is treated as missing ("--").
#define WIND_STALE_MS 5000

// Inputs, in degrees and m/s. Use NAN for anything missing or stale.
typedef struct {
    float awa_deg;        // apparent wind angle, from the bow (0-360 or ±180, starboard +)
    float aws_ms;         // apparent wind speed
    float twa_deg;        // true wind angle from the network (NAN = work it out)
    float tws_ms;         // true wind speed from the network
    float twd_deg;        // true wind direction from the network, referenced to true north
    float hdg_deg;        // heading
    int   hdg_true;       // 1 = hdg_deg is true, 0 = magnetic
    float variation_deg;  // magnetic variation, east positive (NAN = unknown)
    float stw_ms;         // speed through water
    float sog_ms;         // speed over ground
    float cog_deg;        // course over ground (true)
    float set_deg;        // current set from the network, true (NAN = work it out)
    float drift_ms;       // current drift from the network
} WindInputs;

#ifdef __cplusplus
extern "C" {
#endif

void wind_display_create(int screen_num);    // 0-4
void wind_display_destroy(int screen_num);
void wind_display_update(int screen_num, const WindInputs* in);

// Bottom fields. side 0 = left, 1 = right.
// use_default: show the built-in value (left TWS, right TWD); value/unit/desc ignored.
// Otherwise value (NAN shows "--"), unit and description come from the chosen data.
void wind_display_update_field(int screen_num, int side, int use_default,
                               float value, const char* unit, const char* description);

#ifdef __cplusplus
}
#endif
