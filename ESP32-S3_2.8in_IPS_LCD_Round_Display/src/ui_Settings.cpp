#include "ui_Settings.h"
#include "ui.h"
#include "Display_ST7701.h"
#include "TCA9554PWR.h"  // For buzzer control
#include <WiFi.h>

#include "network_setup.h"

#include <Preferences.h>

extern lv_obj_t *ui_Screen1;  // Reference to main screen

lv_obj_t *ui_Settings = NULL;
lv_obj_t *ui_SettingsPanel = NULL;
lv_obj_t *ui_BrightnessSlider = NULL;
lv_obj_t *ui_BrightnessLabel = NULL;
lv_obj_t *ui_IPLabel = NULL;
lv_obj_t *ui_APCredsLabel = NULL;
lv_obj_t *ui_RSSITitle = NULL;
lv_obj_t *ui_RSSILabel = NULL;
lv_obj_t *ui_RSSIBar = NULL;
lv_obj_t *ui_BackButton = NULL;
lv_obj_t *ui_BuzzerSwitch = NULL;
lv_obj_t *ui_BuzzerLabel = NULL;
lv_obj_t *ui_AutoScrollDrop = NULL;
lv_obj_t *ui_AutoScrollLabel = NULL;
lv_obj_t *ui_BuzzerCooldownDrop = NULL;
lv_obj_t *ui_BuzzerCooldownLabel = NULL;


// Timer to periodically sync settings with values (in case web page changes them)
static lv_timer_t *settings_refresh_timer = NULL;

// Global buzzer mode: 0 = Off, 1 = Global, 2 = Per-screen
int buzzer_mode = 0;
uint16_t buzzer_cooldown_sec = 60; // default 60s

// Night mode state
bool night_mode_active = false;
uint8_t brightness_slider_pos = 75;  // 0-100 slider position (default ~55% hw brightness)
static lv_obj_t *night_overlay = NULL;

// Buzzer alert function - makes two beeps
extern "C" void trigger_buzzer_alert() {
    if (buzzer_mode == 0) return; // disabled
    printf("trigger_buzzer_alert() called, buzzer_mode=%d\n", buzzer_mode);
    // First beep
    printf("  -> beep 1 on\n");
    Set_EXIO(EXIO_PIN8, High);
    delay(100);
    Set_EXIO(EXIO_PIN8, Low);
    printf("  -> beep 1 off\n");
    delay(100);
    // Second beep
    printf("  -> beep 2 on\n");
    Set_EXIO(EXIO_PIN8, High);
    delay(100);
    Set_EXIO(EXIO_PIN8, Low);
    printf("  -> beep 2 off\n");
}

