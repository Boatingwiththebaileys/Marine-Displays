/*
 * SignalK WebSocket client for ESP32-S3-RLCD-4.2 Marine Display.
 *
 * Connects to a SignalK server over WebSocket (/signalk/v1/stream),
 * subscribes to paths configured in screen_configs[], parses delta
 * messages, and stores values for display by screen_render.
 *
 * Ported from the Square Display — simplified:
 *   - No gauge-index array (uses path-based lookup only)
 *   - No WS pause/resume (RLCD has more iRAM headroom)
 *   - No needle-to-angle conversion
 */

#ifndef SIGNALK_CONFIG_H
#define SIGNALK_CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Navigation globals (for COMPASS / POSITION display types)
extern volatile float g_nav_latitude;
extern volatile float g_nav_longitude;
extern char g_sk_datetime[32];
extern SemaphoreHandle_t sensor_mutex;

// Get a stored value by its SignalK path, or NAN if not received yet
float get_sensor_value_by_path(const String& path);

// Get SignalK metadata (units, description) for a path
String get_sensor_unit_by_path(const String& path);
String get_sensor_description_by_path(const String& path);

// Collect all SignalK paths needed by a single screen (0-based index)
std::vector<String> get_signalk_paths_for_screen(int screen_idx);

// Collect all SignalK paths across all screens (unique)
std::vector<String> get_all_signalk_paths();

// Start the SignalK WebSocket client (call after WiFi is connected)
void enable_signalk(const char* server_ip, uint16_t server_port);

// Stop the SignalK WebSocket client
void disable_signalk();

// Rebuild and resend subscriptions after config change
void refresh_signalk_subscriptions();

// Subscribe only to the active screen's paths (+ background graph screens)
void subscribe_to_active_screen(int screen_0based);

#endif // SIGNALK_CONFIG_H
