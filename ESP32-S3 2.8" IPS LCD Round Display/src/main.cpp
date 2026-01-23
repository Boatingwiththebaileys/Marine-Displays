#include <Preferences.h>
// Global test mode flag: disables live data updates when true
bool test_mode = false;
#include <Arduino.h>
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "LVGL_Driver.h"
#include "SD_Card.h"
#include "ui.h"
#include "ui_Settings.h"
#include "signalk_config.h"
#include "screen_config_c_api.h"
#include "sensESP_setup.h"
#include "gauge_config.h"
#include "needle_style.h"
#ifdef __cplusplus
extern "C" {
#endif
void show_fallback_error_screen_if_needed();
#ifdef __cplusplus
}
#endif
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rgb565_decoder.h"  // Custom decoder for binary RGB565 images

// External UI elements (per-screen icons are declared in ui_ScreenN.h via ui.h)

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
        auto_scroll_timer = lv_timer_create([](lv_timer_t *t){ (void)t; ui_next_screen(); }, (uint32_t)sec * 1000, NULL);
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
        lv_anim_set_time(&a, 300);  // 300ms smooth animation
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
        lv_anim_set_time(&a, 300);  // 300ms smooth animation
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
    lv_anim_set_time(&a, 300);
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

// Update both needles for the active screen using live Signal K sensor values
extern "C" void update_needles_for_screen(int screen_num) {
    // Index 1-5 correspond to Screen1..Screen5
    if (screen_num < 1 || screen_num > 5) return;
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
            // Debug output disabled for performance
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
            lv_anim_set_time(&a, 300);
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
            lv_anim_set_time(&a, 300);
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

void setup() {
        // test_nvs_minimal() removed during cleanup
    // Serial for debugging - with timeout
    Serial.setTxTimeoutMs(0);  // Non-blocking serial
    Serial.begin(115200);
    delay(500);
    
    Serial.println("\n\n=== ESP32 Round Display Starting ===");
    Serial.flush();
    
    // I2C and IO expander
    I2C_Init();
    delay(100);
    TCA9554PWR_Init(0x00);
    Set_EXIO(EXIO_PIN8, Low);    // Start with buzzer OFF
    Set_EXIO(EXIO_PIN3, Low);    // Keep other pins low
    Serial.println("I2C and IO expander initialized");
    Serial.flush();
    
    // LCD (now with reduced pixel clock + larger bounce buffer)
    LCD_Init();
    Serial.print("LCD initialized at ");
    Serial.print(ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ / 1000000);  // MHz
    Serial.println("MHz pixel clock");
    Serial.flush();
    
    // SD Card (must be initialized before LVGL to load images)
    SD_Init();
    Serial.println("SD card initialized");
    Serial.flush();
    
    // Load persisted preferences BEFORE initializing the UI so dynamic image paths
    // are available during screen construction.
    load_preferences();

    // Show fallback error screen if all configs are blank
    show_fallback_error_screen_if_needed();

    // LVGL
    Lvgl_Init();

    // Initialize RGB565 binary image decoder (fast loading, no PNG decode overhead)
    rgb565_decoder_init();
    Serial.println("RGB565 decoder initialized");
    Serial.flush();

    ui_init();  // Load SquareLine UI
    Serial.println("LVGL and UI initialized");
    Serial.flush();
    
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
    setup_sensESP();
    Serial.println("WiFi setup complete");
    Serial.flush();
    
    // Start Signal K only if server is actually configured
    Serial.println("Checking Signal K configuration...");
    Serial.flush();
    String sk_ip = get_signalk_server_ip();
    Serial.print("Signal K Server IP: '");
    Serial.print(sk_ip);
    Serial.println("'");
    Serial.flush();
    
    if (sk_ip.length() > 0 && is_wifi_connected()) {
        Serial.println("Starting Signal K...");
        Serial.flush();
        enable_signalk("", "", sk_ip.c_str(), get_signalk_server_port());
    } else {
        Serial.println("Signal K not configured yet");
        Serial.println("Connect to web UI to configure Signal K server");
        Serial.flush();
    }
    
    Serial.println("Display initialized with WiFi optimizations.");
    Serial.print("WiFi SSID: ");
    Serial.println(WiFi.SSID());
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Navigate to http://esp32-rounddisplay.local or check your router for device IP");
    Serial.flush();
}

void loop() {
    config_server.handleClient();
    // Use Signal K data instead of demo animation
    static int16_t needle_angle = 0;
    static int16_t lower_needle_angle = 0;
    static unsigned long last_needle_update = 0;
    
    // Switch to Signal K mode
    static bool use_demo_mode = false;
    
    // Check if in setup mode - use preview angles instead of Signal K data
    if (gauge_is_setup_mode()) {
        int16_t top_angle = gauge_get_preview_top_angle();
        int16_t bottom_angle = gauge_get_preview_bottom_angle();
        Serial.print("[DEBUG] Setup mode active. Preview angles: top=");
        Serial.print(top_angle);
        Serial.print(", bottom=");
        Serial.println(bottom_angle);
        rotate_needle(top_angle);
        rotate_lower_needle(bottom_angle);
    } else if (use_demo_mode) {
        needle_angle = (needle_angle + 1) % 360;
        lower_needle_angle = (lower_needle_angle + 2) % 360;
        
        rotate_needle(needle_angle);
        rotate_lower_needle(lower_needle_angle);
    } else {
        // Use values from Signal K (automatically updated by background task)
        // Update needles every 100ms for smooth operation
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
            }

            update_needles_for_screen(current_screen);
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
                    // Debug: log buzzer decision
                    printf("[ALERT] screen=%d gauge=%d chosen_zone=%d val=%.2f buz_enabled=%d first_run=%d cooldown_expired=%d\n",
                           screen_idx, g, chosen_zone, val, (int)buz_enabled, (int)first_run_buzzer, (int)cooldown_expired);
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
                                printf("[ALERT-GLOBAL] screen=%d gauge=%d chosen_zone=%d val=%.2f buz_enabled=%d\n",
                                       s, g, chosen_zone, rval, (int)buz_enabled);
                                trigger_buzzer_alert();
                                last_buzzer_time = now;
                                first_run_buzzer = false;
                                fired = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    Lvgl_Loop();

    // Small delay to prevent excessive loop iterations
    delay(1);
    yield();
}
