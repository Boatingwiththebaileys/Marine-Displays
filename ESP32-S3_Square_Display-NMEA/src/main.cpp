#include <Preferences.h>
#include <esp_err.h>
#include <WiFi.h>
// Global test mode flag: disables live data updates when true
bool test_mode = false;
#include <Arduino.h>
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "Touch_GT911.h"
#include "LVGL_Driver.h"
#include "SD_Card.h"
#include "ui.h"
#include "ui_Settings.h"
#include "signalk_config.h"
#include "nmea2000_config.h"
#include "screen_config_c_api.h"
#include "network_setup.h"
#include "gauge_config.h"
#include "needle_style.h"
#include "number_display.h"
#include "dual_number_display.h"
#include "quad_number_display.h"
#include "gauge_number_display.h"
#include "graph_display.h"
#include "position_display.h"
#include "compass_display.h"
#include "ais_display.h"
#include "attitude_display.h"
#include "anchor_display.h"
#include "Gyro_QMI8658.h"
#include "unit_convert.h"
#include "version.h"
#include "RTC_PCF85063.h"
#ifdef __cplusplus
extern "C" {
#endif
void show_fallback_error_screen_if_needed();
#ifdef __cplusplus
}
#endif

// Apply visuals at runtime (hot-update) helper from sensESP_setup
bool apply_all_screen_visuals();
bool apply_screen_visuals_for_one(int s);
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rgb565_decoder.h"  // Custom decoder for binary RGB565 images
#include "driver/spi_master.h"

// External UI elements (per-screen icons are declared in ui_ScreenN.h via ui.h)

// Screen-off power saving state
bool     g_screen_is_off      = false;
uint32_t g_last_activity_ms   = 0;   // updated on touch; also used by LVGL_Driver

// Animation state tracking
static int16_t current_needle_angle = 0;
static int16_t current_lower_needle_angle = 0;

// Global needle angle tracking for all screens (1-based: Screen1..Screen5)
int16_t last_top_angle[6] = {0, 0, 0, 0, 0, 0};     // [screen] - all start at 0°
int16_t last_bottom_angle[6] = {0, 180, 180, 180, 180, 180};  // [screen] - all start at 180°

// Buzzer alert function is implemented in `src/ui_Settings.cpp`.
// The stub was removed to avoid duplicate definitions.

// Buzzer runtime state (moved to file-scope so settings can signal immediate re-eval)
unsigned long last_buzzer_time = 0;
bool first_run_buzzer = true;
static unsigned long last_alarm_log_time = 0;  // rate-limit alarm serial output
#define ALARM_LOG_INTERVAL_MS 5000

// Animation callback for upper needle
static void needle_anim_cb(void * var, int32_t v) {
    lv_obj_t* needle = (lv_obj_t*)var;
    if (needle != NULL) {
        // Determine which screen/gauge this needle object corresponds to
        int screen = 0;
        int gauge = 0; // 0 = top
        if (needle == ui_Needle) { screen = 0; gauge = 0; }
        else if (needle == ui_Needle2) { screen = 1; gauge = 0; }
        else if (needle == ui_Needle3) { screen = 2; gauge = 0; }
        else if (needle == ui_Needle4) { screen = 3; gauge = 0; }
        else if (needle == ui_Needle5) { screen = 4; gauge = 0; }

        NeedleStyle s = get_needle_style(screen, gauge);
        float rad = (v - 90) * PI / 180.0f;
        static lv_point_t points[2];
        points[0].x = s.cx + (int16_t)(s.inner * cos(rad));
        points[0].y = s.cy + (int16_t)(s.inner * sin(rad));
        points[1].x = s.cx + (int16_t)(s.outer * cos(rad));
        points[1].y = s.cy + (int16_t)(s.outer * sin(rad));
        lv_line_set_points(needle, points, 2);
    }
}

// Animation callback for lower needle
static void lower_needle_anim_cb(void * var, int32_t v) {
    lv_obj_t* needle = (lv_obj_t*)var;
    if (needle != NULL) {
        int screen = 0;
        int gauge = 1; // bottom
        if (needle == ui_Lower_Needle) { screen = 0; gauge = 1; }
        else if (needle == ui_Lower_Needle2) { screen = 1; gauge = 1; }
        else if (needle == ui_Lower_Needle3) { screen = 2; gauge = 1; }
        else if (needle == ui_Lower_Needle4) { screen = 3; gauge = 1; }
        else if (needle == ui_Lower_Needle5) { screen = 4; gauge = 1; }

        NeedleStyle s = get_needle_style(screen, gauge);
        float rad = (v - 90) * PI / 180.0f;
        static lv_point_t points[2];
        points[0].x = s.cx + (int16_t)(s.inner * cos(rad));
        points[0].y = s.cy + (int16_t)(s.inner * sin(rad));
        points[1].x = s.cx + (int16_t)(s.outer * cos(rad));
        points[1].y = s.cy + (int16_t)(s.outer * sin(rad));
        lv_line_set_points(needle, points, 2);
    }
}

// Auto-scroll timer handle (null when disabled)
static lv_timer_t *auto_scroll_timer = NULL;

// Set auto-scroll interval (seconds). 0 disables auto-scroll.
void set_auto_scroll_interval(uint16_t sec) {
    // Remove existing timer if present
    if (auto_scroll_timer) {
        lv_timer_del(auto_scroll_timer);
        auto_scroll_timer = NULL;
    }
    if (sec > 0) {
        // Create a timer that only advances screens when Settings is NOT active
        auto_scroll_timer = lv_timer_create([](lv_timer_t *t){ (void)t;
            // Do not auto-advance when the Settings screen is open
            if (lv_scr_act() == ui_Settings) return;
            ui_next_screen();
        }, (uint32_t)sec * 1000, NULL);
    }
}

// Smooth animated needle updates - now fast with line-based rendering!
void rotate_needle(int16_t angle) {
    if (ui_Needle != NULL && angle != current_needle_angle) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_Needle);
        lv_anim_set_exec_cb(&a, needle_anim_cb);
        lv_anim_set_values(&a, current_needle_angle, angle);
        lv_anim_set_time(&a, 500);  // 500ms smooth animation
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        current_needle_angle = angle;
    }
}

void rotate_lower_needle(int16_t angle) {
    if (ui_Lower_Needle != NULL && angle != current_lower_needle_angle) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ui_Lower_Needle);
        lv_anim_set_exec_cb(&a, lower_needle_anim_cb);
        lv_anim_set_values(&a, current_lower_needle_angle, angle);
        lv_anim_set_time(&a, 500);  // 500ms smooth animation
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        current_lower_needle_angle = angle;
    }
}

// Legacy unit-to-angle helpers removed; mapping now uses
// `gauge_value_to_angle_screen()` via `value_to_angle_for_param()`.

// Unified conversion function for all parameter types
// param_type: 0=RPM, 1=Coolant Temp, 2=Fuel, 3=Exhaust Temp, 4=Oil Pressure
// New version: per-screen, per-gauge calibration
int16_t value_to_angle_for_param(float value, int screen, int gauge) {
    int16_t angle = gauge_value_to_angle_screen(value, screen, gauge);
    return angle;
}

// Generic needle animation helper that caches the last angle per needle
static void animate_generic_needle(lv_obj_t* needle, int16_t &last_angle, int16_t new_angle, bool is_lower) {
    if (needle == NULL) {
        return;
    }
    if (new_angle == last_angle) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, needle);
    lv_anim_set_exec_cb(&a, is_lower ? lower_needle_anim_cb : needle_anim_cb);
    lv_anim_set_values(&a, last_angle, new_angle);
    lv_anim_set_time(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);

    

    last_angle = new_angle;
}

// Initialize all needle positions to defaults: top needles at 0°, bottom needles at 180°
void initialize_needle_positions() {
    // Set all top needles to 0 degrees (pointing up)
    // Initialize line-based needles by calling the same callbacks used by animations
    if (ui_Needle) needle_anim_cb(ui_Needle, 0);
    if (ui_Needle2) needle_anim_cb(ui_Needle2, 0);
    if (ui_Needle3) needle_anim_cb(ui_Needle3, 0);
    if (ui_Needle4) needle_anim_cb(ui_Needle4, 0);
    if (ui_Needle5) needle_anim_cb(ui_Needle5, 0);

    // Set all bottom needles to 180 degrees (pointing down)
    if (ui_Lower_Needle) lower_needle_anim_cb(ui_Lower_Needle, 180);
    if (ui_Lower_Needle2) lower_needle_anim_cb(ui_Lower_Needle2, 180);
    if (ui_Lower_Needle3) lower_needle_anim_cb(ui_Lower_Needle3, 180);
    if (ui_Lower_Needle4) lower_needle_anim_cb(ui_Lower_Needle4, 180);
    if (ui_Lower_Needle5) lower_needle_anim_cb(ui_Lower_Needle5, 180);
}

// File-level tracking for number displays (moved out of function scope for external reset)
static bool number_displays_created[5] = {false, false, false, false, false};
static float last_display_values[5] = {NAN, NAN, NAN, NAN, NAN};
static String last_display_units[5] = {"", "", "", "", ""};
static String last_display_descriptions[5] = {"", "", "", "", ""};

