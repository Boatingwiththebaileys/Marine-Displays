#include "ui_Settings.h"
#include "ui.h"
#include "Display_ST7701.h"
#include "TCA9554PWR.h"  // For buzzer control
#include <WiFi.h>

#include "network_setup.h"

#include <Preferences.h>

static uint8_t v4_slider_to_pwm(uint16_t slider_val);

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
static lv_obj_t *ui_ScreenOffDrop = NULL;
static lv_obj_t *ui_ScreenOffLabel = NULL;

// Timer to periodically sync settings with values (in case web page changes them)
static lv_timer_t *settings_refresh_timer = NULL;

// Global buzzer mode: 0 = Off, 1 = Global, 2 = Per-screen
int buzzer_mode = 0;
uint16_t buzzer_cooldown_sec = 60; // default 60s

// Brightness level: 0=Normal, 1=Dim, 2=Night, 3=Night+
uint8_t brightness_level = 0;
static lv_obj_t *night_overlays[6] = {NULL}; // 0-4 = Screen1-5, 5 = Settings
static lv_obj_t *ui_BrightnessDrop = NULL;
static lv_obj_t *ui_BrightnessLevelLabel = NULL;
static lv_obj_t *ui_BrightnessSliderWidget = NULL;  // brightness slider (both V3 and V4)
static lv_obj_t *ui_BrightnessPctLabel = NULL;       // value label (V4: %, V3: mode name)


// Buzzer alert function.
// Circuit: PIN6 LOW -> Q1 off -> Q7 on -> buzzer ON  (active-LOW via NPN pair)
//          PIN6 HIGH -> Q1 on -> Q7 off -> buzzer OFF
// When TCA9554 CONFIG register resets to 0xFF (all inputs), PIN6 becomes high-Z.
// R18 (4.7k) then pulls Q1 base LOW -> Q7 stays ON -> buzzer sounds continuously.
// Fix: always force CONFIG=0x00 (all outputs) before AND after every beep.
extern "C" void trigger_buzzer_alert() {
    if (buzzer_mode == 0) return;
    printf("trigger_buzzer_alert() called, buzzer_mode=%d\n", buzzer_mode);

    if (is_board_v4()) {
      // V4 (CH32V003): BEE_EN = EXIO6/bit6 = PIN_BEE_EN (pin 7).
      // Direction is 0xFF (all outputs). Write full register values directly
      // (not shadow-based read-modify-write) so shadow desync can't silence us.
      // Re-assert DIR=0xFF first in case it was corrupted by io_expander library.
      printf("[BUZ] v4 beep: dir_shadow=0x%02x out_shadow=0x%02x\n",
             ch32v003_get_dir_shadow(), ch32v003_get_out_shadow());
      I2C_Write_EXIO(CH32V003_DIR_REG, CH32V003_DIR_ALL_OUT);    // ensure DIR=0xFF
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_BUZZER);  // 0x7A: BEE_EN HIGH
      ets_delay_us(200000);
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL);  // 0x3A: BEE_EN LOW
      ets_delay_us(100000);
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_BUZZER);  // 0x7A: BEE_EN HIGH
      ets_delay_us(200000);
      I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL);  // 0x3A: BEE_EN LOW
      printf("[BUZ] v4 beep done\n");
    } else {
      // V3: buzzer on EXIO_PIN6 (bit5)
      Set_EXIOS(Read_EXIOS(exio_output_reg()) & (uint8_t)~(1 << (EXIO_PIN6 - 1)));
      Mode_EXIOS(0x00);
      Set_EXIO(EXIO_PIN6, High);
      ets_delay_us(200000);
      Set_EXIO(EXIO_PIN6, Low);
      ets_delay_us(100000);
      Set_EXIO(EXIO_PIN6, High);
      ets_delay_us(200000);
      Set_EXIO(EXIO_PIN6, Low);
      Mode_EXIOS(0x00);
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
            lv_label_set_text(ui_APCredsLabel, "SSID: ESP32-SquareDisplay\nPass: 12345678");
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
            // Color the bar based on signal quality
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
    
    // Update brightness slider
    if (ui_BrightnessSliderWidget != NULL) {
        lv_slider_set_value(ui_BrightnessSliderWidget, brightness_level, LV_ANIM_OFF);
    }
    if (ui_BrightnessPctLabel != NULL) {
        if (is_board_v4()) {
            uint16_t sv = brightness_level;
            uint8_t pwm = v4_slider_to_pwm(sv);
            if (sv >= 110) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", pwm);
                lv_label_set_text(ui_BrightnessPctLabel, buf);
            } else {
                char buf[12];
                snprintf(buf, sizeof(buf), "R:%d%%", pwm);
                lv_label_set_text(ui_BrightnessPctLabel, buf);
            }
        } else {
            static const char *slabels[] = {"Normal", "Dim", "Night", "Night+"};
            lv_label_set_text(ui_BrightnessPctLabel, slabels[brightness_level < 4 ? brightness_level : 0]);
        }
    }

    // Update screen off timeout dropdown
    if (ui_ScreenOffDrop != NULL) {
        uint16_t sel = 0;
        if (screen_off_timeout_min == 1) sel = 1;
        else if (screen_off_timeout_min == 5) sel = 2;
        else if (screen_off_timeout_min == 10) sel = 3;
        else if (screen_off_timeout_min == 30) sel = 4;
        lv_dropdown_set_selected(ui_ScreenOffDrop, sel);
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

// ── Brightness overlay management ─────────────────────────────────────
// Creates a full-screen semi-transparent overlay on each screen.
// Overlays are non-clickable so touch events pass through to widgets below.
static lv_obj_t* create_night_overlay(lv_obj_t *parent) {
    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 480, 480);
    lv_obj_set_align(overlay, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |
                                LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);  // hidden by default
    lv_obj_move_foreground(overlay);
    return overlay;
}

