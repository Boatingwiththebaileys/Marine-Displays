#ifndef _UI_SETTINGS_H
#define _UI_SETTINGS_H

#include "lvgl.h"

// Settings screen objects
extern lv_obj_t *ui_Settings;
extern lv_obj_t *ui_SettingsPanel;
extern lv_obj_t *ui_BrightnessSlider;
extern lv_obj_t *ui_BrightnessLabel;
extern lv_obj_t *ui_IPLabel;
extern lv_obj_t *ui_BackButton;
extern lv_obj_t *ui_BuzzerSwitch;
extern lv_obj_t *ui_BuzzerLabel;

extern int buzzer_mode;  // 0 = Off, 1 = Global, 2 = Per-screen
extern uint16_t buzzer_cooldown_sec; // 0 = constant, otherwise seconds between beeps
extern bool first_run_buzzer; // Set by settings when cooldown changes to allow immediate re-eval

#ifdef __cplusplus
extern "C" {
#endif

void ui_Settings_screen_init(void);
void update_ip_address(void);
void trigger_buzzer_alert(void);

#ifdef __cplusplus
}
#endif

#endif
