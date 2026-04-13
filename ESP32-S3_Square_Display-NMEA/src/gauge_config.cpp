#include <cstdint>
#include "network_setup.h"
#include <cmath>
#include <cstdlib>

// Get angle for a value using per-screen, per-gauge calibration
int16_t gauge_value_to_angle_screen(float value, int screen, int gauge) {
    if (screen < 0 || screen >= NUM_SCREENS) return 0;
    if (gauge < 0 || gauge > 1) return 0;
    GaugeCalibrationPoint* cal = gauge_cal[screen][gauge];
    // Find which segment the value falls into
    for (int i = 0; i < 4; i++) {
        float val1 = cal[i].value;
        float val2 = cal[i + 1].value;
        int16_t angle1 = cal[i].angle;
        int16_t angle2 = cal[i + 1].angle;
        if ((val1 <= val2 && value >= val1 && value <= val2) ||
            (val1 > val2 && value >= val2 && value <= val1)) {
            float value_range = val2 - val1;
            if (fabs(value_range) < 0.001) return angle1;
            float normalized = (value - val1) / value_range;
            int16_t result;
            if (angle2 < angle1) {
                result = angle1 - (int16_t)(normalized * abs(angle2 - angle1));
            } else {
                result = angle1 + (int16_t)(normalized * abs(angle2 - angle1));
            }
            return result;
        }
    }
    if (value < cal[0].value) return cal[0].angle;
    return cal[4].angle;
}
// Use the Preferences object from network_setup.cpp for all calibration storage
#include "gauge_config.h"
#include <Arduino.h>
#include "network_setup.h"
#include "signalk_config.h"
extern Preferences preferences;
GaugeConfig current_config;
static bool setup_mode = false;
static int16_t preview_top_angle = 0;
static int16_t preview_bottom_angle = 0;

