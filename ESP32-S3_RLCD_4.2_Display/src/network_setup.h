#ifndef NETWORK_SETUP_H
#define NETWORK_SETUP_H

#include <Arduino.h>
#include <WebServer.h>

// Global synchronous web server instance
extern WebServer config_server;

// Initialize network (WiFi + WebServer) with web UI for configuration
void setup_network();

// Load persisted preferences from NVS
void load_preferences();

// Save preferences to NVS
void save_preferences();

// Check if WiFi is connected
bool is_wifi_connected();

// Get configured Signal K server IP
String get_signalk_server_ip();

// Get configured Signal K port
uint16_t get_signalk_server_port();

// Auto-scroll interval in seconds (0 = off)
extern uint16_t auto_scroll_sec;

// Timestamp of last screen-config web request (0 = none)
extern volatile unsigned long last_config_activity;
#define CONFIG_MODE_TIMEOUT_MS 15000

#endif // NETWORK_SETUP_H
