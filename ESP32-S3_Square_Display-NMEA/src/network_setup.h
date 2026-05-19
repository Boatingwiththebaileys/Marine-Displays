
#ifndef NETWORK_SETUP_H
#define NETWORK_SETUP_H

#include <Arduino.h>
#include <WebServer.h>
#include <vector>
#include "signalk_config.h"  // For NUM_SCREENS, TOTAL_PARAMS

#include "calibration_types.h"
extern GaugeCalibrationPoint gauge_cal[NUM_SCREENS][2][5];

// Global synchronous web server instance (WebServer — handleClient() called
// from loop() on Core 1; HTTP handlers also run on Core 1).
extern WebServer config_server;

// Auto-scroll interval in seconds (0 = off)
extern uint16_t auto_scroll_sec;
// Screen-off timeout in minutes (0 = always on)
extern uint16_t screen_off_timeout_min;

// Request the UI to change auto-scroll interval at runtime
void set_auto_scroll_interval(uint16_t sec);

// Initialize network (WiFi + WebServer) with web UI for configuration
void setup_network();

// Set this flag from any context to request apply_all_screen_visuals() be called
// safely from loop() on the next iteration, avoiding LVGL access from HTTP handlers.
extern volatile bool g_pending_visual_apply;
extern volatile bool g_error_screen_active;
// Per-screen lazy re-apply flags: set when a save happens while the screen was
// inactive; cleared when that screen becomes active and visuals are re-applied.
extern volatile bool g_screens_need_apply[5];
// Timestamp (millis) when the config page was last opened. Used by the WS
// watchdog in loop() to auto-resume if the user closes the browser without saving.
extern unsigned long g_config_page_last_seen;

// Deferred LVGL action flags set by async HTTP handlers (Core 0) and consumed
// in loop() on Core 1 where LVGL operations are safe.
extern volatile int  g_pending_set_screen_idx;   // -1 = no pending
extern volatile bool g_pending_test_gauge;         // move needle for calibration
extern volatile int  g_pending_test_screen_idx;
extern volatile int  g_pending_test_gauge_idx;
extern volatile int  g_pending_test_angle;
extern volatile bool g_pending_apply_needles;      // apply_all_needle_styles()
extern volatile bool g_pending_auto_scroll_update; // set_auto_scroll_interval()
extern volatile uint16_t g_pending_auto_scroll_sec;

// Check if WiFi is connected
bool is_wifi_connected();

// Own ship MMSI for AIS self-filtering
extern String saved_own_mmsi;

// Get configured Signal K server IP (empty string if not configured)
String get_signalk_server_ip();

// Get configured Signal K port
uint16_t get_signalk_server_port();

// Get/set configured Signal K path by index (0-9)
String get_signalk_path_by_index(int index);
void set_signalk_path_by_index(int index, const String& path);

// Get Signal K paths needed by a single screen (0-based index)
std::vector<String> get_signalk_paths_for_screen(int screen_idx);

// Get all configured Signal K paths (gauges, number displays, dual displays) - unique only
std::vector<String> get_all_signalk_paths();

// Load persisted preferences and screen configs (from NVS or SD fallback)
void load_preferences();

// Dump loaded `screen_configs` to the log for debugging
void dump_screen_configs();

// Backward compatibility helpers
inline String get_signalk_path1() { return get_signalk_path_by_index(0); }
inline String get_signalk_path2() { return get_signalk_path_by_index(1); }

#endif // SENSEXP_SETUP_H