void gauge_config_init() {
    static bool calibration_initialized = false;
    if (calibration_initialized) return;
    calibration_initialized = true;
    // Initialize all 10 calibration curves (5 param types × 2 positions)
    // Position 0 = top gauge, Position 1 = bottom gauge
    // IMPORTANT: Ensure we start in normal mode, not setup mode
    setup_mode = false;
    Serial.println("Gauge config init: setup_mode = false (normal Signal K mode)");
    // RPM Top (0-60 Hz) - Used on screens 1, 2  
    // IMPORTANT: 16.67 Hz should map to ~90° (quarter scale)
    current_config.calibrations[PARAM_RPM][0].values[0] = 0.0f;     // 0 Hz
    current_config.calibrations[PARAM_RPM][0].values[1] = 15.0f;    // 15 Hz  
    current_config.calibrations[PARAM_RPM][0].values[2] = 30.0f;    // 30 Hz
    current_config.calibrations[PARAM_RPM][0].values[3] = 45.0f;    // 45 Hz
    current_config.calibrations[PARAM_RPM][0].values[4] = 60.0f;    // 60 Hz
    current_config.calibrations[PARAM_RPM][0].angles[0] = 0;        // 0°
    current_config.calibrations[PARAM_RPM][0].angles[1] = 90;       // 90°  
    current_config.calibrations[PARAM_RPM][0].angles[2] = 180;      // 180°
    current_config.calibrations[PARAM_RPM][0].angles[3] = 270;      // 270°
    current_config.calibrations[PARAM_RPM][0].angles[4] = 360;      // 360°
    Serial.println("Initialized RPM calibration: 0-60 Hz -> 0-360°");
    
    // RPM Bottom (unused but initialized)
    current_config.calibrations[PARAM_RPM][1] = current_config.calibrations[PARAM_RPM][0];
    
    // Coolant Temp Top (40-120°C in Kelvin) - Used on screens 3, 4, 5
    current_config.calibrations[PARAM_COOLANT_TEMP][0].values[0] = 313.15f;  // 40°C
    current_config.calibrations[PARAM_COOLANT_TEMP][0].values[1] = 333.15f;  // 60°C
    current_config.calibrations[PARAM_COOLANT_TEMP][0].values[2] = 353.15f;  // 80°C
    current_config.calibrations[PARAM_COOLANT_TEMP][0].values[3] = 373.15f;  // 100°C
    current_config.calibrations[PARAM_COOLANT_TEMP][0].values[4] = 393.15f;  // 120°C
    current_config.calibrations[PARAM_COOLANT_TEMP][0].angles[0] = 0;
    current_config.calibrations[PARAM_COOLANT_TEMP][0].angles[1] = 90;
    current_config.calibrations[PARAM_COOLANT_TEMP][0].angles[2] = 180;
    current_config.calibrations[PARAM_COOLANT_TEMP][0].angles[3] = 270;
    current_config.calibrations[PARAM_COOLANT_TEMP][0].angles[4] = 360;
    
    // Coolant Temp Bottom (40-120°C in Kelvin) - Used on screen 1
    current_config.calibrations[PARAM_COOLANT_TEMP][1].values[0] = 313.15f;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].values[1] = 333.15f;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].values[2] = 353.15f;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].values[3] = 373.15f;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].values[4] = 393.15f;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[0] = 0;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[1] = 90;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[2] = 180;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[3] = 270;
    current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[4] = 360;
    
    // Fuel Bottom (0-100%) - Used on screen 2
    current_config.calibrations[PARAM_FUEL][1].values[0] = 0.0f;
    current_config.calibrations[PARAM_FUEL][1].values[1] = 25.0f;
    current_config.calibrations[PARAM_FUEL][1].values[2] = 50.0f;
    current_config.calibrations[PARAM_FUEL][1].values[3] = 75.0f;
    current_config.calibrations[PARAM_FUEL][1].values[4] = 100.0f;
    current_config.calibrations[PARAM_FUEL][1].angles[0] = 0;
    current_config.calibrations[PARAM_FUEL][1].angles[1] = 90;
    current_config.calibrations[PARAM_FUEL][1].angles[2] = 180;
    current_config.calibrations[PARAM_FUEL][1].angles[3] = 270;
    current_config.calibrations[PARAM_FUEL][1].angles[4] = 360;
    
    // Fuel Top (unused but initialized)
    current_config.calibrations[PARAM_FUEL][0] = current_config.calibrations[PARAM_FUEL][1];
    
    // Exhaust Temp Bottom (200-700°C in Kelvin) - Used on screens 3, 5
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].values[0] = 473.15f;  // 200°C
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].values[1] = 548.15f;  // 275°C
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].values[2] = 623.15f;  // 350°C
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].values[3] = 698.15f;  // 425°C
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].values[4] = 973.15f;  // 700°C
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].angles[0] = 0;
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].angles[1] = 90;
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].angles[2] = 180;
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].angles[3] = 270;
    current_config.calibrations[PARAM_EXHAUST_TEMP][1].angles[4] = 360;
    
    // Exhaust Temp Top (unused but initialized)
    current_config.calibrations[PARAM_EXHAUST_TEMP][0] = current_config.calibrations[PARAM_EXHAUST_TEMP][1];
    
    // Oil Pressure Bottom (0-6 bar in Pa) - Used on screen 4
    current_config.calibrations[PARAM_OIL_PRESSURE][1].values[0] = 0.0f;
    current_config.calibrations[PARAM_OIL_PRESSURE][1].values[1] = 150000.0f;   // 1.5 bar
    current_config.calibrations[PARAM_OIL_PRESSURE][1].values[2] = 300000.0f;   // 3 bar
    current_config.calibrations[PARAM_OIL_PRESSURE][1].values[3] = 450000.0f;   // 4.5 bar
    current_config.calibrations[PARAM_OIL_PRESSURE][1].values[4] = 600000.0f;   // 6 bar
    current_config.calibrations[PARAM_OIL_PRESSURE][1].angles[0] = 0;
    current_config.calibrations[PARAM_OIL_PRESSURE][1].angles[1] = 90;
    current_config.calibrations[PARAM_OIL_PRESSURE][1].angles[2] = 180;
    current_config.calibrations[PARAM_OIL_PRESSURE][1].angles[3] = 270;
    current_config.calibrations[PARAM_OIL_PRESSURE][1].angles[4] = 360;
    
    // Oil Pressure Top (unused but initialized)
    current_config.calibrations[PARAM_OIL_PRESSURE][0] = current_config.calibrations[PARAM_OIL_PRESSURE][1];
    
    // Legacy fields for backward compatibility (use RPM top and Coolant bottom)
    // Legacy fields for backward compatibility (use RPM top and Coolant bottom)
    for (int i = 0; i < CALIBRATION_POINTS; i++) {
        current_config.top_values[i] = current_config.calibrations[PARAM_RPM][0].values[i];
        current_config.top_angles[i] = current_config.calibrations[PARAM_RPM][0].angles[i];
        current_config.bottom_values[i] = current_config.calibrations[PARAM_COOLANT_TEMP][1].values[i];
        current_config.bottom_angles[i] = current_config.calibrations[PARAM_COOLANT_TEMP][1].angles[i];
    }
    
    current_config.top_min = 0.0f;
    current_config.top_max = 60.0f;
    current_config.top_angle_min = 0;
    current_config.top_angle_max = 360;
    current_config.bottom_min = 313.15f;
    current_config.bottom_max = 393.15f;
    current_config.bottom_angle_min = 0;
    current_config.bottom_angle_max = 360;
    
    // Try to load from userdata partition (persists across firmware updates)
    // Load persisted calibration from Preferences (safe open/close here)
    Serial.println("[gauge_config] Loading gauge calibration from storage...");
    if (preferences.begin("settings", true)) {
        const char* param_names[] = {"rpm", "coolant", "fuel", "exhaust", "oil"};
        const char* pos_names[] = {"top", "bot"};
        for (int p = 0; p < PARAM_TYPE_COUNT; p++) {
            for (int pos = 0; pos < 2; pos++) {
                for (int i = 0; i < CALIBRATION_POINTS; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "%s_%s_val_%d", param_names[p], pos_names[pos], i);
                    current_config.calibrations[p][pos].values[i] = preferences.getFloat(key, current_config.calibrations[p][pos].values[i]);
                    snprintf(key, sizeof(key), "%s_%s_ang_%d", param_names[p], pos_names[pos], i);
                    current_config.calibrations[p][pos].angles[i] = preferences.getShort(key, current_config.calibrations[p][pos].angles[i]);
                }
            }
        }
        preferences.end();
    } else {
        Serial.println("[gauge_config] Preferences not available; using defaults.");
    }
    for (int i = 0; i < CALIBRATION_POINTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "top_val_%d", i);
        current_config.top_values[i] = preferences.getFloat(key, current_config.top_values[i]);
        snprintf(key, sizeof(key), "top_ang_%d", i);
        current_config.top_angles[i] = preferences.getShort(key, current_config.top_angles[i]);
    }
    for (int i = 0; i < CALIBRATION_POINTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "bot_val_%d", i);
        current_config.bottom_values[i] = preferences.getFloat(key, current_config.bottom_values[i]);
        snprintf(key, sizeof(key), "bot_ang_%d", i);
        current_config.bottom_angles[i] = preferences.getShort(key, current_config.bottom_angles[i]);
    }
    Serial.println("[gauge_config] Gauge config loaded from flash (5-point calibration)");
}