// Reset tracking for number display (called when display is externally recreated)
extern "C" void reset_number_display_tracking(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    int screen_idx = screen_num - 1;
    
    // Reset tracking for this screen to force update on next cycle
    number_displays_created[screen_idx] = true;  // Mark as created externally
    last_display_values[screen_idx] = NAN;
    last_display_units[screen_idx] = "";
    last_display_descriptions[screen_idx] = "";
}

// Force immediate update of number display (bypasses change detection)
extern "C" void force_update_number_display(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    int screen_idx = screen_num - 1;
    
    // Get the configured Signal K path
    String number_path = String(screen_configs[screen_idx].number_path);
    if (number_path.length() == 0) {
        number_display_update(screen_idx, 0.0f, "No Path", "");
        return;
    }
    
    // Get current sensor data
    float display_value = get_sensor_value_by_path(number_path);
    String unit_str = get_sensor_unit_by_path(number_path);
    String description = get_sensor_description_by_path(number_path);
    
    // Convert SI units to display units based on unit system preference
    display_value = convert_unit(display_value, unit_str, number_path, unit_str);
    
    // Force update regardless of change detection
    if (!isnan(display_value)) {
        number_display_update(screen_idx, display_value, unit_str.c_str(), description.c_str());
    } else {
        number_display_update(screen_idx, 0.0f, unit_str.c_str(), description.c_str());
    }
    
    // Update tracking with current values
    last_display_values[screen_idx] = display_value;
    last_display_units[screen_idx] = unit_str;
    last_display_descriptions[screen_idx] = description;
}

// Update number display for the active screen using live Signal K sensor values
static void update_number_display_for_screen(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    
    int screen_idx = screen_num - 1;  // Convert to 0-based index
    
    // Get the configured Signal K path for this screen's number display
    String number_path = String(screen_configs[screen_idx].number_path);
    if (number_path.length() == 0) {
        // No path configured, show placeholder
        if (!number_displays_created[screen_idx]) {
            number_display_create(screen_idx);
            number_displays_created[screen_idx] = true;
        }
        number_display_update(screen_idx, 0.0f, "No Path", "");
        return;
    }
    
    // Find the sensor index for this path
    int sensor_idx = -1;
    for (int i = 0; i < NUM_SCREENS * 2; i++) {
        if (get_signalk_path_by_index(i) == number_path) {
            sensor_idx = i;
            break;
        }
    }
    
    float display_value = 0.0f;
    String unit_str = "";
    String description = "";
    
    if (sensor_idx >= 0) {
        // Use the sensor value from the matching path
        display_value = get_sensor_value(sensor_idx);
        
        // Get metadata from SignalK (units and description)
        unit_str = get_sensor_unit(sensor_idx);
        description = get_sensor_description(sensor_idx);
        
        // Convert SI units to display units based on unit system preference
        display_value = convert_unit(display_value, unit_str, number_path, unit_str);
    } else {
        // Path is not a gauge path — look it up via extended sensor maps
        display_value = get_sensor_value_by_path(number_path);
        unit_str = get_sensor_unit_by_path(number_path);
        description = get_sensor_description_by_path(number_path);
        display_value = convert_unit(display_value, unit_str, number_path, unit_str);
    }
    
    // Create number display if it doesn't exist (note: may also be created externally via ui_hotupdate)
    if (!number_displays_created[screen_idx]) {
        number_display_create(screen_idx);
        number_displays_created[screen_idx] = true;
        // Reset tracking to force immediate update
        last_display_values[screen_idx] = NAN;
        last_display_units[screen_idx] = "";
        last_display_descriptions[screen_idx] = "";
    }
    
    // Only update display if value, unit, or description changed
    bool value_changed = (isnan(display_value) != isnan(last_display_values[screen_idx])) ||
                         (!isnan(display_value) && fabs(display_value - last_display_values[screen_idx]) > 0.001f);
    bool unit_changed = (unit_str != last_display_units[screen_idx]);
    bool desc_changed = (description != last_display_descriptions[screen_idx]);
    
    if (value_changed || unit_changed || desc_changed) {
        // Update the display with separate unit and description
        if (!isnan(display_value)) {
            number_display_update(screen_idx, display_value, unit_str.c_str(), description.c_str());
        } else {
            number_display_update(screen_idx, 0.0f, unit_str.c_str(), description.c_str());
        }
        
        // Save current state
        last_display_values[screen_idx] = display_value;
        last_display_units[screen_idx] = unit_str;
        last_display_descriptions[screen_idx] = description;
        last_display_values[screen_idx] = display_value;
        last_display_units[screen_idx] = unit_str;
    }
    
    // Buzzer alarm check for Number display
    // Low alarm: threshold in min[0][1], enable flag in buzzer[0][1]
    // High alarm: threshold in max[0][2], enable flag in buzzer[0][2]
    if (!isnan(display_value) && buzzer_mode != 0) {
        extern uint16_t buzzer_cooldown_sec;
        unsigned long ALERT_COOLDOWN_MS = (buzzer_cooldown_sec == 0) ? 0UL : (unsigned long)buzzer_cooldown_sec * 1000UL;
        unsigned long now_buz = millis();
        bool cooldown_ok = (now_buz - last_buzzer_time > ALERT_COOLDOWN_MS);
        bool low_armed  = (screen_configs[screen_idx].buzzer[0][1] != 0);
        bool high_armed = (screen_configs[screen_idx].buzzer[0][2] != 0);
        float low_thresh  = screen_configs[screen_idx].min[0][1];
        float high_thresh = screen_configs[screen_idx].max[0][2];
        bool low_fired  = low_armed  && (display_value < low_thresh);
        bool high_fired = high_armed && (display_value > high_thresh);
        if ((low_fired || high_fired) && (first_run_buzzer || cooldown_ok)) {
            if (now_buz - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                Serial.printf("[ALARM] screen=%d number=%.2f low=%s(%.2f) high=%s(%.2f)\n",
                    screen_idx, display_value,
                    low_fired  ? "TRIP" : "ok", low_thresh,
                    high_fired ? "TRIP" : "ok", high_thresh);
                last_alarm_log_time = now_buz;
            }
            trigger_buzzer_alert();
            last_buzzer_time = now_buz;
            first_run_buzzer = false;
        }
    }
}

// Update dual number displays for the active screen using live Signal K sensor values
static void update_dual_number_display_for_screen(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    
    int screen_idx = screen_num - 1;  // Convert to 0-based index
    
    // Get the configured Signal K paths for top and bottom displays
    String top_path = String(screen_configs[screen_idx].dual_top_path);
    String bottom_path = String(screen_configs[screen_idx].dual_bottom_path);
    
    // Note: Dual display creation now happens only in apply_all_screen_visuals() at boot
    // This ensures displays are only created for screens configured as DUAL type
    
    // Helper lambda to get value and metadata for a path
    auto get_path_data = [](const String& path, float& value, String& unit, String& description) {
        if (path.length() == 0) {
            value = 0.0f;
            unit = "No Path";
            description = "";
            return false;
        }
        
        // Get data directly by path (works for gauge, number, and dual display paths)
        value = get_sensor_value_by_path(path);
        unit = get_sensor_unit_by_path(path);
        description = get_sensor_description_by_path(path);
        
        if (isnan(value)) {
            unit = "N/A";
            description = "";
            return false;
        }
        
        value = convert_unit(value, unit, path, unit);
        return true;
    };
    
    // Get top display data
    float top_value = 0.0f;
    String top_unit = "";
    String top_description = "";
    get_path_data(top_path, top_value, top_unit, top_description);
    
    // Get bottom display data
    float bottom_value = 0.0f;
    String bottom_unit = "";
    String bottom_description = "";
    get_path_data(bottom_path, bottom_value, bottom_unit, bottom_description);
    
    // Update both displays
    dual_number_display_update_top(screen_idx, 
                                     isnan(top_value) ? 0.0f : top_value, 
                                     top_unit.c_str(), 
                                     top_description.c_str());
    dual_number_display_update_bottom(screen_idx, 
                                        isnan(bottom_value) ? 0.0f : bottom_value, 
                                        bottom_unit.c_str(), 
                                        bottom_description.c_str());

    // Buzzer alarm check for Dual display
    // Top:    min/max/buzzer[0][1] = low,  [0][2] = high
    // Bottom: min/max/buzzer[1][1] = low,  [1][2] = high
    if (buzzer_mode != 0) {
        extern uint16_t buzzer_cooldown_sec;
        unsigned long ALERT_COOLDOWN_MS = (buzzer_cooldown_sec == 0) ? 0UL : (unsigned long)buzzer_cooldown_sec * 1000UL;
        unsigned long now_buz = millis();
        bool cooldown_ok = (now_buz - last_buzzer_time > ALERT_COOLDOWN_MS);
        float dual_vals[2] = { top_value, bottom_value };
        for (int ch = 0; ch < 2; ch++) {
            if (isnan(dual_vals[ch])) continue;
            bool la = (screen_configs[screen_idx].buzzer[ch][1] != 0);
            bool ha = (screen_configs[screen_idx].buzzer[ch][2] != 0);
            bool lf = la && (dual_vals[ch] < screen_configs[screen_idx].min[ch][1]);
            bool hf = ha && (dual_vals[ch] > screen_configs[screen_idx].max[ch][2]);
            if ((lf || hf) && (first_run_buzzer || cooldown_ok)) {
                if (now_buz - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                    Serial.printf("[ALARM DUAL] screen=%d ch=%d val=%.2f low=%s(%.2f) high=%s(%.2f)\n",
                        screen_idx, ch, dual_vals[ch],
                        lf ? "TRIP" : "ok", screen_configs[screen_idx].min[ch][1],
                        hf ? "TRIP" : "ok", screen_configs[screen_idx].max[ch][2]);
                    last_alarm_log_time = now_buz;
                }
                trigger_buzzer_alert();
                last_buzzer_time = now_buz;
                first_run_buzzer = false;
                break; // one beep per update cycle
            }
        }
    }
}

