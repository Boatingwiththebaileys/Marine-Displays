#ifndef SIGNALK_CONFIG_H
#define SIGNALK_CONFIG_H

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// Number of screens and parameters
#define NUM_SCREENS 5
#define PARAMS_PER_SCREEN 2
#define TOTAL_PARAMS (NUM_SCREENS * PARAMS_PER_SCREEN)  // 10 total

// Global array to hold all sensor values (10 parameters)
extern float g_sensor_values[TOTAL_PARAMS];
extern SemaphoreHandle_t sensor_mutex;

// Parameter indices for each screen
enum ParamIndex {
    // Screen 1: RPM + Coolant Temp
    SCREEN1_RPM = 0,
    SCREEN1_COOLANT_TEMP = 1,
    
    // Screen 2: RPM + Fuel
    SCREEN2_RPM = 2,
    SCREEN2_FUEL = 3,
    
    // Screen 3: Coolant Temp + Exhaust Temp
    SCREEN3_COOLANT_TEMP = 4,
    SCREEN3_EXHAUST_TEMP = 5,
    
    // Screen 4: Fuel + Coolant Temp
    SCREEN4_FUEL = 6,
    SCREEN4_COOLANT_TEMP = 7,
    
    // Screen 5: Oil Pressure + Coolant Temp
    SCREEN5_OIL_PRESSURE = 8,
    SCREEN5_COOLANT_TEMP = 9
};

// Sensor value getters/setters (thread-safe)
float get_sensor_value(int index);
void set_sensor_value(int index, float value);

// Backward compatibility helpers
inline float get_frequency_hz() { return get_sensor_value(SCREEN1_RPM); }
inline float get_temperature_k() { return get_sensor_value(SCREEN1_COOLANT_TEMP); }
inline void set_frequency_hz(float hz) { set_sensor_value(SCREEN1_RPM, hz); }
inline void set_temperature_k(float temp) { set_sensor_value(SCREEN1_COOLANT_TEMP, temp); }

// Mutex initialization
void init_sensor_mutex();

// Signal K control functions
void enable_signalk(const char* ssid, const char* password, const char* server_ip, uint16_t server_port);
void disable_signalk();
// Rebuild and (re)send Signal K subscription list from current configuration
void refresh_signalk_subscriptions();

// Enqueue an outgoing message to be sent when WS is connected
void enqueue_signalk_message(const String &msg);
// Convert value to angle based on parameter type and position
int16_t value_to_angle_for_param(float value, int param_type, int position);

// Update needle display from Signal K values for specific screen
#ifdef __cplusplus
extern "C" {
#endif
void update_needles_for_screen(int screen_num);
#ifdef __cplusplus
}
#endif

#endif // SIGNALK_CONFIG_H