// Apply color and opacity to an overlay based on brightness level
static void apply_overlay_style(lv_obj_t *overlay, uint8_t level) {
    switch (level) {
        case 1: // Dim: dark overlay, no red tint
            lv_obj_set_style_bg_color(overlay, lv_color_make(0, 0, 0), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
            break;
        case 2: // Night: dark red
            lv_obj_set_style_bg_color(overlay, lv_color_make(40, 0, 0), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
            break;
        case 3: // Night+: darker red
            lv_obj_set_style_bg_color(overlay, lv_color_make(30, 0, 0), 0);
            lv_obj_set_style_bg_opa(overlay, LV_OPA_80, 0);
            break;
        default: // Normal: hidden
            break;
    }
}

extern "C" void night_mode_init_overlays(void) {
    lv_obj_t *screens[6] = {ui_Screen1, ui_Screen2, ui_Screen3,
                             ui_Screen4, ui_Screen5, ui_Settings};
    for (int i = 0; i < 6; i++) {
        if (screens[i] && !night_overlays[i]) {
            night_overlays[i] = create_night_overlay(screens[i]);
        }
    }
    if (brightness_level > 0) {
        set_brightness_level(brightness_level);
    }
}

// Non-linear slider-to-PWM mapping for V4.
// Quadratic curve gives more slider resolution at lower brightness.
static uint8_t v4_slider_to_pwm(uint16_t slider_val) {
    if (slider_val >= 110) {
        // Normal range: slider 110-200 -> PWM 10-100%
        float frac = (float)(slider_val - 110) / 90.0f;
        return (uint8_t)(10 + (int)(frac * frac * 90.0f));
    } else {
        // Red range: slider 10-109 -> PWM 10-100%
        float frac = (float)(slider_val - 10) / 99.0f;
        return (uint8_t)(10 + (int)(frac * frac * 90.0f));
    }
}

// Deferred NVS save for brightness — avoids dozens of flash writes while sliding
static uint8_t  g_brightness_pending = 0;
static bool     g_brightness_dirty   = false;
static uint32_t g_brightness_last_change_ms = 0;
static const uint32_t BRIGHTNESS_NVS_DEFER_MS = 1000; // save 1s after last change

// Call from loop() or an LVGL timer to flush deferred brightness save
extern "C" void brightness_nvs_flush(void) {
    if (g_brightness_dirty &&
        (millis() - g_brightness_last_change_ms) >= BRIGHTNESS_NVS_DEFER_MS) {
        g_brightness_dirty = false;
        if (is_board_v4()) {
            // Apply final PWM value directly (don't call set_brightness_level
            // which would re-dirty the flag and cause an infinite save loop)
            uint16_t sv = g_brightness_pending;
            uint8_t pwm = v4_slider_to_pwm(sv);
            extern void Set_Backlight(uint8_t Light);
            Set_Backlight(pwm);
            // Persist raw slider value (10-200) as UShort
            Preferences pn;
            if (pn.begin("settings", false)) {
                pn.putUShort("brightness_v4", (uint16_t)g_brightness_pending);
                pn.end();
            }
        } else {
            Preferences pn;
            if (pn.begin("settings", false)) {
                pn.putUChar("brightness_lv", g_brightness_pending);
                pn.end();
            }
        }
        Serial.printf("[BRIGHT] NVS saved: %d\n", g_brightness_pending);
    }
}

// Throttle V4 I2C PWM writes to avoid corrupting CH32V003 expander state
static uint32_t g_last_pwm_write_ms = 0;
static const uint32_t PWM_WRITE_MIN_INTERVAL_MS = 50;

extern "C" void set_brightness_level(uint8_t level) {
    brightness_level = level;
    if (is_board_v4()) {
        // V4 unified slider 10-200:
        //   200→110: PWM brightness (level-100)% with no overlay
        //   109→10:  red overlay mode, PWM = level%
        uint16_t slider_val = level; // raw slider value 10-200
        if (slider_val > 200) slider_val = 200;
        if (slider_val < 10) slider_val = 10;
        brightness_level = (uint8_t)slider_val;

        bool red_mode = (slider_val < 110);
        uint8_t pwm_pct = v4_slider_to_pwm(slider_val);

        // Apply PWM backlight (throttled)
        uint32_t now = millis();
        if ((now - g_last_pwm_write_ms) >= PWM_WRITE_MIN_INTERVAL_MS) {
            g_last_pwm_write_ms = now;
            extern void Set_Backlight(uint8_t Light);
            Set_Backlight(pwm_pct);
        }

        // Apply/remove red overlay
        if (red_mode) {
            // Overlay opacity: slider 109→10 → light to heavy red
            // Map slider_val 10-109 to opacity 30-230
            uint8_t opa = (uint8_t)(230 - ((slider_val - 10) * 200 / 99));
            for (int i = 0; i < 6; i++) {
                if (night_overlays[i]) {
                    lv_obj_set_style_bg_color(night_overlays[i], lv_color_make(40, 0, 0), 0);
                    lv_obj_set_style_bg_opa(night_overlays[i], opa, 0);
                    lv_obj_clear_flag(night_overlays[i], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_move_foreground(night_overlays[i]);
                }
            }
        } else {
            // Hide red overlays
            for (int i = 0; i < 6; i++) {
                if (night_overlays[i]) {
                    lv_obj_add_flag(night_overlays[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        // Update slider widget if it exists
        if (ui_BrightnessSliderWidget) {
            lv_slider_set_value(ui_BrightnessSliderWidget, slider_val, LV_ANIM_OFF);
        }
        if (ui_BrightnessPctLabel) {
            if (!red_mode) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d%%", pwm_pct);
                lv_label_set_text(ui_BrightnessPctLabel, buf);
            } else {
                char buf[12];
                snprintf(buf, sizeof(buf), "R:%d%%", pwm_pct);
                lv_label_set_text(ui_BrightnessPctLabel, buf);
            }
        }
        // Defer NVS persist
        g_brightness_pending = brightness_level;
        g_brightness_dirty = true;
        g_brightness_last_change_ms = millis();
    } else {
        // V3: use overlay-based 4-step dimming (level 0-3)
        if (level > 3) level = 0;
        brightness_level = level;
        for (int i = 0; i < 6; i++) {
            if (night_overlays[i]) {
                if (level > 0) {
                    apply_overlay_style(night_overlays[i], level);
                    lv_obj_clear_flag(night_overlays[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(night_overlays[i], LV_OBJ_FLAG_HIDDEN);
                }
                lv_obj_move_foreground(night_overlays[i]);
            }
        }
        // Update slider widget if it exists
        if (ui_BrightnessSliderWidget) {
            lv_slider_set_value(ui_BrightnessSliderWidget, level, LV_ANIM_OFF);
        }
        if (ui_BrightnessPctLabel) {
            static const char *slabels[] = {"Normal", "Dim", "Night", "Night+"};
            lv_label_set_text(ui_BrightnessPctLabel, slabels[level < 4 ? level : 0]);
        }
        // V3 has only 4 positions so NVS write is fine immediately
        Preferences pn;
        if (pn.begin("settings", false)) {
            pn.putUChar("brightness_lv", level);
            pn.end();
        }
        static const char *labels[] = {"Normal", "Dim", "Night", "Night+"};
        Serial.printf("[BRIGHT] Brightness: %s\n", labels[level < 4 ? level : 0]);
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
    lv_obj_set_y(title, -195);
    lv_obj_set_align(title, LV_ALIGN_CENTER);
    // Make title slightly larger if built-in Montserrat fonts are available
#if LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
#else
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
#endif
    
    // Buzzer label + mode dropdown + cooldown dropdown all on one line
    ui_BuzzerLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_BuzzerLabel, "Buzzer:");
    lv_obj_set_style_text_color(ui_BuzzerLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BuzzerLabel, -155);
    lv_obj_set_y(ui_BuzzerLabel, -40);
    lv_obj_set_align(ui_BuzzerLabel, LV_ALIGN_CENTER);

    // Buzzer mode dropdown (Off / Global / Per-screen)
    ui_BuzzerSwitch = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerSwitch, "Off\nGlobal\nPer-screen");
    lv_obj_set_width(ui_BuzzerSwitch, 120);
    lv_obj_set_height(ui_BuzzerSwitch, 32);
    lv_obj_set_x(ui_BuzzerSwitch, -55);
    lv_obj_set_y(ui_BuzzerSwitch, -40);
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

    // Buzzer cooldown dropdown (same line as mode, no separate label)
    ui_BuzzerCooldownDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_BuzzerCooldownDrop, "Constant\n5s pause\n10s pause\n30s pause\n60s pause");
    lv_obj_set_width(ui_BuzzerCooldownDrop, 110);
    lv_obj_set_height(ui_BuzzerCooldownDrop, 32);
    lv_obj_set_x(ui_BuzzerCooldownDrop, 80);
    lv_obj_set_y(ui_BuzzerCooldownDrop, -40);
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
    lv_label_set_text(ip_title, "IP Address:");
    lv_obj_set_style_text_color(ip_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ip_title, -40);
    lv_obj_set_y(ip_title, -150);
    lv_obj_set_align(ip_title, LV_ALIGN_CENTER);
    
    // IP Address value
    ui_IPLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_IPLabel, "Checking...");
    lv_obj_set_style_text_color(ui_IPLabel, lv_color_hex(0x00FF00), 0);
    lv_obj_set_x(ui_IPLabel, 70);
    lv_obj_set_y(ui_IPLabel, -150);
    lv_obj_set_align(ui_IPLabel, LV_ALIGN_CENTER);

    // AP credentials label (shown instead of RSSI when in AP mode)
    ui_APCredsLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_APCredsLabel, "SSID: ESP32-SquareDisplay\nPass: 12345678");
    lv_obj_set_style_text_color(ui_APCredsLabel, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(ui_APCredsLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_x(ui_APCredsLabel, 0);
    lv_obj_set_y(ui_APCredsLabel, -100);
    lv_obj_set_align(ui_APCredsLabel, LV_ALIGN_CENTER);
    lv_label_set_long_mode(ui_APCredsLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ui_APCredsLabel, 260);
    lv_obj_add_flag(ui_APCredsLabel, LV_OBJ_FLAG_HIDDEN);
    
    // WiFi Signal Strength row
    ui_RSSITitle = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_RSSITitle, "WiFi Signal:");
    lv_obj_set_style_text_color(ui_RSSITitle, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_RSSITitle, -110);
    lv_obj_set_y(ui_RSSITitle, -100);
    lv_obj_set_align(ui_RSSITitle, LV_ALIGN_CENTER);
    
    // RSSI bar
    ui_RSSIBar = lv_bar_create(ui_SettingsPanel);
    lv_obj_set_width(ui_RSSIBar, 120);
    lv_obj_set_height(ui_RSSIBar, 14);
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
    lv_obj_set_x(ui_RSSILabel, 150);
    lv_obj_set_y(ui_RSSILabel, -100);
    lv_obj_set_align(ui_RSSILabel, LV_ALIGN_CENTER);
    
    // Auto-scroll dropdown
    ui_AutoScrollLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_AutoScrollLabel, "Auto-scroll:");
    lv_obj_set_style_text_color(ui_AutoScrollLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_AutoScrollLabel, -80);
    lv_obj_set_y(ui_AutoScrollLabel, 20);
    lv_obj_set_align(ui_AutoScrollLabel, LV_ALIGN_CENTER);

    ui_AutoScrollDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_AutoScrollDrop, "Off\n5s\n10s\n30s");
    lv_obj_set_width(ui_AutoScrollDrop, 140);
    lv_obj_set_height(ui_AutoScrollDrop, 32);
    lv_obj_set_x(ui_AutoScrollDrop, 40);
    lv_obj_set_y(ui_AutoScrollDrop, 20);
    lv_obj_set_align(ui_AutoScrollDrop, LV_ALIGN_CENTER);

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

    // Screen Off timeout dropdown
    ui_ScreenOffLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_ScreenOffLabel, "Screen Sleep:");
    lv_obj_set_style_text_color(ui_ScreenOffLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_ScreenOffLabel, -100);
    lv_obj_set_y(ui_ScreenOffLabel, 80);
    lv_obj_set_align(ui_ScreenOffLabel, LV_ALIGN_CENTER);

    ui_ScreenOffDrop = lv_dropdown_create(ui_SettingsPanel);
    lv_dropdown_set_options(ui_ScreenOffDrop, "Always on\n1 min\n5 min\n10 min\n30 min");
    lv_obj_set_width(ui_ScreenOffDrop, 140);
    lv_obj_set_height(ui_ScreenOffDrop, 32);
    lv_obj_set_x(ui_ScreenOffDrop, 40);
    lv_obj_set_y(ui_ScreenOffDrop, 80);
    lv_obj_set_align(ui_ScreenOffDrop, LV_ALIGN_CENTER);

    // Set current selection from persisted value
    {
        uint16_t so_sel = 0;
        if (screen_off_timeout_min == 1) so_sel = 1;
        else if (screen_off_timeout_min == 5) so_sel = 2;
        else if (screen_off_timeout_min == 10) so_sel = 3;
        else if (screen_off_timeout_min == 30) so_sel = 4;
        lv_dropdown_set_selected(ui_ScreenOffDrop, so_sel);
    }

    // Event handler: persist and apply screen off timeout
    lv_obj_add_event_cb(ui_ScreenOffDrop, [](lv_event_t *e){
        lv_obj_t *dd = lv_event_get_target(e);
        uint16_t idx = lv_dropdown_get_selected(dd);
        uint16_t mins = 0;
        if (idx == 1) mins = 1;
        else if (idx == 2) mins = 5;
        else if (idx == 3) mins = 10;
        else if (idx == 4) mins = 30;
        screen_off_timeout_min = mins;
        // Persist to NVS
        Preferences p;
        if (p.begin("settings", false)) {
            p.putUShort("screen_off_tmout", mins);
            p.end();
        }
        // Reset activity timer so the new timeout starts from now
        extern uint32_t g_last_activity_ms;
        g_last_activity_ms = millis();
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Brightness control — V4: PWM slider (0-100%), V3: 4-step overlay dropdown
    ui_BrightnessLevelLabel = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(ui_BrightnessLevelLabel, "Brightness:");
    lv_obj_set_style_text_color(ui_BrightnessLevelLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BrightnessLevelLabel, -90);
    lv_obj_set_y(ui_BrightnessLevelLabel, 145);
    lv_obj_set_align(ui_BrightnessLevelLabel, LV_ALIGN_CENTER);

    ui_BrightnessSliderWidget = lv_slider_create(ui_SettingsPanel);
    lv_obj_set_width(ui_BrightnessSliderWidget, 100);
    lv_obj_set_x(ui_BrightnessSliderWidget, 25);
    lv_obj_set_y(ui_BrightnessSliderWidget, 145);
    lv_obj_set_align(ui_BrightnessSliderWidget, LV_ALIGN_CENTER);

    ui_BrightnessPctLabel = lv_label_create(ui_SettingsPanel);
    lv_obj_set_style_text_color(ui_BrightnessPctLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_x(ui_BrightnessPctLabel, 120);
    lv_obj_set_y(ui_BrightnessPctLabel, 145);
    lv_obj_set_align(ui_BrightnessPctLabel, LV_ALIGN_CENTER);

    if (is_board_v4()) {
        // V4: unified slider 10-200
        // 200→110: normal PWM backlight 100%→10% (no overlay)
        // 109→10:  red overlay mode, PWM 100%→10%
        lv_slider_set_range(ui_BrightnessSliderWidget, 10, 200);
        {
            Preferences pnm;
            if (pnm.begin("settings", true)) {
                uint16_t stored = pnm.getUShort("brightness_v4", 200);
                if (stored > 200) stored = 200;
                if (stored < 10) stored = 10;
                brightness_level = (uint8_t)(stored > 255 ? 255 : stored);
                pnm.end();
            } else {
                brightness_level = 200;
            }
            lv_slider_set_value(ui_BrightnessSliderWidget, brightness_level, LV_ANIM_OFF);
            {
                uint8_t pwm = v4_slider_to_pwm(brightness_level);
                if (brightness_level >= 110) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%d%%", pwm);
                    lv_label_set_text(ui_BrightnessPctLabel, buf);
                } else {
                    char buf[12];
                    snprintf(buf, sizeof(buf), "R:%d%%", pwm);
                    lv_label_set_text(ui_BrightnessPctLabel, buf);
                }
            }
        }
    } else {
        // V3: 4-position slider (Normal/Dim/Night/Night+)
        lv_slider_set_range(ui_BrightnessSliderWidget, 0, 3);
        {
            Preferences pnm;
            if (pnm.begin("settings", true)) {
                brightness_level = pnm.getUChar("brightness_lv", 0);
                if (brightness_level > 3) brightness_level = 0;
                pnm.end();
            }
            lv_slider_set_value(ui_BrightnessSliderWidget, brightness_level, LV_ANIM_OFF);
            static const char *slabels[] = {"Normal", "Dim", "Night", "Night+"};
            lv_label_set_text(ui_BrightnessPctLabel, slabels[brightness_level]);
        }
    }

    lv_obj_add_event_cb(ui_BrightnessSliderWidget, [](lv_event_t *e){
        lv_obj_t *slider = lv_event_get_target(e);
        int32_t val = lv_slider_get_value(slider);
        if (val < 0) val = 0;
        if (val > 255) val = 255;
        set_brightness_level((uint8_t)val);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Instruction text
    lv_obj_t *instruction = lv_label_create(ui_SettingsPanel);
    lv_label_set_text(instruction, "Swipe up to return");
    lv_obj_set_style_text_color(instruction, lv_color_hex(0x808080), 0);
    lv_obj_set_x(instruction, 0);
    lv_obj_set_y(instruction, 195);
    lv_obj_set_align(instruction, LV_ALIGN_CENTER);

    // Set current selection from persisted value
    extern uint16_t auto_scroll_sec;
    sel = 0;
    if (auto_scroll_sec == 5) sel = 1;
    else if (auto_scroll_sec == 10) sel = 2;
    else if (auto_scroll_sec == 30) sel = 3;
    lv_dropdown_set_selected(ui_AutoScrollDrop, sel);
}