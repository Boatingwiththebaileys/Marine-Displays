#ifndef _UI_SETTINGS_H
#define _UI_SETTINGS_H

#include "lvgl.h"

// Settings screen objects
extern lv_obj_t *ui_Settings;
extern lv_obj_t *ui_SettingsPanel;
extern lv_obj_t *ui_BrightnessSlider;
extern lv_obj_t *ui_BrightnessLabel;
extern lv_obj_t *ui_IPLabel;
extern lv_obj_t *ui_APCredsLabel;
extern lv_obj_t *ui_RSSILabel;
extern lv_obj_t *ui_RSSIBar;
extern lv_obj_t *ui_BackButton;
extern lv_obj_t *ui_BuzzerSwitch;
extern lv_obj_t *ui_BuzzerLabel;

extern int buzzer_mode;  // 0 = Off, 1 = Global, 2 = Per-screen
extern uint16_t buzzer_cooldown_sec; // 0 = constant, otherwise seconds between beeps
extern bool first_run_buzzer; // Set by settings when cooldown changes to allow immediate re-eval

// Brightness level: 0=Normal, 1=Dim, 2=Night, 3=Night+
extern uint8_t brightness_level;

#ifdef __cplusplus
extern "C" {
#endif

void ui_Settings_screen_init(void);
void update_ip_address(void);
void trigger_buzzer_alert(void);
void night_mode_init_overlays(void);
void set_brightness_level(uint8_t level);
void brightness_nvs_flush(void);

#ifdef __cplusplus
}
#endif

#endif
