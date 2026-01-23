
#ifndef SENSEXP_SETUP_H
#define SENSEXP_SETUP_H

#include <Arduino.h>
#include <WebServer.h>
#include "signalk_config.h"  // For NUM_SCREENS, TOTAL_PARAMS

#include "calibration_types.h"
extern GaugeCalibrationPoint gauge_cal[NUM_SCREENS][2][5];

// Global web server instance
extern WebServer config_server;

// Auto-scroll interval in seconds (0 = off)
extern uint16_t auto_scroll_sec;

// Request the UI to change auto-scroll interval at runtime
void set_auto_scroll_interval(uint16_t sec);

// Initialize SensESP with web UI for configuration
void setup_sensESP();

// Check if WiFi is connected via SensESP
bool is_wifi_connected();

// Get configured Signal K server IP (empty string if not configured)
String get_signalk_server_ip();

// Get configured Signal K port
uint16_t get_signalk_server_port();

// Get/set configured Signal K path by index (0-9)
String get_signalk_path_by_index(int index);
void set_signalk_path_by_index(int index, const String& path);

// Load persisted preferences and screen configs (from NVS or SD fallback)
void load_preferences();

// Dump loaded `screen_configs` to the log for debugging
void dump_screen_configs();

// Backward compatibility helpers
inline String get_signalk_path1() { return get_signalk_path_by_index(0); }
inline String get_signalk_path2() { return get_signalk_path_by_index(1); }

#endif // SENSEXP_SETUP_H