// Apply or remove the red night-mode overlay on lv_layer_top()
void apply_night_mode_overlay(bool enable)
{
    if (enable) {
        if (night_overlay == NULL) {
            night_overlay = lv_obj_create(lv_layer_top());
            lv_obj_remove_style_all(night_overlay);
            lv_obj_set_size(night_overlay, 480, 480);
            lv_obj_set_style_bg_color(night_overlay, lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_bg_opa(night_overlay, 128, 0);
            lv_obj_clear_flag(night_overlay, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_obj_clear_flag(night_overlay, LV_OBJ_FLAG_HIDDEN);
        night_mode_active = true;
    } else {
        if (night_overlay != NULL) {
            lv_obj_add_flag(night_overlay, LV_OBJ_FLAG_HIDDEN);
        }
        night_mode_active = false;
    }
}

// Compute hardware brightness from unified slider position and apply overlay
void apply_brightness_from_slider(uint8_t slider_pos)
{
    brightness_slider_pos = slider_pos;
    uint8_t hw;
    if (slider_pos >= 50) {
        // Normal mode: map 50-100 → 10-100% hardware brightness
        hw = (uint8_t)(10 + (slider_pos - 50) * 90 / 50);
        apply_night_mode_overlay(false);
    } else {
        // Night mode: map 0-49 → 1-10% hardware brightness
        hw = (uint8_t)(1 + slider_pos * 9 / 49);
        apply_night_mode_overlay(true);
    }
    LCD_Backlight = hw;
    Set_Backlight(hw);
}

// Event handler for brightness slider
static void brightness_slider_event_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);

    apply_brightness_from_slider((uint8_t)value);

    // Update slider indicator color and label based on mode
    if (value >= 50) {
        lv_obj_set_style_bg_color(slider, lv_color_hex(0x00A8FF), LV_PART_INDICATOR);
        lv_label_set_text_fmt(ui_BrightnessLabel, "Brightness: %d%%", (int)LCD_Backlight);
    } else {
        lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
        lv_label_set_text_fmt(ui_BrightnessLabel, "Night: %d%%", (int)LCD_Backlight);
    }

    // Persist slider position
    Preferences p;
    if (p.begin("settings", false)) {
        p.putUShort("bright_pos", (uint16_t)value);
        p.end();
    }
}

// Event handler for buzzer dropdown
static void buzzer_switch_event_cb(lv_event_t *e)
{
    lv_obj_t * dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    // Map: 0=Off, 1=Global, 2=Per-screen
    buzzer_mode = (int)sel;
    // Keep the label static; dropdown shows the current mode beside it.
    if (buzzer_mode == 1) printf("Buzzer mode: Global\n");
    else if (buzzer_mode == 2) printf("Buzzer mode: Per-screen\n");
    else printf("Buzzer mode: Off\n");

    // Persist buzzer mode to Preferences
    Preferences p;
    if (p.begin("settings", false)) {
        p.putUShort("buzzer_mode", (uint16_t)buzzer_mode);
        p.end();
    }
}

// Update IP address and RSSI when screen is shown
extern "C" void update_ip_address(void)
{
    bool connected = (WiFi.status() == WL_CONNECTED);
    IPAddress apIP = WiFi.softAPIP();
    bool in_ap_mode = !connected && apIP != INADDR_NONE && apIP != IPAddress(0, 0, 0, 0);

    if (ui_IPLabel != NULL) {
        if (connected) {
            String ip = WiFi.localIP().toString();
            lv_label_set_text(ui_IPLabel, ip.c_str());
            lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0x00FF00), 0);
        } else if (in_ap_mode) {
            String apStr = "AP: " + apIP.toString();
            lv_label_set_text(ui_IPLabel, apStr.c_str());
            lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0xFFAA00), 0);
        } else {
            lv_label_set_text(ui_IPLabel, "Not Connected");
            lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0x808080), 0);
        }
    }

    // In AP mode: show SSID/password, hide RSSI. When connected: show RSSI, hide creds.
    if (ui_APCredsLabel != NULL) {
        if (in_ap_mode) {
            lv_label_set_text(ui_APCredsLabel, "SSID: ESP32-RoundDisplay\nPass: 12345678");
            lv_obj_clear_flag(ui_APCredsLabel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui_APCredsLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui_RSSIBar != NULL) {
        if (in_ap_mode) lv_obj_add_flag(ui_RSSIBar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(ui_RSSIBar, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_RSSITitle != NULL) {
        if (in_ap_mode) lv_obj_add_flag(ui_RSSITitle, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(ui_RSSITitle, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_RSSILabel != NULL) {
        if (in_ap_mode) {
            lv_obj_add_flag(ui_RSSILabel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(ui_RSSILabel, LV_OBJ_FLAG_HIDDEN);
            if (connected) {
                int rssi = WiFi.RSSI();
                int pct = constrain(2 * (rssi + 100), 0, 100);
                lv_bar_set_value(ui_RSSIBar, pct, LV_ANIM_ON);
                lv_color_t bar_color;
                if (pct > 60) bar_color = lv_color_hex(0x22AA22);
                else if (pct > 30) bar_color = lv_color_hex(0xDDAA22);
                else bar_color = lv_color_hex(0xDD2222);
                lv_obj_set_style_bg_color(ui_RSSIBar, bar_color, LV_PART_INDICATOR);
                char buf[24];
                snprintf(buf, sizeof(buf), "%d dBm (%d%%)", rssi, pct);
                lv_label_set_text(ui_RSSILabel, buf);
                lv_obj_set_style_text_color(ui_RSSILabel, bar_color, 0);
            } else {
                lv_bar_set_value(ui_RSSIBar, 0, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(ui_RSSIBar, lv_color_hex(0x808080), LV_PART_INDICATOR);
                lv_label_set_text(ui_RSSILabel, "No Signal");
                lv_obj_set_style_text_color(ui_RSSILabel, lv_color_hex(0x808080), 0);
            }
        }
    }
}

// Update all settings dropdowns to reflect current values
// (in case they were changed via web interface while Settings screen is open)
extern "C" void update_settings_values(void)
{
    // Update IP address
    update_ip_address();
    
    // Update buzzer mode dropdown
    if (ui_BuzzerSwitch != NULL) {
        lv_dropdown_set_selected(ui_BuzzerSwitch, (uint16_t)buzzer_mode);
    }
    
    // Update buzzer cooldown dropdown
    if (ui_BuzzerCooldownDrop != NULL) {
        uint16_t sel = 0;
        if (buzzer_cooldown_sec == 10) sel = 0;
        else if (buzzer_cooldown_sec == 30) sel = 1;
        else if (buzzer_cooldown_sec == 60) sel = 2;
        else if (buzzer_cooldown_sec == 120) sel = 3;
        else if (buzzer_cooldown_sec == 300) sel = 4;
        lv_dropdown_set_selected(ui_BuzzerCooldownDrop, sel);
    }
    
    // Update auto-scroll dropdown
    if (ui_AutoScrollDrop != NULL) {
        extern uint16_t auto_scroll_sec;
        uint16_t sel = 0;
        if (auto_scroll_sec == 5) sel = 1;
        else if (auto_scroll_sec == 10) sel = 2;
        else if (auto_scroll_sec == 30) sel = 3;
        lv_dropdown_set_selected(ui_AutoScrollDrop, sel);
    }

    // Sync brightness slider position and mode colors
    if (ui_BrightnessSlider != NULL) {
        lv_slider_set_value(ui_BrightnessSlider, brightness_slider_pos, LV_ANIM_OFF);
        if (brightness_slider_pos >= 50) {
            lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0x00A8FF), LV_PART_INDICATOR);
            lv_label_set_text_fmt(ui_BrightnessLabel, "Brightness: %d%%", (int)LCD_Backlight);
        } else {
            lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0xFF0000), LV_PART_INDICATOR);
            lv_label_set_text_fmt(ui_BrightnessLabel, "Night: %d%%", (int)LCD_Backlight);
        }
    }

}

// Event handler for back button (swipe up)
static void back_button_event_cb(lv_event_t *e)
{
    lv_scr_load_anim(ui_Screen1, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
}

// Event handler for swipe up gesture - manual detection
static int16_t settings_swipe_start_x = 0;
static int16_t settings_swipe_start_y = 0;
static bool settings_swipe_in_progress = false;

static void swipe_up_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_get_act(), &point);
        settings_swipe_start_x = point.x;
        settings_swipe_start_y = point.y;
        settings_swipe_in_progress = true;
    }
    else if (code == LV_EVENT_RELEASED && settings_swipe_in_progress) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_get_act(), &point);
        int16_t end_x = point.x;
        int16_t end_y = point.y;
        
        int16_t delta_x = end_x - settings_swipe_start_x;
        int16_t delta_y = end_y - settings_swipe_start_y;
        
        // Check for upward swipe (delta_y < -50 and mostly vertical)
        if (delta_y < -50 && abs(delta_y) > abs(delta_x)) {
            printf("SWIPE UP DETECTED - Returning to Main\n");
            lv_scr_load_anim(ui_Screen1, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
        }
        
        settings_swipe_in_progress = false;
    }
}

