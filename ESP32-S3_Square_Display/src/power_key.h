// power_key.h — V4 boards on battery: hold the power button to switch off.
//
// On the V4 board (Waveshare schematic V4.0) the battery stays connected only
// while SYS_EN (IO expander EXIO5) is high or the power button (Key1) is held.
// The firmware sets SYS_EN high at start-up, so without this it runs until
// the battery is flat. Key1 also pulls SYS_OUT (GPIO16) low, which is how we
// see it.
//
// Hold the button for POWER_KEY_HOLD_MS, then let go: SYS_EN goes low and the
// battery is disconnected. While the button is held the battery stays
// connected through the button itself, so we wait for the release.
// On 12 V or USB power the board can't switch itself off; after a moment we
// put SYS_EN back and say so.
//
// V3 boards: the button goes straight to the charger chip (double-click to
// switch off), so this does nothing there.
//
// Used by both the Signal K and NMEA 2000 Square firmware.

#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "TCA9554PWR.h"

#define POWER_KEY_PIN      16     // SYS_OUT: LOW while Key1 is pressed (V4)
#define POWER_KEY_HOLD_MS  3000

#include "ui.h"   // declares inter_24

static lv_obj_t *power_msg_box = nullptr;
static lv_obj_t *power_msg_label = nullptr;

static void power_msg_show(const char *text) {
    if (!power_msg_box) {
        power_msg_box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(power_msg_box);
        lv_obj_set_size(power_msg_box, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(power_msg_box, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(power_msg_box, LV_OPA_COVER, 0);
        power_msg_label = lv_label_create(power_msg_box);
        lv_obj_set_width(power_msg_label, LV_PCT(80));
        lv_obj_set_style_text_color(power_msg_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(power_msg_label, &inter_24, 0);
        lv_obj_set_style_text_align(power_msg_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(power_msg_label);
    }
    lv_label_set_text(power_msg_label, text);
    lv_refr_now(NULL);
}

static void power_msg_hide() {
    if (power_msg_box) {
        lv_obj_del(power_msg_box);
        power_msg_box = nullptr;
        power_msg_label = nullptr;
    }
}

// Call from loop().
static void power_key_loop() {
    if (!is_board_v4()) return;

    static bool started = false;
    static bool seen_release = false;
    static bool armed = false;
    static unsigned long down_since = 0;

    if (!started) {
        pinMode(POWER_KEY_PIN, INPUT);   // 10k pull-up on the board
        started = true;
    }
    bool pressed = digitalRead(POWER_KEY_PIN) == LOW;
    unsigned long now = millis();

    // The press that switched the display on may still be held at start-up.
    if (!seen_release) {
        if (!pressed) seen_release = true;
        return;
    }

    if (pressed) {
        if (down_since == 0) down_since = now ? now : 1;
        if (!armed && now - down_since >= POWER_KEY_HOLD_MS) {
            armed = true;
            Serial.println("[POWER] Button held - switching off on release");
            power_msg_show("Let go of the button\nto switch off");
        }
        return;
    }

    // Released
    bool was_armed = armed;
    down_since = 0;
    armed = false;
    if (!was_armed) return;

    power_msg_show("Switching off...");
    Serial.println("[POWER] SYS_EN low - disconnecting the battery");
    Serial.flush();
    delay(300);
    Set_EXIO(PIN_SYS_EN, Low);
    delay(2000);

    // Still running: powered from 12 V or USB, which this can't switch off.
    Set_EXIO(PIN_SYS_EN, High);
    Serial.println("[POWER] Still powered (12 V / USB) - staying on");
    power_msg_show("On 12 V or USB power.\nSwitch off at the supply.");
    delay(3000);
    power_msg_hide();
}