void gauge_config_load(GaugeConfig &config) {
    config = current_config;
}

void gauge_config_save(const GaugeConfig &config) {
    current_config = config;
    // Preferences must already be open! Only write if open.
    Serial.println("[gauge_config] Saving gauge calibration to NVS...");
    const char* param_names[] = {"rpm", "coolant", "fuel", "exhaust", "oil"};
    const char* pos_names[] = {"top", "bot"};
    for (int p = 0; p < PARAM_TYPE_COUNT; p++) {
        for (int pos = 0; pos < 2; pos++) {
            for (int i = 0; i < CALIBRATION_POINTS; i++) {
                char key[32];
                snprintf(key, sizeof(key), "%s_%s_val_%d", param_names[p], pos_names[pos], i);
                preferences.putFloat(key, config.calibrations[p][pos].values[i]);
                snprintf(key, sizeof(key), "%s_%s_ang_%d", param_names[p], pos_names[pos], i);
                preferences.putShort(key, config.calibrations[p][pos].angles[i]);
            }
        }
    }
    for (int i = 0; i < CALIBRATION_POINTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "top_val_%d", i);
        preferences.putFloat(key, config.top_values[i]);
        snprintf(key, sizeof(key), "top_ang_%d", i);
        preferences.putShort(key, config.top_angles[i]);
    }
    for (int i = 0; i < CALIBRATION_POINTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "bot_val_%d", i);
        preferences.putFloat(key, config.bottom_values[i]);
        snprintf(key, sizeof(key), "bot_ang_%d", i);
        preferences.putShort(key, config.bottom_angles[i]);
    }
    Serial.println("[gauge_config] Gauge config saved to flash (5-point calibration)");
    // After saving calibration changes, refresh Signal K subscriptions so
    // any path changes or re-applies take effect immediately.
    refresh_signalk_subscriptions();
}