// Update quad number displays for the active screen using live Signal K sensor values
static void update_quad_number_display_for_screen(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    
    int screen_idx = screen_num - 1;  // Convert to 0-based index
    
    // Get the configured Signal K paths for all 4 quadrants
    String tl_path = String(screen_configs[screen_idx].quad_tl_path);
    String tr_path = String(screen_configs[screen_idx].quad_tr_path);
    String bl_path = String(screen_configs[screen_idx].quad_bl_path);
    String br_path = String(screen_configs[screen_idx].quad_br_path);
    
    // Note: Quad display creation now happens only in apply_all_screen_visuals() at boot
    // This ensures displays are only created for screens configured as QUAD type
    
    // Helper lambda to get value and metadata for a path
    auto get_path_data = [](const String& path, float& value, String& unit, String& description) {
        if (path.length() == 0) {
            value = 0.0f;
            unit = "No Path";
            description = "";
            return false;
        }
        
        // Get data directly by path
        value = get_sensor_value_by_path(path);
        unit = get_sensor_unit_by_path(path);
        description = get_sensor_description_by_path(path);
        
        if (isnan(value)) {
            unit = "N/A";
            description = "";
            return false;
        }
        
        value = convert_unit(value, unit, path, unit);
        return true;
    };
    
    // Get data for all quadrants
    float tl_value, tr_value, bl_value, br_value;
    String tl_unit, tr_unit, bl_unit, br_unit;
    String tl_description, tr_description, bl_description, br_description;
    
    get_path_data(tl_path, tl_value, tl_unit, tl_description);
    get_path_data(tr_path, tr_value, tr_unit, tr_description);
    get_path_data(bl_path, bl_value, bl_unit, bl_description);
    get_path_data(br_path, br_value, br_unit, br_description);
    
    // Update all quadrants
    quad_number_display_update_tl(screen_idx, isnan(tl_value) ? 0.0f : tl_value, tl_unit.c_str(), tl_description.c_str());
    quad_number_display_update_tr(screen_idx, isnan(tr_value) ? 0.0f : tr_value, tr_unit.c_str(), tr_description.c_str());
    quad_number_display_update_bl(screen_idx, isnan(bl_value) ? 0.0f : bl_value, bl_unit.c_str(), bl_description.c_str());
    quad_number_display_update_br(screen_idx, isnan(br_value) ? 0.0f : br_value, br_unit.c_str(), br_description.c_str());

    // Buzzer alarm check for Quad display
    // Slot mapping: TL=g0 z1(low)/z2(high), TR=g0 z3(low)/z4(high)
    //               BL=g1 z1(low)/z2(high), BR=g1 z3(low)/z4(high)
    if (buzzer_mode != 0) {
        extern uint16_t buzzer_cooldown_sec;
        unsigned long ALERT_COOLDOWN_MS = (buzzer_cooldown_sec == 0) ? 0UL : (unsigned long)buzzer_cooldown_sec * 1000UL;
        unsigned long now_buz = millis();
        bool cooldown_ok = (now_buz - last_buzzer_time > ALERT_COOLDOWN_MS);
        bool quad_fired = false;
        auto check_quad = [&](float val, int g, int zl, int zh) {
            if (quad_fired || isnan(val)) return;
            bool la = (screen_configs[screen_idx].buzzer[g][zl] != 0);
            bool ha = (screen_configs[screen_idx].buzzer[g][zh] != 0);
            bool lf = la && (val < screen_configs[screen_idx].min[g][zl]);
            bool hf = ha && (val > screen_configs[screen_idx].max[g][zh]);
            if ((lf || hf) && (first_run_buzzer || cooldown_ok)) quad_fired = true;
        };
        check_quad(tl_value, 0, 1, 2);
        check_quad(tr_value, 0, 3, 4);
        check_quad(bl_value, 1, 1, 2);
        check_quad(br_value, 1, 3, 4);
        if (quad_fired) {
            if (now_buz - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                Serial.printf("[ALARM QUAD] screen=%d fired\n", screen_idx);
                last_alarm_log_time = now_buz;
            }
            trigger_buzzer_alert();
            last_buzzer_time = now_buz;
            first_run_buzzer = false;
        }
    }
}

// Update gauge+number display for the active screen using live Signal K sensor values
static void update_gauge_number_display_for_screen(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    
    int screen_idx = screen_num - 1;  // Convert to 0-based index
    
    // Get the configured Signal K path for the center number display
    String center_path = String(screen_configs[screen_idx].gauge_num_center_path);
    
    // Note: Gauge+Number display creation now happens only in apply_all_screen_visuals() at boot
    // This ensures displays are only created for screens configured as GAUGE_NUMBER type
    
    // Helper lambda to get value and metadata for a path
    auto get_path_data = [](const String& path, float& value, String& unit, String& description) {
        if (path.length() == 0) {
            value = 0.0f;
            unit = "No Path";
            description = "";
            return false;
        }
        
        // Get data directly by path
        value = get_sensor_value_by_path(path);
        unit = get_sensor_unit_by_path(path);
        description = get_sensor_description_by_path(path);
        
        if (isnan(value)) {
            unit = "N/A";
            description = "";
            return false;
        }
        
        value = convert_unit(value, unit, path, unit);
        return true;
    };
    
    // Get center display data
    float center_value = 0.0f;
    String center_unit = "";
    String center_description = "";
    get_path_data(center_path, center_value, center_unit, center_description);
    
    // Update center number display
    gauge_number_display_update_center(screen_idx, 
                                         isnan(center_value) ? 0.0f : center_value, 
                                         center_unit.c_str(), 
                                         center_description.c_str());

    // Buzzer alarm check for Gauge+Number center display
    // min[1][1]/buzzer[1][1]=low, max[1][2]/buzzer[1][2]=high
    if (!isnan(center_value) && buzzer_mode != 0) {
        extern uint16_t buzzer_cooldown_sec;
        unsigned long ALERT_COOLDOWN_MS = (buzzer_cooldown_sec == 0) ? 0UL : (unsigned long)buzzer_cooldown_sec * 1000UL;
        unsigned long now_buz = millis();
        bool cooldown_ok = (now_buz - last_buzzer_time > ALERT_COOLDOWN_MS);
        bool la = (screen_configs[screen_idx].buzzer[1][1] != 0);
        bool ha = (screen_configs[screen_idx].buzzer[1][2] != 0);
        bool lf = la && (center_value < screen_configs[screen_idx].min[1][1]);
        bool hf = ha && (center_value > screen_configs[screen_idx].max[1][2]);
        if ((lf || hf) && (first_run_buzzer || cooldown_ok)) {
            if (now_buz - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                Serial.printf("[ALARM GNUM] screen=%d center=%.2f low=%s(%.2f) high=%s(%.2f)\n",
                    screen_idx, center_value,
                    lf ? "TRIP" : "ok", screen_configs[screen_idx].min[1][1],
                    hf ? "TRIP" : "ok", screen_configs[screen_idx].max[1][2]);
                last_alarm_log_time = now_buz;
            }
            trigger_buzzer_alert();
            last_buzzer_time = now_buz;
            first_run_buzzer = false;
        }
    }
}

static void update_graph_display_for_screen(int screen_num) {
    if (screen_num < 1 || screen_num > 5) return;
    
    int screen_idx = screen_num - 1;  // Convert to 0-based index
    
    // Get the configured Signal K path for the graph
    String graph_path = String(screen_configs[screen_idx].number_path);  // Reuse number_path field
    
    // Helper lambda to get value and metadata for a path
    auto get_path_data = [](const String& path, float& value, String& unit, String& description) {
        if (path.length() == 0) {
            value = 0.0f;
            unit = "No Path";
            description = "";
            return false;
        }
        
        // Get data directly by path
        value = get_sensor_value_by_path(path);
        unit = get_sensor_unit_by_path(path);
        description = get_sensor_description_by_path(path);
        
        if (isnan(value)) {
            unit = "N/A";
            description = "";
            return false;
        }
        
        value = convert_unit(value, unit, path, unit);
        return true;
    };
    
    // Get graph data for first series
    float graph_value = 0.0f;
    String graph_unit = "";
    String graph_description = "";
    get_path_data(graph_path, graph_value, graph_unit, graph_description);
    
    // Get graph data for second series (if configured)
    String graph_path_2 = String(screen_configs[screen_idx].graph_path_2);
    float graph_value_2 = NAN;
    String graph_unit_2 = "";
    String graph_description_2 = "";
    if (graph_path_2.length() > 0) {
        get_path_data(graph_path_2, graph_value_2, graph_unit_2, graph_description_2);
    }
    
    // Update graph display (adds new data point)
    graph_display_update(screen_idx, 
                        isnan(graph_value) ? 0.0f : graph_value, 
                        graph_unit.c_str(), 
                        graph_description.c_str(),
                        graph_value_2,  // Pass NAN if not configured
                        graph_unit_2.c_str(),
                        graph_description_2.c_str());
}

