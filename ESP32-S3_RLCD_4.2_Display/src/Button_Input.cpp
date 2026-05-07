/*
 * Button input driver for ESP32-S3-RLCD-4.2.
 * Maps BOOT (GPIO 0) and KEY (GPIO 18) buttons to LVGL input device.
 *
 * BOOT button → LV_KEY_ENTER  (select / confirm)
 * KEY  button → LV_KEY_NEXT   (navigate to next widget)
 *
 * Both buttons are active-low with internal pull-ups.
 */

#include "Button_Input.h"
#include "board_pins.h"
#include "LVGL_Driver.h"

#include <Arduino.h>
#include <esp_log.h>

static const char *TAG = "BtnInput";

static lv_indev_drv_t  indev_drv;
static lv_indev_t     *indev = nullptr;

// Track last pressed key for the LVGL encoder/keypad driver
static uint32_t last_key  = 0;
static bool     key_pressed = false;

static void button_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // Check BOOT button (active low)
    if (digitalRead(BOOT_BTN_PIN) == LOW) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key   = LV_KEY_ENTER;
        last_key    = LV_KEY_ENTER;
        key_pressed = true;
        return;
    }
    // Check KEY button (active low)
    if (digitalRead(KEY_BTN_PIN) == LOW) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key   = LV_KEY_NEXT;
        last_key    = LV_KEY_NEXT;
        key_pressed = true;
        return;
    }

    // No button pressed
    data->state = LV_INDEV_STATE_RELEASED;
    data->key   = last_key;  // Report last key for release event
    key_pressed = false;
}

void Button_Init() {
    // Configure button GPIOs as inputs with pull-ups
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    pinMode(KEY_BTN_PIN,  INPUT_PULLUP);

    // Register LVGL keypad input device
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = button_read_cb;
    indev = lv_indev_drv_register(&indev_drv);

    // Create a default group and assign to this input device
    lv_group_t *grp = lv_group_create();
    lv_group_set_default(grp);
    lv_indev_set_group(indev, grp);

    ESP_LOGI(TAG, "Button input initialised (BOOT=GPIO%d, KEY=GPIO%d)",
             BOOT_BTN_PIN, KEY_BTN_PIN);
}