int16_t gauge_top_value_to_angle(float value) {
    // Clamp to min/max calibration points
    if (value <= current_config.top_values[0]) return current_config.top_angles[0];
    if (value >= current_config.top_values[CALIBRATION_POINTS-1]) return current_config.top_angles[CALIBRATION_POINTS-1];
    
    // Find which segment the value falls into
    for (int i = 0; i < CALIBRATION_POINTS - 1; i++) {
        if (value >= current_config.top_values[i] && value <= current_config.top_values[i+1]) {
            // Piecewise linear interpolation between points i and i+1
            float value_range = current_config.top_values[i+1] - current_config.top_values[i];
            if (value_range <= 0) return current_config.top_angles[i];
            
            float normalized = (value - current_config.top_values[i]) / value_range;
            int16_t angle_range = current_config.top_angles[i+1] - current_config.top_angles[i];
            
            return current_config.top_angles[i] + (int16_t)(normalized * angle_range);
        }
    }
    
    return current_config.top_angles[0];  // Fallback
}

int16_t gauge_bottom_value_to_angle(float value) {
    // Clamp to min/max calibration points
    if (value <= current_config.bottom_values[0]) return current_config.bottom_angles[0];
    if (value >= current_config.bottom_values[CALIBRATION_POINTS-1]) return current_config.bottom_angles[CALIBRATION_POINTS-1];
    
    // Find which segment the value falls into
    for (int i = 0; i < CALIBRATION_POINTS - 1; i++) {
        if (value >= current_config.bottom_values[i] && value <= current_config.bottom_values[i+1]) {
            // Piecewise linear interpolation between points i and i+1
            float value_range = current_config.bottom_values[i+1] - current_config.bottom_values[i];
            if (value_range <= 0) return current_config.bottom_angles[i];
            
            float normalized = (value - current_config.bottom_values[i]) / value_range;
            int16_t angle_range = current_config.bottom_angles[i+1] - current_config.bottom_angles[i];
            
            return current_config.bottom_angles[i] + (int16_t)(normalized * angle_range);
        }
    }
    
    return current_config.bottom_angles[0];  // Fallback
}

// Get angle for a value using parameter type and position
// param_type: 0=RPM, 1=Coolant, 2=Fuel, 3=Exhaust, 4=Oil
// position: 0=top gauge, 1=bottom gauge
int16_t gauge_value_to_angle(float value, int param_type, int position) {
    // Bounds check
    if (param_type < 0 || param_type >= PARAM_TYPE_COUNT) return 0;
    if (position < 0 || position > 1) return 0;

    ParamCalibration* cal = &current_config.calibrations[param_type][position];

    // Find which segment the value falls into
    for (int i = 0; i < CALIBRATION_POINTS - 1; i++) {
        float val1 = cal->values[i];
        float val2 = cal->values[i + 1];
        int16_t angle1 = cal->angles[i];
        int16_t angle2 = cal->angles[i + 1];

        // Check if value is within this segment
        if ((val1 <= val2 && value >= val1 && value <= val2) ||
            (val1 > val2 && value >= val2 && value <= val1)) {
            // Linear interpolation between the two points
            float value_range = val2 - val1;
            if (fabs(value_range) < 0.001) return angle1;  // Avoid division by zero

            float normalized = (value - val1) / value_range;
            int16_t result;
            if (angle2 < angle1) {
                // Counterclockwise gauge: angles decrease as value increases
                result = angle1 - (int16_t)(normalized * abs(angle2 - angle1));
            } else {
                // Clockwise gauge: angles increase as value increases
                result = angle1 + (int16_t)(normalized * abs(angle2 - angle1));
            }
            return result;
        }
    }

    // If value is outside range, return nearest endpoint
    if (value < cal->values[0]) {
        return cal->angles[0];
    }
    return cal->angles[CALIBRATION_POINTS - 1];
}

bool gauge_is_setup_mode() {
    return setup_mode;
}

void gauge_set_setup_mode(bool enabled) {
    setup_mode = enabled;
    if (enabled) {
        // Initialize preview angles to min angles when entering setup mode
        preview_top_angle = current_config.top_angle_min;
        preview_bottom_angle = current_config.bottom_angle_min;
    }
}

void gauge_set_preview_angles(int16_t top_angle, int16_t bottom_angle) {
    preview_top_angle = top_angle;
    preview_bottom_angle = bottom_angle;
}

void gauge_set_preview_top_angle(int16_t angle) {
    preview_top_angle = angle;
}

void gauge_set_preview_bottom_angle(int16_t angle) {
    preview_bottom_angle = angle;
}

int16_t gauge_get_preview_top_angle() {
    return preview_top_angle;
}

int16_t gauge_get_preview_bottom_angle() {
    return preview_bottom_angle;
}