// Update both needles for the active screen using live Signal K sensor values
extern "C" void update_needles_for_screen(int screen_num) {
    // Index 1-5 correspond to Screen1..Screen5
    if (screen_num < 1 || screen_num > 5) return;
    
    // Check display type for this screen (0-based index)
    int screen_idx = screen_num - 1;
    if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_NUMBER) {
        // Number display mode - show single large number
        update_number_display_for_screen(screen_num);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_DUAL) {
        // Dual display mode - show two numbers (top and bottom)
        update_dual_number_display_for_screen(screen_num);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_QUAD) {
        // Quad display mode - show four numbers (TL, TR, BL, BR)
        update_quad_number_display_for_screen(screen_num);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_GAUGE_NUMBER) {
        // Gauge+Number display mode - show gauge on top, number in center
        update_gauge_number_display_for_screen(screen_num);
        // Note: Don't return here - we still need to update the gauge needle below
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_GRAPH) {
        // Graph display mode - show LVGL chart
        update_graph_display_for_screen(screen_num);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_POSITION) {
        // Position display mode - show lat/lon and UTC time from SignalK
        position_display_update(screen_idx,
            (float)g_nav_latitude, (float)g_nav_longitude, g_nav_datetime);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_COMPASS) {
        // Compass display — heading from NMEA 2000 PGN 127250 (already in degrees)
        String hdg_path = String(screen_configs[screen_idx].number_path);
        if (hdg_path.length() == 0) hdg_path = String((int)N2K_HEADING);
        float hdg_deg = get_sensor_value_by_path(hdg_path);
        if (!isnan(hdg_deg)) {
            compass_display_update(screen_idx, hdg_deg, 0);
        }
        // BL / BR extra data fields
        auto compass_get_field = [](const String& path, float& val, String& unit, String& desc) {
            if (path.length() == 0) { val = NAN; unit = ""; desc = ""; return; }
            val  = get_sensor_value_by_path(path);
            unit = get_sensor_unit_by_path(path);
            desc = get_sensor_description_by_path(path);
            val = convert_unit(val, unit, path, unit);
        };
        String bl_path = String(screen_configs[screen_idx].quad_bl_path);
        String br_path = String(screen_configs[screen_idx].quad_br_path);
        float bl_val, br_val; String bl_unit, br_unit, bl_desc, br_desc;
        compass_get_field(bl_path, bl_val, bl_unit, bl_desc);
        compass_get_field(br_path, br_val, br_unit, br_desc);
        compass_display_update_bl(screen_idx, bl_val, bl_unit.c_str(), bl_desc.c_str());
        compass_display_update_br(screen_idx, br_val, br_unit.c_str(), br_desc.c_str());
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_ATTITUDE) {
        // Attitude display — pitch/roll/yaw from onboard IMU
        float att_pitch, att_roll, att_yaw;
        attitude_imu_read(&att_pitch, &att_roll, &att_yaw);
        attitude_display_update(screen_idx, att_pitch, att_roll, att_yaw);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_ANCHOR) {
        // Anchor alarm — own-boat GPS position + COG/SOG
        float own_cog_deg = get_sensor_value_by_path(String((int)N2K_COG));
        float own_sog_ms  = get_sensor_value_by_path(String((int)N2K_SOG));
        anchor_display_update(screen_idx,
            (float)g_nav_latitude, (float)g_nav_longitude,
            own_cog_deg, own_sog_ms);
        return;
    } else if (screen_configs[screen_idx].display_type == DISPLAY_TYPE_AIS) {
        // AIS radar display — N2K targets arrive via PGN handlers;
        // fall back to Signal K REST API when WiFi is connected and N2K is not running.
        if (!is_nmea2000_running()) {
            String sk_ip = get_signalk_server_ip();
            uint16_t sk_port = get_signalk_server_port();
            ais_fetch_targets(sk_ip.c_str(), sk_port);
        }
        // Own-boat nav data: COG/SOG/heading from sensor map
        float own_cog_deg = get_sensor_value_by_path(String((int)N2K_COG));
        float own_sog_ms  = get_sensor_value_by_path(String((int)N2K_SOG));
        float own_hdg_deg = get_sensor_value_by_path(String((int)N2K_HEADING));
        float own_cog = own_cog_deg;  // already degrees
        float own_sog = isnan(own_sog_ms) ? NAN : convert_speed(own_sog_ms);
        ais_display_update(screen_idx, (float)g_nav_latitude, (float)g_nav_longitude,
                           own_cog, own_sog, own_hdg_deg);
        return;
    }
    
    // If test mode is active, skip all live data updates
    extern bool test_mode;
    if (test_mode) return;

    // Default angles: top needles at 0°, bottom needles at 180°
    // Use the global `last_top_angle` / `last_bottom_angle` defined at file scope
    extern int16_t last_top_angle[6];
    extern int16_t last_bottom_angle[6];
    static bool initialized[6] = {false, false, false, false, false, false}; // Track if needles have been set to defaults

    lv_obj_t* top_needle = NULL;
    lv_obj_t* bottom_needle = NULL;
    ParamType top_type = PARAM_RPM;
    ParamType bottom_type = PARAM_COOLANT_TEMP;
    float top_value = 0.0f;
    float bottom_value = 0.0f;

    switch (screen_num) {
        case 1:  // RPM + Coolant Temp
            top_needle = ui_Needle;
            bottom_needle = ui_Lower_Needle;
            top_value = get_sensor_value(SCREEN1_RPM);
            bottom_value = get_sensor_value(SCREEN1_COOLANT_TEMP);
            top_type = PARAM_RPM;
            bottom_type = PARAM_COOLANT_TEMP;
            break;
        case 2:  // RPM + Fuel
            top_needle = ui_Needle2;
            bottom_needle = ui_Lower_Needle2;
            top_value = get_sensor_value(SCREEN2_RPM);
            bottom_value = get_sensor_value(SCREEN2_FUEL);
            top_type = PARAM_RPM;
            bottom_type = PARAM_FUEL;
            break;
        case 3:  // Coolant Temp + Exhaust Temp
            top_needle = ui_Needle3;
            bottom_needle = ui_Lower_Needle3;
            top_value = get_sensor_value(SCREEN3_COOLANT_TEMP);
            bottom_value = get_sensor_value(SCREEN3_EXHAUST_TEMP);
            top_type = PARAM_COOLANT_TEMP;
            bottom_type = PARAM_EXHAUST_TEMP;
            break;
        case 4:  // Fuel + Coolant Temp
            top_needle = ui_Needle4;
            bottom_needle = ui_Lower_Needle4;
            top_value = get_sensor_value(SCREEN4_FUEL);
            bottom_value = get_sensor_value(SCREEN4_COOLANT_TEMP);
            top_type = PARAM_FUEL;
            bottom_type = PARAM_COOLANT_TEMP;
            break;
        case 5:  // Oil Pressure + Coolant Temp
            top_needle = ui_Needle5;
            bottom_needle = ui_Lower_Needle5;
            top_value = get_sensor_value(SCREEN5_OIL_PRESSURE);
            bottom_value = get_sensor_value(SCREEN5_COOLANT_TEMP);
            top_type = PARAM_OIL_PRESSURE;
            bottom_type = PARAM_COOLANT_TEMP;
            break;
        default:
            return;
    }

    // Set defaults on first run, then use sensor data or keep defaults if no valid data
    int16_t top_angle, bottom_angle;
    
    if (!initialized[screen_num]) {
        // First run: set to defaults
        top_angle = 0;    // Top needles start at 0°
        bottom_angle = 180; // Bottom needles start at 180°
        initialized[screen_num] = true;
    } else {
        // Use sensor data if valid (not NAN); otherwise keep current position.
        // Previous logic excluded zero values (e.g. RPM==0) which prevented
        // needles from updating when a valid zero reading was present. Treat
        // any non-NAN value as valid here.
        if (!isnan(top_value)) {
            top_angle = value_to_angle_for_param(top_value, screen_num - 1, 0);  // 0 = top gauge
        } else {
            top_angle = last_top_angle[screen_num]; // Keep current position if no valid data
        }

        if (!isnan(bottom_value)) {
            bottom_angle = value_to_angle_for_param(bottom_value, screen_num - 1, 1);  // 1 = bottom gauge
        } else {
            bottom_angle = last_bottom_angle[screen_num]; // Keep current position if no valid data
        }
    }


    // Reduced debug output for production build

    animate_generic_needle(top_needle, last_top_angle[screen_num], top_angle, false);
    animate_generic_needle(bottom_needle, last_bottom_angle[screen_num], bottom_angle, true);

    // Update dynamic icon recoloring for this screen/gauges based on current values
    lv_obj_t* top_icon = NULL;
    lv_obj_t* bottom_icon = NULL;

    switch (screen_num) {
        case 1:
            top_icon = ui_TopIcon1;
            bottom_icon = ui_BottomIcon1;
            break;
        case 2:
            top_icon = ui_TopIcon2;
            bottom_icon = ui_BottomIcon2;
            break;
        case 3:
            top_icon = ui_TopIcon3;
            bottom_icon = ui_BottomIcon3;
            break;
        case 4:
            top_icon = ui_TopIcon4;
            bottom_icon = ui_BottomIcon4;
            break;
        case 5:
            top_icon = ui_TopIcon5;
            bottom_icon = ui_BottomIcon5;
            break;
        default:
            break;
    }

    if (top_icon) _ui_apply_icon_style(top_icon, screen_num - 1, 0);
    if (bottom_icon) _ui_apply_icon_style(bottom_icon, screen_num - 1, 1);
}

