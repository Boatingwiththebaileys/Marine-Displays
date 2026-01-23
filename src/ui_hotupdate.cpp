// Runtime hot-update helpers for updating backgrounds and icons without reboot
#include "ui.h"
#include "screen_config_c_api.h"
#include <lvgl.h>

// Forward declarations for embedded image fallbacks (provided by SquareLine ui.h)
extern const char *ui_img_rev_counter_png;
extern const char *ui_img_rev_fuel_png;
extern const char *ui_img_temp_exhaust_png;
extern const char *ui_img_fuel_temp_png;
extern const char *ui_img_oil_temp_png;

static lv_obj_t *get_background_img_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_RevTemp;
        case 1: return ui_RevFuel;
        case 2: return ui_TempExhaust;
        case 3: return ui_FuelTemp;
        case 4: return ui_OilTemp;
        default: return NULL;
    }
}

// Return a fallback background source for a screen.
// Can be either a string (SD path) or a pointer to an embedded lv_img_dsc_t.
static const void *get_fallback_bg_for_screen(int s) {
    // Prefer a single embedded default image for all screens.
    return &ui_img_default_png;
}

static lv_obj_t *get_top_icon_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_TopIcon1;
        case 1: return ui_TopIcon2;
        case 2: return ui_TopIcon3;
        case 3: return ui_TopIcon4;
        case 4: return ui_TopIcon5;
        default: return NULL;
    }
}

static lv_obj_t *get_bottom_icon_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_BottomIcon1;
        case 1: return ui_BottomIcon2;
        case 2: return ui_BottomIcon3;
        case 3: return ui_BottomIcon4;
        case 4: return ui_BottomIcon5;
        default: return NULL;
    }
}

static lv_obj_t *get_lower_needle_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_Lower_Needle;
        case 1: return ui_Lower_Needle2;
        case 2: return ui_Lower_Needle3;
        case 3: return ui_Lower_Needle4;
        case 4: return ui_Lower_Needle5;
        default: return NULL;
    }
}

// Apply the background for a single screen. Returns true if object exists and update attempted.
bool apply_background_for_screen(int s) {
    lv_obj_t *bg = get_background_img_obj_for_screen(s);
    if (!bg) return false;
    const char *path = screen_configs[s].background_path;
    if (path && path[0] != '\0') {
        lv_img_set_src(bg, path);
    } else {
        const void *fb = get_fallback_bg_for_screen(s);
        if (fb) lv_img_set_src(bg, fb);
        else lv_img_set_src(bg, NULL);
    }
    lv_obj_invalidate(bg);
    return true;
}

// Apply top/bottom icon images for a single screen
bool apply_icons_for_screen(int s) {
    lv_obj_t *top = get_top_icon_obj_for_screen(s);
    lv_obj_t *bot = get_bottom_icon_obj_for_screen(s);
    bool any = false;
    if (top) {
        const char *p = screen_configs[s].icon_paths[0];
        if (p && p[0] != '\0') lv_img_set_src(top, p); else lv_img_set_src(top, NULL);
        lv_obj_invalidate(top);
        any = true;
    }
    if (bot) {
        // Respect per-screen show_bottom flag: hide bottom icon if disabled
        if (!screen_configs[s].show_bottom) {
            lv_img_set_src(bot, NULL);
            lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
        } else {
            const char *p = screen_configs[s].icon_paths[1];
            if (p && p[0] != '\0') lv_img_set_src(bot, p); else lv_img_set_src(bot, NULL);
            lv_obj_clear_flag(bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(bot);
        }
        any = true;
    }
    // Also hide/show the lower needle line object (the actual second gauge needle)
    lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
    if (lower_needle) {
        if (!screen_configs[s].show_bottom) {
            lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
        }
        any = true;
    }
    return any;
}

// Apply visuals for all screens. Returns true if at least one target object was present.
bool apply_all_screen_visuals() {
    bool any = false;
    for (int s = 0; s < NUM_SCREENS; ++s) {
        bool a = apply_background_for_screen(s);
        bool b = apply_icons_for_screen(s);
        any = any || a || b;
    }
    // Force LVGL refresh
    lv_refr_now(NULL);
    return any;
}