extern "C" void ui_Settings_screen_init(void)
{
    ui_Settings = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Settings, lv_color_hex(0x000000), 0);
    
    // Add swipe detection to background (won't interfere with child widgets)
    lv_obj_add_event_cb(ui_Settings, swipe_up_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_Settings, swipe_up_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui_Settings, swipe_up_event_cb, LV_EVENT_RELEASED, NULL);
    
    // Update all settings values when screen loads (to sync with any web changes)
    lv_obj_add_event_cb(ui_Settings, [](lv_event_t *e) {
        update_settings_values();
        // Start periodic refresh timer (every 2 seconds) while Settings screen is visible
        if (settings_refresh_timer == NULL) {
            settings_refresh_timer = lv_timer_create([](lv_timer_t *t) {
                update_settings_values();
            }, 2000, NULL);
        }
    }, LV_EVENT_SCREEN_LOADED, NULL);
    
    // Stop refresh timer when leaving Settings screen
    lv_obj_add_event_cb(ui_Settings, [](lv_event_t *e) {
        if (settings_refresh_timer != NULL) {
            lv_timer_del(settings_refresh_timer);
            settings_refresh_timer = NULL;
        }
    }, LV_EVENT_SCREEN_UNLOADED, NULL);
    
    printf("Settings swipe events registered\n");
    
    // Settings panel - circular for round display
    ui_SettingsPanel = lv_obj_create(ui_Settings);
    lv_obj_set_width(ui_SettingsPanel, 460);
    lv_obj_set_height(ui_SettingsPanel, 460);
    lv_obj_set_align(ui_SettingsPanel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_SettingsPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_SettingsPanel, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(ui_SettingsPanel, 0, 0);
    lv_obj_set_style_radius(ui_SettingsPanel, 230, 0);  // Perfect circle (radius = width/2)
    
    // Add swipe detection to panel as well
    lv_obj_add_event_cb(ui_SettingsPanel, swipe_up_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(ui_SettingsPanel, swipe_up_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui_SettingsPanel, swipe_up_event_cb, LV_EVENT_RELEASED, NULL);
    
    // Title
    lv_obj_t *title = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(title, "SETTINGS");  // All caps for emphasis
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(title, 0);
    lv_obj_set_y(title, -180);
    lv_obj_set_align(title, LV_ALIGN_CENTER);
    // Make title slightly larger if built-in Montserrat fonts are available
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
#else
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
#endif
    
    // Brightness label
    ui_BrightnessLabel = lv_label_create(ui_SettingsPanel);
    if (brightness_slider_pos >= 50)
        lv_label_set_text_fmt(ui_BrightnessLabel, "Brightness: %d%%", (int)LCD_Backlight);
    else
        lv_label_set_text_fmt(ui_BrightnessLabel, "Night: %d%%", (int)LCD_Backlight);
    lv_obj_set_style_text_color(ui_BrightnessLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BrightnessLabel, 0);
    lv_obj_set_y(ui_BrightnessLabel, -70);
    lv_obj_set_align(ui_BrightnessLabel, LV_ALIGN_CENTER);
    
    // Brightness slider (0-100: top half normal, bottom half night mode)
    ui_BrightnessSlider = lv_slider_create(ui_SettingsPanel);
    lv_slider_set_range(ui_BrightnessSlider, 0, 100);
    lv_slider_set_value(ui_BrightnessSlider, brightness_slider_pos, LV_ANIM_OFF);
    lv_obj_set_width(ui_BrightnessSlider, 300);
    lv_obj_set_height(ui_BrightnessSlider, 20);
    lv_obj_set_x(ui_BrightnessSlider, 0);
    lv_obj_set_y(ui_BrightnessSlider, -20);
    lv_obj_set_align(ui_BrightnessSlider, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_BrightnessSlider,
        brightness_slider_pos >= 50 ? lv_color_hex(0x00A8FF) : lv_color_hex(0xFF0000),
        LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_BrightnessSlider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_event_cb(ui_BrightnessSlider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Buzzer label (static) and dropdown on same line
    ui_BuzzerLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_BuzzerLabel, "Buzzer:");
    lv_obj_set_style_text_color(ui_BuzzerLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BuzzerLabel, -70);
    lv_obj_set_y(ui_BuzzerLabel, 30);
    lv_obj_set_align(ui_BuzzerLabel, LV_ALIGN_CENTER);

    // Buzzer mode dropdown (Off / Global / Per-screen) inline with label
    ui_BuzzerSwitch = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerSwitch, "Off\nGlobal\nPer-screen");
    lv_obj_set_width(ui_BuzzerSwitch, 140);
    lv_obj_set_height(ui_BuzzerSwitch, 32);
    lv_obj_set_x(ui_BuzzerSwitch, 40);
    lv_obj_set_y(ui_BuzzerSwitch, 30);
    lv_obj_set_align(ui_BuzzerSwitch, LV_ALIGN_CENTER);
    lv_obj_add_event_cb(ui_BuzzerSwitch, buzzer_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Load persisted buzzer mode and cooldown from Preferences (settings namespace)
    Preferences p;
    if (p.begin("settings", true)) {
        buzzer_mode = p.getUShort("buzzer_mode", buzzer_mode);
        buzzer_cooldown_sec = p.getUShort("buzzer_cooldown", buzzer_cooldown_sec);
        p.end();
    }

    // Apply loaded state to dropdown (label is static)
    lv_dropdown_set_selected(ui_BuzzerSwitch, (uint16_t)buzzer_mode);

    // Buzzer cooldown label + dropdown (inline-ish below buzzer control)
    ui_BuzzerCooldownLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_BuzzerCooldownLabel, "Cooldown Timer:");
    lv_obj_set_style_text_color(ui_BuzzerCooldownLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BuzzerCooldownLabel, -100);
    lv_obj_set_y(ui_BuzzerCooldownLabel, 70);
    lv_obj_set_align(ui_BuzzerCooldownLabel, LV_ALIGN_CENTER);

    ui_BuzzerCooldownDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerCooldownDrop, "Constant\n5s\n10s\n30s\n60s");
    lv_obj_set_width(ui_BuzzerCooldownDrop, 140);
    lv_obj_set_height(ui_BuzzerCooldownDrop, 32);
    lv_obj_set_x(ui_BuzzerCooldownDrop, 40);
    lv_obj_set_y(ui_BuzzerCooldownDrop, 70);
    lv_obj_set_align(ui_BuzzerCooldownDrop, LV_ALIGN_CENTER);

    // Map persisted seconds to dropdown index
    uint16_t sel = 4; // default 60s
    if (buzzer_cooldown_sec == 0) sel = 0;
    else if (buzzer_cooldown_sec == 5) sel = 1;
    else if (buzzer_cooldown_sec == 10) sel = 2;
    else if (buzzer_cooldown_sec == 30) sel = 3;
    else sel = 4;
    lv_dropdown_set_selected(ui_BuzzerCooldownDrop, sel);

    // Event handler to persist and apply cooldown
    lv_obj_add_event_cb(ui_BuzzerCooldownDrop, [](lv_event_t *e){
        lv_obj_t *dd = lv_event_get_target(e);
        uint16_t idx = lv_dropdown_get_selected(dd);
        uint16_t sec = 60;
        if (idx == 0) sec = 0;
        else if (idx == 1) sec = 5;
        else if (idx == 2) sec = 10;
        else if (idx == 3) sec = 30;
        else if (idx == 4) sec = 60;
        buzzer_cooldown_sec = sec;
        Preferences p2;
        if (p2.begin("settings", false)) {
            p2.putUShort("buzzer_cooldown", buzzer_cooldown_sec);
            p2.end();
        }
        // Signal main loop to re-evaluate buzzer firing immediately
        extern bool first_run_buzzer; // declared in ui_Settings.h
        first_run_buzzer = true;
    }, LV_EVENT_VALUE_CHANGED, NULL);
    
    // IP Address label title
    lv_obj_t *ip_title = lv_label_create(ui_SettingsPanel);
        lv_label_set_text(ip_title, "IP Address:     ");
    lv_obj_set_style_text_color(ip_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ip_title, -40);
    lv_obj_set_y(ip_title, -130);
    lv_obj_set_align(ip_title, LV_ALIGN_CENTER);
    
    // IP Address value
    ui_IPLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_IPLabel, "Checking...");
    lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_x(ui_IPLabel, 40);
    lv_obj_set_y(ui_IPLabel, -130);
    lv_obj_set_align(ui_IPLabel, LV_ALIGN_CENTER);

    // AP credentials label (shown instead of RSSI when in AP mode)
    ui_APCredsLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_APCredsLabel, "SSID: ESP32-RoundDisplay\nPass: 12345678");
    lv_obj_set_style_text_color(ui_APCredsLabel, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(ui_APCredsLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_x(ui_APCredsLabel, 0);
    lv_obj_set_y(ui_APCredsLabel, -100);
    lv_obj_set_align(ui_APCredsLabel, LV_ALIGN_CENTER);
    lv_label_set_long_mode(ui_APCredsLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui_APCredsLabel, 220);
    lv_obj_add_flag(ui_APCredsLabel, LV_OBJ_FLAG_HIDDEN);  // hidden by default; shown in AP mode
    
    // WiFi Signal Strength row
    ui_RSSITitle = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_RSSITitle, "WiFi Signal:");
    lv_obj_set_style_text_color(ui_RSSITitle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_RSSITitle, -90);
    lv_obj_set_y(ui_RSSITitle, -100);
    lv_obj_set_align(ui_RSSITitle, LV_ALIGN_CENTER);
    
    // RSSI bar
    ui_RSSIBar = lv_bar_create(ui_SettingsPanel);
    lv_obj_set_width(ui_RSSIBar, 100);
    lv_obj_set_height(ui_RSSIBar, 12);
    lv_obj_set_x(ui_RSSIBar, 20);
    lv_obj_set_y(ui_RSSIBar, -100);
    lv_obj_set_align(ui_RSSIBar, LV_ALIGN_CENTER);
    lv_bar_set_range(ui_RSSIBar, 0, 100);
    lv_bar_set_value(ui_RSSIBar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui_RSSIBar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_RSSIBar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_RSSIBar, lv_color_hex(0x22AA22), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ui_RSSIBar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_RSSIBar, 4, LV_PART_INDICATOR);
    
    // RSSI text label (e.g. "-65 dBm (70%)")
    ui_RSSILabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_RSSILabel, "--");
    lv_obj_set_style_text_color(ui_RSSILabel, lv_color_hex(0x808080), 0);
    lv_obj_set_x(ui_RSSILabel, 125);
    lv_obj_set_y(ui_RSSILabel, -100);
    lv_obj_set_align(ui_RSSILabel, LV_ALIGN_CENTER);
    
    // Auto-scroll dropdown (placed above the instruction)
    ui_AutoScrollLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_AutoScrollLabel, "Auto-scroll:");
    lv_obj_set_style_text_color(ui_AutoScrollLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_AutoScrollLabel, -80);
    lv_obj_set_y(ui_AutoScrollLabel, 110);
    lv_obj_set_align(ui_AutoScrollLabel, LV_ALIGN_CENTER);

    ui_AutoScrollDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_AutoScrollDrop, "Off\n5s\n10s\n30s");
    lv_obj_set_width(ui_AutoScrollDrop, 140);
    lv_obj_set_height(ui_AutoScrollDrop, 32);
    lv_obj_set_x(ui_AutoScrollDrop, 40);
    lv_obj_set_y(ui_AutoScrollDrop, 110);
    lv_obj_set_align(ui_AutoScrollDrop, LV_ALIGN_CENTER);

    // Instruction text
    lv_obj_t *instruction = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(instruction, "Swipe up to return");
    lv_obj_set_style_text_color(instruction, lv_color_hex(0x808080), 0);
    lv_obj_set_x(instruction, 0);
    lv_obj_set_y(instruction, 150);
    lv_obj_set_align(instruction, LV_ALIGN_CENTER);

    // Set current selection from persisted value
    extern uint16_t auto_scroll_sec;
    sel = 0;
    if (auto_scroll_sec == 5) sel = 1;
    else if (auto_scroll_sec == 10) sel = 2;
    else if (auto_scroll_sec == 30) sel = 3;
    lv_dropdown_set_selected(ui_AutoScrollDrop, sel);

    // Event handler: persist and apply
    lv_obj_add_event_cb(ui_AutoScrollDrop, [](lv_event_t *e){
        lv_obj_t *dd = lv_event_get_target(e);
        uint16_t idx = lv_dropdown_get_selected(dd);
        uint16_t sec = 0;
        if (idx == 1) sec = 5;
        else if (idx == 2) sec = 10;
        else if (idx == 3) sec = 30;
        // Persist to NVS
        Preferences p;
        if (p.begin("settings", false)) {
            p.putUShort("auto_scroll", sec);
            p.end();
        }
        // Apply immediately
        set_auto_scroll_interval(sec);
    }, LV_EVENT_VALUE_CHANGED, NULL);
}