// Move the specified gauge (top/bottom) on a given screen to the specified angle for testing
void test_move_gauge(int screen, int gauge, int angle) {
    // screen: 0-4 (Screen1..Screen5), gauge: 0=top, 1=bottom
    lv_obj_t* top_needles[5] = {ui_Needle, ui_Needle2, ui_Needle3, ui_Needle4, ui_Needle5};
    lv_obj_t* bottom_needles[5] = {ui_Lower_Needle, ui_Lower_Needle2, ui_Lower_Needle3, ui_Lower_Needle4, ui_Lower_Needle5};
    extern int16_t last_top_angle[6];
    extern int16_t last_bottom_angle[6];
    if (screen < 0 || screen > 4) {
        // Debug output disabled for performance
        return;
    }
    int idx = screen + 1; // last_*_angle arrays are 1-based (Screen1..Screen5)
    // Debug output disabled for performance
    if (gauge == 0) {
        // Top gauge (line)
        if (top_needles[screen]) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, top_needles[screen]);
            lv_anim_set_exec_cb(&a, needle_anim_cb);
            lv_anim_set_values(&a, last_top_angle[idx], angle);
            lv_anim_set_time(&a, 500);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
            last_top_angle[idx] = angle;
        } else {
            // Debug output disabled for performance
        }
    } else if (gauge == 1) {
        // Bottom gauge (line)
        if (bottom_needles[screen]) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, bottom_needles[screen]);
            lv_anim_set_exec_cb(&a, lower_needle_anim_cb);
            lv_anim_set_values(&a, last_bottom_angle[idx], angle);
            lv_anim_set_time(&a, 500);
            lv_anim_set_path_cb(&a, lv_anim_path_linear);
            lv_anim_start(&a);
            last_bottom_angle[idx] = angle;
        } else {
            // Debug output disabled for performance
        }
    } else {
        // Debug output disabled for performance
    }
}

// ets_printf writes directly to hardware UART0 (visible on CH343 monitor)
// even when Serial uses USB-OTG CDC (ARDUINO_USB_CDC_ON_BOOT=1).
extern "C" int ets_printf(const char *fmt, ...);

