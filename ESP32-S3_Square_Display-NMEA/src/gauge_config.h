#pragma once
#include <stdint.h>
#include <Preferences.h>

#define CALIBRATION_POINTS 5

// Parameter types that can be displayed
enum ParamType {
    PARAM_RPM = 0,
    PARAM_COOLANT_TEMP = 1,
    PARAM_FUEL = 2,
    PARAM_EXHAUST_TEMP = 3,
    PARAM_OIL_PRESSURE = 4,
    PARAM_TYPE_COUNT = 5
};

// Single parameter calibration curve
struct ParamCalibration {

    float values[CALIBRATION_POINTS];      // 5 value points
    int16_t angles[CALIBRATION_POINTS];    // 5 corresponding angles
};

// Gauge configuration structure with curves for each parameter type and position
struct GaugeConfig {
    // ...existing code...
    // Calibration curves for each parameter type and position [param_type][position]
    // position: 0 = top gauge, 1 = bottom gauge
    ParamCalibration calibrations[PARAM_TYPE_COUNT][2];
    
    // Legacy fields for backward compatibility (deprecated - use calibrations[PARAM_RPM] and calibrations[PARAM_COOLANT_TEMP])
    float top_values[CALIBRATION_POINTS];      
    int16_t top_angles[CALIBRATION_POINTS];    
    float bottom_values[CALIBRATION_POINTS];   
    int16_t bottom_angles[CALIBRATION_POINTS]; 
    
    float top_min;
    float top_max;
    int16_t top_angle_min;
    int16_t top_angle_max;
    float bottom_min;
    float bottom_max;
    int16_t bottom_angle_min;
    int16_t bottom_angle_max;
};
// Global gauge configuration instance
extern GaugeConfig current_config;

// Initialize gauge configuration with defaults
void gauge_config_init();

// Load gauge configuration from flash
void gauge_config_load(GaugeConfig &config);

// Save gauge configuration to flash
void gauge_config_save(const GaugeConfig &config);

// Get current configuration
GaugeConfig& gauge_config_get();

// Convert value to angle using calibration for specific parameter type and position
// param_type: 0=RPM, 1=Coolant, 2=Fuel, 3=Exhaust, 4=Oil
// position: 0=top gauge, 1=bottom gauge
int16_t gauge_value_to_angle(float value, int param_type, int position);

// Backward compatibility - use RPM and Coolant curves
int16_t gauge_top_value_to_angle(float value);
int16_t gauge_bottom_value_to_angle(float value);

// Setup mode functions for live needle preview
bool gauge_is_setup_mode();
void gauge_set_setup_mode(bool enabled);
void gauge_set_preview_angles(int16_t top_angle, int16_t bottom_angle);
void gauge_set_preview_top_angle(int16_t angle);
void gauge_set_preview_bottom_angle(int16_t angle);
int16_t gauge_get_preview_top_angle();
int16_t gauge_get_preview_bottom_angle();

// New function for per-screen, per-gauge calibration
int16_t gauge_value_to_angle_screen(float value, int screen, int gauge);
