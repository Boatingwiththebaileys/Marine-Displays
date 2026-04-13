#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdint.h>

// Maximum number of AIS targets to track and render
#define AIS_MAX_TARGETS 20

// AIS range presets (nautical miles) — stored in ScreenConfig.graph_time_range
// (repurposed for AIS when display_type == DISPLAY_TYPE_AIS)
typedef enum {
    AIS_RANGE_0_1NM = 0,  // 0.1 NM
    AIS_RANGE_0_5NM = 1,  // 0.5 NM
    AIS_RANGE_1NM   = 2,  // 1 NM
    AIS_RANGE_2NM   = 3,  // 2 NM
    AIS_RANGE_5NM   = 4,  // 5 NM
    AIS_RANGE_10NM  = 5,  // 10 NM
    AIS_RANGE_20NM  = 6   // 20 NM
} AisRange;

// Create the AIS radar display on the given screen (screen_num 0-4).
void ais_display_create(int screen_num);

// Redraw the AIS display with current data.
// own_lat, own_lon: own ship position (degrees)
// own_cog: own course over ground (degrees true)
// own_sog: own speed over ground (knots)
// own_hdg: own heading (degrees true) — used for head-up north indicator
void ais_display_update(int screen_num, float own_lat, float own_lon,
                        float own_cog, float own_sog, float own_hdg);

// Remove all LVGL objects owned by the AIS display for the given screen.
void ais_display_destroy(int screen_num);

// Fetch AIS target data from Signal K REST API.
// Call periodically (every ~5 seconds) from a task or the main loop.
// server_ip and server_port are the Signal K server connection details.
void ais_fetch_targets(const char* server_ip, uint16_t server_port);

// ─── N2K AIS target update (called from NMEA 2000 PGN handlers) ─────────────
// Update or insert an AIS target position (from PGN 129038 / 129039).
// mmsi: MMSI number, lat/lon in degrees, cog in degrees true, sog in knots.
void ais_n2k_update_position(uint32_t mmsi, double lat, double lon,
                             double cog_deg, double sog_kn);

// Update the name of an existing AIS target (from PGN 129794 / 129809).
void ais_n2k_update_name(uint32_t mmsi, const char* name);

#ifdef __cplusplus
}
#endif