void setup() {
    // Immediate hardware UART0 marker - visible on CH343 even before USB CDC comes up
    ets_printf("\r\n\r\n*** SETUP() START ***\r\n");

    // CRITICAL: silence the buzzer ASAP and configure IO expander.
    // V3 (TCA9554): Set all outputs, PIN6 LOW.
    // V4 (CH32V003): Set OUTPUT=0xFF (all high), DIRECTION=0x3A (BEE_EN as input = safe).
    //   CH32V003 powers up with all outputs HIGH and all pins as inputs.
    //   BEE_EN as input = high-Z → SS8050 base has no drive → buzzer OFF.
    I2C_Init();
    delay(10); // let Wire bus settle before I2C transactions
    // Detect board version (v3=0x20, v4=0x24) before any expander operations
    detect_expander_address();

    if (is_board_v4()) {
      // V4 (CH32V003): Write OUTPUT before DIR to prevent buzzer glitch.
      // CH32V003 powers on with output latch=0xFF (all HIGH). Writing DIR=0xFF
      // first would momentarily drive BEE_EN HIGH (buzzer on) before OUTPUT=0x3A
      // silences it. Writing OUTPUT first ensures safe levels before any pin
      // becomes an output.
      //
      // Keep TP_RST LOW during init (OUT_NORMAL & ~0x02 = 0x38) so GT911 is held
      // in reset while we prepare. GT911 address selection follows below.
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL & ~0x02u); // TP_RST LOW
      I2C_Write_EXIO(CH32V003_DIR_REG, CH32V003_DIR_ALL_OUT);

      // GT911 address selection: with CH32V003 DIR=0xFF and OUTPUT bit2=0,
      // EXIO2 (TP_INT) is driven LOW by CH32V003. We also drive GPIO16 LOW to
      // guarantee INT is LOW even if CH32V003 state is corrupted (e.g. after
      // esptool upload partially corrupted the DIR register via I2C bus recovery).
      // TP_RST is already LOW from the write above — release it HIGH now with
      // INT guaranteed LOW → GT911 latches address 0x5D.
      pinMode(GT911_INT_PIN, OUTPUT);
      digitalWrite(GT911_INT_PIN, LOW);
      delay(50);                                                  // hold RST LOW
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL);  // TP_RST HIGH, INT via EXIO2 still LOW
      delay(120);                                                 // GT911 boots at 0x5D
      pinMode(GT911_INT_PIN, INPUT);                             // release GPIO16; EXIO2 still drives INT
      ets_printf("[GT911] address pre-selected 0x5D via RST sequence\r\n");
    } else {
      // V3 (TCA9554): Write output latch before switching to output mode
      Set_EXIOS(0xDF);             // bit5=0 (PIN6 LOW on V3)
      TCA9554PWR_Init(0x00);       // CONFIG = all outputs
    }
    ets_printf("*** IO expander configured (board %s) ***\r\n", is_board_v4() ? "v4" : "v3");

    // Serial for debugging - with timeout
    Serial.setTxTimeoutMs(0);  // Non-blocking serial
    Serial.begin(115200);
    delay(500);

    ets_printf("*** Serial.begin done ***\r\n");
    Serial.println("\n\n=== ESP32 Square Display Starting ===");
    Serial.printf("Firmware version: %s\n", FW_VERSION);
    Serial.flush();
    
    // I2C and IO expander already initialized above; re-assert safe state after delay
    if (!is_board_v4()) {
      Set_EXIO(EXIO_PIN6, Low);    // V3: re-assert PIN6 LOW
    }
    ets_printf("*** I2C+expander done ***\r\n");
    Serial.println("I2C and IO expander initialized");
    Serial.flush();

    // Initialize the on-board RTC (PCF85063) for local timekeeping
    PCF85063_Init();
    Serial.println("RTC (PCF85063) initialized");
    Serial.flush();
    
    // Shared SPI bus initialization (used by ST7701 command SPI and SDSPI)
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = LCD_MOSI_PIN;
        buscfg.miso_io_num = SD_D0_PIN; // SD data0 / MISO
        buscfg.sclk_io_num = LCD_CLK_PIN;
        buscfg.quadwp_io_num = -1;
        buscfg.quadhd_io_num = -1;
        buscfg.max_transfer_sz = 4000; // allow larger transfers for SD
        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ret == ESP_OK) {
            Serial.println("Shared SPI bus initialized");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            Serial.println("Shared SPI bus already initialized (ok)");
        } else {
            Serial.printf("Shared SPI bus init failed: 0x%08x\n", ret);
        }
    }

    // Stage 1: Silence the SD card (do NOT call SD_MMC.begin() yet)
    // IO expander is already initialized above; set EXIO_PIN4 HIGH to tell the
    // SD card to ignore the shared SPI bus.
    Serial.println("SD: silencing SD (EXIO_PIN4 HIGH) before display init");
    SD_D3_EN();
    vTaskDelay(pdMS_TO_TICKS(10));

    // Quiet logging for display initialization
    esp_log_level_set("st7701_rgb", ESP_LOG_WARN);
    esp_log_level_set("esp_lcd", ESP_LOG_WARN);
    esp_log_level_set("esp_panel", ESP_LOG_WARN);

    // Initialize the display
    LCD_Init();
    // Re-assert safe IO expander state after LCD_Init (which may have modified registers)
    if (is_board_v4()) {
      // V4: re-assert Waveshare-matching CH32V003 state after LCD_Init
      I2C_Write_EXIO(CH32V003_DIR_REG, CH32V003_DIR_ALL_OUT);
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL);
    } else {
      // V3: re-assert all outputs with PIN6 LOW
      Set_EXIOS(Read_EXIOS(exio_output_reg()) & (uint8_t)~(1 << (EXIO_PIN6 - 1)));
      Mode_EXIOS(0x00);
      Set_EXIO(EXIO_PIN6, Low);
    }

    // Heap integrity check — narrow down PSRAM corruption source
    if (!heap_caps_check_integrity_all(true)) {
        ets_printf("[HEAP] *** CORRUPTION detected AFTER LCD_Init ***\r\n");
    } else {
        ets_printf("[HEAP] OK after LCD_Init (PSRAM free=%u)\r\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // Initialize GT911 touch controller.
    // V4: Waveshare approach — no reset, just probe both addresses.
    // V3: reset with INT=LOW to force address 0x5D, attach interrupt.
    Touch_Init();
    Serial.println("Touch controller initialized");
    Serial.flush();

    // Heap integrity check after touch init
    if (!heap_caps_check_integrity_all(true)) {
        ets_printf("[HEAP] *** CORRUPTION detected AFTER Touch_Init ***\r\n");
    }

    // Stage 3: Full SD re-init now that the display has finished taking the SPI pins
    Serial.println("SD: now performing SD_MMC.begin('/sdcard', true) after display init");
    if (SD_Init() == ESP_OK) {
        Serial.println("SD_Init: success");
    } else {
        Serial.println("SD_Init: failed");
    }
    Serial.println("SD card initialized");
    Serial.flush();
    Serial.print("LCD initialized at ");
    Serial.print(ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ / 1000000);  // MHz
    Serial.println("MHz pixel clock");
    Serial.flush();

    Serial.println("SD: re-enabling (expander CS) after LCD vendor init and checking health");
    SD_D3_EN();
    vTaskDelay(pdMS_TO_TICKS(20));
    SD_RecoveryCheck();

    // Ensure backlight is on for normal operation
    Set_Backlight(100);


    
    // Load persisted preferences BEFORE initializing the UI so dynamic image paths
    // are available during screen construction.
    load_preferences();

    // LVGL
    Lvgl_Init();

    // Heap integrity check after LVGL init (allocates large PSRAM buffers + DMA)
    if (!heap_caps_check_integrity_all(true)) {
        ets_printf("[HEAP] *** CORRUPTION detected AFTER Lvgl_Init ***\r\n");
    } else {
        ets_printf("[HEAP] OK after Lvgl_Init (PSRAM free=%u)\r\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    // Initialize RGB565 binary image decoder (fast loading, no PNG decode overhead)
    rgb565_decoder_init();
    Serial.println("RGB565 decoder initialized");
    Serial.flush();

    ui_init();  // Load SquareLine UI
    night_mode_init_overlays();  // Create night mode overlays on all screens
    Serial.println("LVGL and UI initialized");
    Serial.flush();

    // Try to apply persisted screen visuals at boot. If apply fails (no UI objects yet or no assets),
    // show fallback error screen to help users who haven't uploaded configs or assets.
    bool applied_boot = apply_all_screen_visuals();
    if (!applied_boot) {
        // No visuals applied — show a simple error message so user sees a diagnostics message
        // after LVGL is initialized (calling LVGL APIs is safe now).
        show_fallback_error_screen_if_needed();
    }

    
    // Apply persisted needle styles (colors, widths, lengths, pivot)
    apply_all_needle_styles();

    // Initialize all needles to default positions
    initialize_needle_positions();
    Serial.println("Needle positions initialized");
    Serial.flush();
    
    // Initialize gauge configuration
    gauge_config_init();
    Serial.println("Gauge configuration loaded");
    Serial.flush();

    // Setup auto-scroll timer if configured
    extern uint16_t auto_scroll_sec;
    if (auto_scroll_sec > 0) {
        set_auto_scroll_interval(auto_scroll_sec);
    }
    
    // Initialize sensor mutex for thread-safe access
    init_sensor_mutex();
    
    // Enable WiFi with optimizations
    Serial.println("Starting WiFi setup...");
    Serial.flush();
    setup_network();
    Serial.println("WiFi setup complete");
    g_last_activity_ms = millis(); // start the inactivity timer after boot
    Serial.flush();

    // Heap integrity check after WiFi/network init
    if (!heap_caps_check_integrity_all(true)) {
        ets_printf("[HEAP] *** CORRUPTION detected AFTER setup_network ***\r\n");
    }
    
    // Initialize QMI8658 IMU (accel + gyro) and load level calibration
    QMI8658_Init();
    attitude_load_calibration();
    Serial.println("IMU initialized");
    Serial.flush();

    // Start NMEA 2000 CAN bus listener
    Serial.println("Starting NMEA 2000...");
    Serial.flush();
    enable_nmea2000();
    
    Serial.println("Display initialized with WiFi optimizations.");
    Serial.print("WiFi SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Navigate to http://esp32-squaredisplay.local or check your router for device IP");
    Serial.flush();
}

void loop() {
    config_server.handleClient();

    // --- Screen-off timeout ---------------------------------------------------
    // g_last_activity_ms is updated on every touch in Lvgl_Touchpad_Read().
    // When the timeout fires: backlight off + WiFi modem sleep.
    // Wake is handled in Lvgl_Touchpad_Read(): first touch restores everything
    // and is swallowed so it doesn't trigger a UI action.
    if (screen_off_timeout_min > 0) {
        uint32_t now_ms = millis();
        uint32_t timeout_ms = (uint32_t)screen_off_timeout_min * 60UL * 1000UL;
        if (!g_screen_is_off && (now_ms - g_last_activity_ms >= timeout_ms)) {
            g_screen_is_off = true;
            Set_Backlight(0);
            WiFi.setSleep(true);
            Serial.println("[SCREEN] Screen off — power saving active");
        }
    }
    // -------------------------------------------------------------------------

    // --- RTC is the sole clock source for display ----------------------------
    // RTC drives g_nav_datetime every second.
    // SK datetime (g_sk_datetime) only syncs TO the RTC at startup and then
    // every 10 minutes — it never touches g_nav_datetime directly.
    {
        static uint32_t last_rtc_read_ms   = 0;
        static uint32_t last_rtc_sync_ms   = 0;
        static char     prev_sk_datetime[32] = {0};
        uint32_t now = millis();

        // Sync SignalK → RTC when a new SK datetime arrives (at startup + every 10 min)
        if (g_sk_datetime[0] != '\0' &&
            strcmp(g_sk_datetime, prev_sk_datetime) != 0 &&
            (now - last_rtc_sync_ms > 600000UL || last_rtc_sync_ms == 0))
        {
            int yr, mo, dy, hr, mn, sc;
            if (sscanf(g_sk_datetime, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
                datetime_t rtc_time = {};
                rtc_time.year   = (uint16_t)yr;
                rtc_time.month  = (uint8_t)mo;
                rtc_time.day    = (uint8_t)dy;
                rtc_time.hour   = (uint8_t)hr;
                rtc_time.minute = (uint8_t)mn;
                rtc_time.second = (uint8_t)sc;
                PCF85063_Set_All(rtc_time);
                last_rtc_sync_ms = now;
                Serial.printf("[RTC] Synced from SignalK: %04d-%02d-%02dT%02d:%02d:%02d\n",
                              yr, mo, dy, hr, mn, sc);
            }
            strncpy(prev_sk_datetime, g_sk_datetime, 31);
        }

        // Read RTC every second — this is the ONLY writer to g_nav_datetime
        if (now - last_rtc_read_ms >= 1000) {
            last_rtc_read_ms = now;
            datetime_t t;
            PCF85063_Read_Time(&t);
            if (t.year >= 2024) {
                snprintf(g_nav_datetime, sizeof(g_nav_datetime),
                         "%04d-%02d-%02dT%02d:%02d:%02dZ",
                         t.year, t.month, t.day, t.hour, t.minute, t.second);
            }
        }
    }
    // -------------------------------------------------------------------------

    // Use Signal K data instead of demo animation
    static int16_t needle_angle = 0;
    static int16_t lower_needle_angle = 0;
    static unsigned long last_needle_update = 0;
    
    // Switch to Signal K mode
    static bool use_demo_mode = false;
    
    // Skip all widget access when error screen replaced UI (widgets destroyed)
    if (g_error_screen_active) {
        // Still process deferred visual apply so web UI config saves can rebuild screens
        if (g_pending_visual_apply) {
            g_pending_visual_apply = false;
            // Rebuild all screens from the new config
            if (apply_all_screen_visuals()) {
                g_error_screen_active = false;
                Serial.println("[LOOP] Error screen cleared — config applied successfully");
            }
        }
        // Only run web server + LVGL timer, no gauge/needle/icon updates
        Lvgl_Loop();
        return;
    }

    // Check if in setup mode - use preview angles instead of Signal K data
    if (gauge_is_setup_mode()) {
        int16_t top_angle = gauge_get_preview_top_angle();
        int16_t bottom_angle = gauge_get_preview_bottom_angle();
        rotate_needle(top_angle);
        rotate_lower_needle(bottom_angle);
    } else if (use_demo_mode) {
        needle_angle = (needle_angle + 1) % 360;
        lower_needle_angle = (lower_needle_angle + 2) % 360;
        
        rotate_needle(needle_angle);
        rotate_lower_needle(lower_needle_angle);
    } else if (!is_n2k_updates_paused() && !is_signalk_ws_paused()) {
        // Update needles/displays every 100ms for smooth operation.
        // Skip entirely when config page is open to prevent LVGL races.
        unsigned long now = millis();
        if (now - last_needle_update >= 100) {
            int current_screen = ui_get_current_screen();

            // If the visible screen changed since last update, force-apply
            // the stored angles to the needle objects so the display shows
            // the last-known values immediately (even if angles match).
            static int last_seen_screen = 0;
            if (current_screen != last_seen_screen) {
                extern int16_t last_top_angle[6];
                extern int16_t last_bottom_angle[6];
                lv_obj_t* top_needle = NULL;
                lv_obj_t* bottom_needle = NULL;
                switch (current_screen) {
                    case 1: top_needle = ui_Needle; bottom_needle = ui_Lower_Needle; break;
                    case 2: top_needle = ui_Needle2; bottom_needle = ui_Lower_Needle2; break;
                    case 3: top_needle = ui_Needle3; bottom_needle = ui_Lower_Needle3; break;
                    case 4: top_needle = ui_Needle4; bottom_needle = ui_Lower_Needle4; break;
                    case 5: top_needle = ui_Needle5; bottom_needle = ui_Lower_Needle5; break;
                    default: break;
                }
                // Directly invoke the animation callbacks to set the line points
                // immediately (no animation) so the visual state matches the
                // stored angles.
                if (top_needle) needle_anim_cb(top_needle, last_top_angle[current_screen]);
                if (bottom_needle) lower_needle_anim_cb(bottom_needle, last_bottom_angle[current_screen]);
                last_seen_screen = current_screen;

                // Re-subscribe to only the active screen's SignalK paths
                subscribe_to_active_screen(current_screen);

                // If a save happened while this screen was inactive, re-apply its visuals
                // NOW while it is active so LVGL actually renders them.
                int cs0 = current_screen - 1;
                if (cs0 >= 0 && cs0 < 5 && g_screens_need_apply[cs0]) {
                    g_screens_need_apply[cs0] = false;
                    Serial.printf("[LOOP] lazy apply for screen %d\n", cs0);
                    apply_screen_visuals_for_one(cs0);
                }
            }

            update_needles_for_screen(current_screen);
            
            // Background graph data collection for non-visible graph screens
            // This ensures PSRAM buffers keep accumulating data even when swiped away
            for (int bg = 0; bg < NUM_SCREENS; bg++) {
                if (bg == (current_screen - 1)) continue;  // Active screen handled above
                if (screen_configs[bg].display_type != DISPLAY_TYPE_GRAPH) continue;
                update_graph_display_for_screen(bg + 1);  // 1-based screen number
            }
            
            last_needle_update = now;
        }
        
        // Update icon styles and optionally trigger buzzer alerts per configured zone
        {
            static int last_zone_state[2] = {-1, -1}; // last selected zone per gauge (top=0,bottom=1)
            unsigned long ALERT_COOLDOWN_MS = 60000;
            // Use user-configured buzzer cooldown (seconds) from settings. 0 => constant (no cooldown)
            extern uint16_t buzzer_cooldown_sec;
            if (buzzer_cooldown_sec == 0) ALERT_COOLDOWN_MS = 0;
            else ALERT_COOLDOWN_MS = (unsigned long)buzzer_cooldown_sec * 1000UL;

            int current_screen = ui_get_current_screen();
            int screen_idx = current_screen - 1;
            if (screen_idx < 0) screen_idx = 0;

            // Map icons for convenience
            lv_obj_t* icons[2] = { NULL, NULL };
            switch (current_screen) {
                case 1: icons[0] = ui_TopIcon1; icons[1] = ui_BottomIcon1; break;
                case 2: icons[0] = ui_TopIcon2; icons[1] = ui_BottomIcon2; break;
                case 3: icons[0] = ui_TopIcon3; icons[1] = ui_BottomIcon3; break;
                case 4: icons[0] = ui_TopIcon4; icons[1] = ui_BottomIcon4; break;
                case 5: icons[0] = ui_TopIcon5; icons[1] = ui_BottomIcon5; break;
                default: break;
            }

            // Determine runtime values for top/bottom gauges for this screen
            float runtime_val[2] = { NAN, NAN };
            switch (current_screen) {
                case 1:
                    runtime_val[0] = get_sensor_value(SCREEN1_RPM);
                    runtime_val[1] = get_sensor_value(SCREEN1_COOLANT_TEMP);
                    break;
                case 2:
                    runtime_val[0] = get_sensor_value(SCREEN2_RPM);
                    runtime_val[1] = get_sensor_value(SCREEN2_FUEL);
                    break;
                case 3:
                    runtime_val[0] = get_sensor_value(SCREEN3_COOLANT_TEMP);
                    runtime_val[1] = get_sensor_value(SCREEN3_EXHAUST_TEMP);
                    break;
                case 4:
                    runtime_val[0] = get_sensor_value(SCREEN4_FUEL);
                    runtime_val[1] = get_sensor_value(SCREEN4_COOLANT_TEMP);
                    break;
                case 5:
                    runtime_val[0] = get_sensor_value(SCREEN5_OIL_PRESSURE);
                    runtime_val[1] = get_sensor_value(SCREEN5_COOLANT_TEMP);
                    break;
                default:
                    break;
            }

            // For each gauge, choose zone and optionally trigger buzzer if configured
            for (int g = 0; g < 2; ++g) {
                lv_obj_t* icon = icons[g];
                if (icon == NULL) continue;
                // Only apply gauge zone logic for gauge-type screens
                // (number/dual/quad/graph screens use their own alarm checks)
                if (screen_configs[screen_idx].display_type != DISPLAY_TYPE_GAUGE &&
                    screen_configs[screen_idx].display_type != DISPLAY_TYPE_GAUGE_NUMBER) continue;

                float val = runtime_val[g];
                int chosen_zone = -1;
                // Prefer the most specific matching zone (smallest range) so
                // narrow alert zones win when ranges overlap.
                float best_range = 1e30f;
                for (int z = 1; z <= 4; ++z) {
                    float mn = screen_configs[screen_idx].min[g][z];
                    float mx = screen_configs[screen_idx].max[g][z];
                    if (mn == mx) continue;
                    if (!isnan(val) && val >= mn && val <= mx) {
                        float range = mx - mn;
                        if (range < best_range) { best_range = range; chosen_zone = z; }
                    }
                }
                // If no numeric match but there is a configured zone and value is NaN,
                // pick the first configured zone (fallback behavior).
                if (chosen_zone == -1 && isnan(val)) {
                    for (int z = 1; z <= 4; ++z) {
                        float mn = screen_configs[screen_idx].min[g][z];
                        float mx = screen_configs[screen_idx].max[g][z];
                        if (mn != mx) { chosen_zone = z; break; }
                    }
                }
                if (chosen_zone == -1) chosen_zone = 1;
                int current_state = chosen_zone - 1;

                // Make icon visible and apply style for the chosen zone when it changes
                if (current_state != last_zone_state[g]) {
                    lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
                    _ui_apply_icon_style(icon, screen_idx, g);
                    last_zone_state[g] = current_state;
                }

                // Check buzzer for Per-screen mode every cycle (not only on zone-change)
                bool buz_enabled = (screen_configs[screen_idx].buzzer[g][chosen_zone] != 0);
                unsigned long now = millis();
                bool cooldown_expired = (now - last_buzzer_time > ALERT_COOLDOWN_MS);
                if (buzzer_mode == 2 && buz_enabled && (first_run_buzzer || cooldown_expired)) {
                    if (now - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                        printf("[ALERT] screen=%d gauge=%d chosen_zone=%d val=%.2f buz_enabled=%d first_run=%d cooldown_expired=%d\n",
                               screen_idx, g, chosen_zone, val, (int)buz_enabled, (int)first_run_buzzer, (int)cooldown_expired);
                        last_alarm_log_time = now;
                    }
                    trigger_buzzer_alert();
                    last_buzzer_time = now;
                    first_run_buzzer = false;
                }
            }

            // If Global buzzer mode is enabled, scan all screens for any configured buzzer zones
            if (buzzer_mode == 1) {
                unsigned long now = millis();
                bool cooldown_expired = (now - last_buzzer_time > ALERT_COOLDOWN_MS);
                if (first_run_buzzer || cooldown_expired) {
                    bool fired = false;
                    for (int s = 0; s < NUM_SCREENS && !fired; ++s) {
                        // For each gauge on screen s
                        for (int g = 0; g < 2 && !fired; ++g) {
                            // Only check gauge zones for gauge-type screens
                            if (screen_configs[s].display_type != DISPLAY_TYPE_GAUGE &&
                                screen_configs[s].display_type != DISPLAY_TYPE_GAUGE_NUMBER) continue;
                            // Get runtime value for that screen/gauge
                            float rval = NAN;
                            switch (s+1) {
                                case 1: rval = (g==0) ? get_sensor_value(SCREEN1_RPM) : get_sensor_value(SCREEN1_COOLANT_TEMP); break;
                                case 2: rval = (g==0) ? get_sensor_value(SCREEN2_RPM) : get_sensor_value(SCREEN2_FUEL); break;
                                case 3: rval = (g==0) ? get_sensor_value(SCREEN3_COOLANT_TEMP) : get_sensor_value(SCREEN3_EXHAUST_TEMP); break;
                                case 4: rval = (g==0) ? get_sensor_value(SCREEN4_FUEL) : get_sensor_value(SCREEN4_COOLANT_TEMP); break;
                                case 5: rval = (g==0) ? get_sensor_value(SCREEN5_OIL_PRESSURE) : get_sensor_value(SCREEN5_COOLANT_TEMP); break;
                                default: rval = NAN; break;
                            }
                            int chosen_zone = -1;
                            float best_range = 1e30f;
                            for (int z = 1; z <= 4; ++z) {
                                float mn = screen_configs[s].min[g][z];
                                float mx = screen_configs[s].max[g][z];
                                if (mn == mx) continue;
                                if (!isnan(rval) && rval >= mn && rval <= mx) {
                                    float range = mx - mn;
                                    if (range < best_range) { best_range = range; chosen_zone = z; }
                                }
                            }
                            if (chosen_zone == -1 && isnan(rval)) {
                                for (int z = 1; z <= 4; ++z) {
                                    float mn = screen_configs[s].min[g][z];
                                    float mx = screen_configs[s].max[g][z];
                                    if (mn != mx) { chosen_zone = z; break; }
                                }
                            }
                            if (chosen_zone == -1) chosen_zone = 1;
                            bool buz_enabled = (screen_configs[s].buzzer[g][chosen_zone] != 0);
                            if (buz_enabled) {
                                if (now - last_alarm_log_time > ALARM_LOG_INTERVAL_MS) {
                                    printf("[ALERT-GLOBAL] screen=%d gauge=%d chosen_zone=%d val=%.2f buz_enabled=%d\n",
                                           s, g, chosen_zone, rval, (int)buz_enabled);
                                    last_alarm_log_time = now;
                                }
                                trigger_buzzer_alert();
                                last_buzzer_time = now;
                                first_run_buzzer = false;
                                fired = true;
                                break;
                            }
                        }
                        // Also check number display alarms for this screen
                        if (!fired && screen_configs[s].display_type == DISPLAY_TYPE_NUMBER) {
                            String npath = String(screen_configs[s].number_path);
                            if (npath.length() > 0) {
                                float nval = get_sensor_value_by_path(npath);
                                if (!isnan(nval)) {
                                    bool low_trip  = screen_configs[s].buzzer[0][1] && (nval < screen_configs[s].min[0][1]);
                                    bool high_trip = screen_configs[s].buzzer[0][2] && (nval > screen_configs[s].max[0][2]);
                                    if (low_trip || high_trip) {
                                        trigger_buzzer_alert();
                                        last_buzzer_time = now;
                                        first_run_buzzer = false;
                                        fired = true;
                                    }
                                }
                            }
                        }
                        // Check dual display alarms: top=g0 z1/2, bottom=g1 z1/2
                        if (!fired && screen_configs[s].display_type == DISPLAY_TYPE_DUAL) {
                            float dv[2] = {
                                get_sensor_value_by_path(String(screen_configs[s].dual_top_path)),
                                get_sensor_value_by_path(String(screen_configs[s].dual_bottom_path))
                            };
                            for (int ch = 0; ch < 2 && !fired; ch++) {
                                if (isnan(dv[ch])) continue;
                                bool lt = screen_configs[s].buzzer[ch][1] && (dv[ch] < screen_configs[s].min[ch][1]);
                                bool ht = screen_configs[s].buzzer[ch][2] && (dv[ch] > screen_configs[s].max[ch][2]);
                                if (lt || ht) {
                                    trigger_buzzer_alert(); last_buzzer_time = now;
                                    first_run_buzzer = false; fired = true;
                                }
                            }
                        }
                        // Check quad display alarms: TL=g0z1/2, TR=g0z3/4, BL=g1z1/2, BR=g1z3/4
                        if (!fired && screen_configs[s].display_type == DISPLAY_TYPE_QUAD) {
                            struct { const char* path; int g, zl, zh; } qd[4] = {
                                { screen_configs[s].quad_tl_path, 0, 1, 2 },
                                { screen_configs[s].quad_tr_path, 0, 3, 4 },
                                { screen_configs[s].quad_bl_path, 1, 1, 2 },
                                { screen_configs[s].quad_br_path, 1, 3, 4 },
                            };
                            for (int q = 0; q < 4 && !fired; q++) {
                                float qv = get_sensor_value_by_path(String(qd[q].path));
                                if (isnan(qv)) continue;
                                bool lt = screen_configs[s].buzzer[qd[q].g][qd[q].zl] && (qv < screen_configs[s].min[qd[q].g][qd[q].zl]);
                                bool ht = screen_configs[s].buzzer[qd[q].g][qd[q].zh] && (qv > screen_configs[s].max[qd[q].g][qd[q].zh]);
                                if (lt || ht) {
                                    trigger_buzzer_alert(); last_buzzer_time = now;
                                    first_run_buzzer = false; fired = true;
                                }
                            }
                        }
                        // Check gauge+number center alarm: min[1][1]/buzzer[1][1]=low, max[1][2]/buzzer[1][2]=high
                        if (!fired && screen_configs[s].display_type == DISPLAY_TYPE_GAUGE_NUMBER) {
                            String cpath = String(screen_configs[s].gauge_num_center_path);
                            if (cpath.length() > 0) {
                                float cv = get_sensor_value_by_path(cpath);
                                if (!isnan(cv)) {
                                    bool lt = screen_configs[s].buzzer[1][1] && (cv < screen_configs[s].min[1][1]);
                                    bool ht = screen_configs[s].buzzer[1][2] && (cv > screen_configs[s].max[1][2]);
                                    if (lt || ht) {
                                        trigger_buzzer_alert(); last_buzzer_time = now;
                                        first_run_buzzer = false; fired = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // Deferred LVGL rebuild: HTTP save handlers set this flag instead of calling
    // apply_all_screen_visuals() directly (which would race with DMA flush).
    // We consume it here, just before lv_timer_handler(), when LVGL is idle.
    //
    // IMPORTANT: DO NOT call apply_all_screen_visuals() here — that function
    // unconditionally marks ALL 5 screens for lazy-apply.  When only one
    // screen was saved we must honour the save handler's targeted flag so
    // unchanged screens are never rebuilt (each redundant rebuild leaks
    // ~500 KB of PSRAM in LVGL objects and eventually causes a crash).
    if (g_pending_visual_apply) {
        g_pending_visual_apply = false;
        int active = ui_get_current_screen() - 1; // 0-based
        Serial.printf("[LOOP] deferred apply, active screen=%d\n", active);
        Serial.flush();
        // Apply the active screen immediately if it was flagged.
        // Non-active flagged screens stay flagged; the lazy-apply in the
        // needle-update section will apply them when the user swipes over.
        if (active >= 0 && active < NUM_SCREENS && g_screens_need_apply[active]) {
            g_screens_need_apply[active] = false;
            apply_screen_visuals_for_one(active);
        }
        Serial.println("[LOOP] deferred apply complete");
        Serial.flush();
        // Don't resume WS here — the user is likely still editing the config
        // page and the next tab click would immediately re-pause, creating a
        // connect→disconnect→TIME_WAIT cycle that crashes the next fragment.
        // The 10-second idle watchdog below handles resuming WS after the user
        // stops accessing the config page.
    }

    Lvgl_Loop();
    brightness_nvs_flush();

    // WS / N2K idle watchdog: resume automatically once the config page has been
    // idle for 20 seconds.  The config page sends a keepalive ping every 8s,
    // so this only fires after the user has actually closed the page.
    {
        static unsigned long last_ws_watchdog = 0;
        unsigned long now_wd = millis();
        if (now_wd - last_ws_watchdog >= 2000UL) {
            last_ws_watchdog = now_wd;
            if (g_config_page_last_seen != 0
                    && (now_wd - g_config_page_last_seen) >= 20000UL) {
                Serial.printf("[CFG] Config page closed (idle >20s, iRAM=%u), resuming\n",
                              heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                g_config_page_last_seen = 0;
                if (is_signalk_ws_paused() && !g_signalk_ws_resume_pending)
                    resume_signalk_ws();
                resume_n2k_updates();
            }
        }
    }

    // Buzzer safety maintenance — V3 every 200 ms, V4 every 30 s.
    // V4: With Waveshare setup (dir=0xFF, all outputs), buzzer is controlled
    // via output register bit 6 (BEE_EN).  Periodically ensure it's LOW.
    {
        static unsigned long last_buz_maintain = 0;
        unsigned long now_m = millis();
        if (is_board_v4()) {
            // V4: unconditional write every 30 s (skip first 3 s to avoid startup beep)
            if (now_m > 3000 && (now_m - last_buz_maintain >= 30000)) {
                last_buz_maintain = now_m;
                Set_EXIO(PIN_BEE_EN, Low);
            }
        } else {
            if (now_m - last_buz_maintain >= 200) {
                last_buz_maintain = now_m;
                Set_EXIOS(Read_EXIOS(exio_output_reg()) & (uint8_t)~(1 << (EXIO_PIN6 - 1)));
                Mode_EXIOS(0x00);
            }
        }
    }

    // Small delay to prevent excessive loop iterations
    delay(1);
    yield();
}
