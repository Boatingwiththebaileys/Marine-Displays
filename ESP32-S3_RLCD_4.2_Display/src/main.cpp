/*
 * ESP32-S3-RLCD-4.2 Marine Display — Main entry point
 *
 * Waveshare ESP32-S3-RLCD-4.2 board:
 *   - 400×300 reflective LCD (ST7305, SPI, monochrome)
 *   - No touch — button-based navigation (BOOT + KEY)
 *   - PCF85063 RTC on I2C
 *   - SHTC3 temperature/humidity sensor on I2C
 *   - SD card via SDMMC (1-bit)
 *   - 18650 battery
 *
 * This is the initial skeleton — boots the display, LVGL, buttons,
 * and shows a "Hello Marine Display" label.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include "board_pins.h"
#include "LVGL_Driver.h"
#include "Button_Input.h"
#include "network_setup.h"
#include "screen_config.h"
#include "screen_render.h"
#include "signalk_config.h"
#include "audio_alert.h"

static const char *TAG = "main";

// ─── Setup ──────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    ESP_LOGI(TAG, "ESP32-S3-RLCD-4.2 Marine Display starting...");

    // Network: load prefs, connect WiFi or fall back to AP, start web server
    setup_network();

    // Initialise LVGL + ST7305 display
    LVGL_Init();

    // Initialise button input
    if (LVGL_Lock()) {
        Button_Init();
        LVGL_Unlock();
    }

    // Render the first screen based on saved config
    render_screen(get_current_screen());

    // Initialise audio alert system (ES8311 codec + I2S speaker)
    audio_alert_init();

    // Start SignalK WebSocket if WiFi is connected and server is configured
    if (is_wifi_connected()) {
        String sk_ip = get_signalk_server_ip();
        uint16_t sk_port = get_signalk_server_port();
        if (sk_ip.length() > 0 && sk_port > 0) {
            enable_signalk(sk_ip.c_str(), sk_port);
        }
    }

    ESP_LOGI(TAG, "Setup complete");
}

// ─── Loop ───────────────────────────────────────────────────────────────────

void loop() {
    // Handle web server requests
    config_server.handleClient();

    // Update display with latest SignalK values (~1Hz)
    static unsigned long last_update = 0;
    if (millis() - last_update >= 1000) {
        last_update = millis();
        update_screen_values();
    }

    // Auto-scroll screens (paused while configuring via web UI)
    if (auto_scroll_sec > 0) {
        bool in_config = last_config_activity > 0 &&
                         (millis() - last_config_activity) < CONFIG_MODE_TIMEOUT_MS;
        if (!in_config) {
            static unsigned long last_scroll = 0;
            if (millis() - last_scroll >= (unsigned long)auto_scroll_sec * 1000) {
                last_scroll = millis();
                int next = (get_current_screen() + 1) % NUM_SCREENS;
                set_current_screen(next);
                refresh_signalk_subscriptions();  // Update WebSocket subscriptions for new screen
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}
