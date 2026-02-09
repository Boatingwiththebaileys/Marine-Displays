#include "ui_Settings.h"
#include "ui.h"
#include "Display_ST7701.h"
#include "TCA9554PWR.h"  // For buzzer control
#include <WiFi.h>

#include "sensESP_setup.h"
#include <Preferences.h>

extern lv_obj_t *ui_Screen1;  // Reference to main screen
extern lv_obj_t *ui_Screen2;
extern lv_obj_t *ui_Screen3;
extern lv_obj_t *ui_Screen4;
extern lv_obj_t *ui_Screen5;

// Get current screen number (1-5)
extern "C" int ui_get_current_screen(void);

// Track which screen opened settings so we can return to it
int previous_screen_before_settings = 1;

lv_obj_t *ui_Settings = NULL;
lv_obj_t *ui_SettingsPanel = NULL;
lv_obj_t *ui_IPLabel = NULL;
lv_obj_t *ui_BackButton = NULL;
lv_obj_t *ui_BuzzerSwitch = NULL;
lv_obj_t *ui_BuzzerLabel = NULL;
lv_obj_t *ui_AutoScrollDrop = NULL;
lv_obj_t *ui_AutoScrollLabel = NULL;
lv_obj_t *ui_BuzzerCooldownDrop = NULL;
lv_obj_t *ui_BuzzerCooldownLabel = NULL;

// Global buzzer mode: 0 = Off, 1 = Global, 2 = Per-screen
int buzzer_mode = 0;
uint16_t buzzer_cooldown_sec = 60; // default 60s

// Buzzer alert function - makes two beeps
extern "C" void trigger_buzzer_alert() {
    if (buzzer_mode == 0) return; // disabled
    printf("trigger_buzzer_alert() called, buzzer_mode=%d\n", buzzer_mode);
    // First beep
    printf("  -> beep 1 on\n");
    Set_EXIO(EXIO_PIN6, High);
    delay(100);
    Set_EXIO(EXIO_PIN6, Low);
    printf("  -> beep 1 off\n");
    delay(100);
    // Second beep
    printf("  -> beep 2 on\n");
    Set_EXIO(EXIO_PIN6, High);
    delay(100);
    Set_EXIO(EXIO_PIN6, Low);
    printf("  -> beep 2 off\n");
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

// Update IP address when screen is shown
extern "C" void update_ip_address(void)
{
    if (ui_IPLabel != NULL) {
        if (WiFi.status() == WL_CONNECTED) {
            String ip = WiFi.localIP().toString();
            lv_label_set_text(ui_IPLabel, ip.c_str());
        } else {
            lv_label_set_text(ui_IPLabel, "Not Connected");
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
            printf("SWIPE UP DETECTED - Returning to screen %d\n", previous_screen_before_settings);
            // Return to the screen that was active before settings opened
            lv_obj_t* target_screen = ui_Screen1;  // Default to Screen1
            switch(previous_screen_before_settings) {
                case 1: target_screen = ui_Screen1; break;
                case 2: target_screen = ui_Screen2; break;
                case 3: target_screen = ui_Screen3; break;
                case 4: target_screen = ui_Screen4; break;
                case 5: target_screen = ui_Screen5; break;
            }
            lv_scr_load_anim(target_screen, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
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
    
    // Update IP address when screen loads
    lv_obj_add_event_cb(ui_Settings, [](lv_event_t *e) { update_ip_address(); }, LV_EVENT_SCREEN_LOADED, NULL);
    
    printf("Settings swipe events registered\n");
    
    // Settings panel - circular for round display
    ui_SettingsPanel = lv_obj_create(ui_Settings);
    lv_obj_set_width(ui_SettingsPanel, 480);
    lv_obj_set_height(ui_SettingsPanel, 480);
    lv_obj_set_align(ui_SettingsPanel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_SettingsPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_SettingsPanel, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_width(ui_SettingsPanel, 0, 0);
    lv_obj_set_style_radius(ui_SettingsPanel, 0, 0);  // Square corners for square display
    
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
    
    // Buzzer label (static) and dropdown on same line
    ui_BuzzerLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_BuzzerLabel, "Buzzer:");
    lv_obj_set_style_text_color(ui_BuzzerLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BuzzerLabel, -70);
    lv_obj_set_y(ui_BuzzerLabel, -80);
    lv_obj_set_align(ui_BuzzerLabel, LV_ALIGN_CENTER);

    // Buzzer mode dropdown (Off / Global / Per-screen) inline with label
    ui_BuzzerSwitch = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerSwitch, "Off\nGlobal\nPer-screen");
    lv_obj_set_width(ui_BuzzerSwitch, 140);
    lv_obj_set_height(ui_BuzzerSwitch, 32);
    lv_obj_set_x(ui_BuzzerSwitch, 40);
    lv_obj_set_y(ui_BuzzerSwitch, -80);
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
    lv_obj_set_y(ui_BuzzerCooldownLabel, 60);
    lv_obj_set_align(ui_BuzzerCooldownLabel, LV_ALIGN_CENTER);

    ui_BuzzerCooldownDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerCooldownDrop, "Constant\n5s\n10s\n30s\n60s");
    lv_obj_set_width(ui_BuzzerCooldownDrop, 140);
    lv_obj_set_height(ui_BuzzerCooldownDrop, 32);
    lv_obj_set_x(ui_BuzzerCooldownDrop, 40);
    lv_obj_set_y(ui_BuzzerCooldownDrop, 60);
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
    lv_obj_set_y(ip_title, -120);
    lv_obj_set_align(ip_title, LV_ALIGN_CENTER);
    
    // IP Address value
    ui_IPLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_IPLabel, "Checking...");
    lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_x(ui_IPLabel, 40);
    lv_obj_set_y(ui_IPLabel, -120);
    lv_obj_set_align(ui_IPLabel, LV_ALIGN_CENTER);
    
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

    // Instruction text (moved below auto-scroll)
    lv_obj_t *instruction = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(instruction, "Swipe up to return");
    lv_obj_set_style_text_color(instruction, lv_color_hex(0x808080), 0);
    lv_obj_set_x(instruction, 0);
    lv_obj_set_y(instruction, 160);
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
