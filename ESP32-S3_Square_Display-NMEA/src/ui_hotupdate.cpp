// Runtime hot-update helpers for updating backgrounds and icons without reboot
#include "ui.h"
#include "screen_config_c_api.h"
#include "number_display.h"
#include "dual_number_display.h"
#include "quad_number_display.h"
#include "gauge_number_display.h"
#include "ui_Settings.h"
#include "graph_display.h"
#include "compass_display.h"
#include "position_display.h"
#include "ais_display.h"
#include "attitude_display.h"
#include <lvgl.h>
#include "esp_log.h"
#include <Arduino.h>
#include "esp_task_wdt.h"
#include <esp_heap_caps.h>
static const char *TAG_UIHOT = "ui_hotupdate";

// Forward declaration for reset function in main.cpp
extern "C" void reset_number_display_tracking(int screen_num);
extern "C" void mark_compass_display_created(int screen_num);
extern "C" void mark_compass_display_destroyed(int screen_num);
extern "C" void mark_position_display_created(int screen_num);
extern "C" void mark_position_display_destroyed(int screen_num);
// Forward declaration for update function in main.cpp
extern "C" void force_update_number_display(int screen_num);
// Per-screen lazy-apply flags (defined in network_setup.cpp)
extern volatile bool g_screens_need_apply[5];

// Forward declarations for embedded image fallbacks (provided by SquareLine ui.h)
extern const char *ui_img_rev_counter_png;
extern const char *ui_img_rev_fuel_png;
extern const char *ui_img_temp_exhaust_png;
extern const char *ui_img_fuel_temp_png;
extern const char *ui_img_oil_temp_png;

static lv_obj_t *get_screen_obj(int s) {
    switch (s) {
        case 0: return ui_Screen1;
        case 1: return ui_Screen2;
        case 2: return ui_Screen3;
        case 3: return ui_Screen4;
        case 4: return ui_Screen5;
        default: return NULL;
    }
}

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

static lv_obj_t *get_upper_needle_obj_for_screen(int s) {
    switch (s) {
        case 0: return ui_Needle;
        case 1: return ui_Needle2;
        case 2: return ui_Needle3;
        case 3: return ui_Needle4;
        case 4: return ui_Needle5;
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
        // Log path and LVGL source type for debugging unknown-image warnings
        ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background path='%s' src_type=%d", s, path, (int)lv_img_src_get_type((const void*)path));
        lv_img_set_src(bg, path);
    } else {
        const void *fb = get_fallback_bg_for_screen(s);
        if (fb) {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background using fallback src_type=%d", s, (int)lv_img_src_get_type(fb));
            lv_img_set_src(bg, fb);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d background none", s);
            lv_obj_add_flag(bg, LV_OBJ_FLAG_HIDDEN);
        }
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
        ESP_LOGI(TAG_UIHOT, "[LVGL IMG DEBUG] screen=%d top icon ptr=%p path='%s' len=%d", s, p, (p ? p : "NULL"), (p ? strlen(p) : -1));
        if (p && p[0] != '\0') {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d top icon path='%s' src_type=%d", s, p, (int)lv_img_src_get_type((const void*)p));
            lv_img_set_src(top, p);
            lv_obj_set_style_img_opa(top, LV_OPA_COVER, 0);  // Make fully opaque
            lv_obj_clear_flag(top, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d top icon none - HIDING", s);
            lv_obj_set_style_img_opa(top, LV_OPA_TRANSP, 0);  // Make completely transparent
            lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_invalidate(top);
        any = true;
    }
    if (bot) {
        // Respect per-screen show_bottom flag: hide bottom icon if disabled
        if (!screen_configs[s].show_bottom) {
            ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon hidden", s);
            lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
        } else {
            const char *p = screen_configs[s].icon_paths[1];
            if (p && p[0] != '\0') {
                ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon path='%s' src_type=%d", s, p, (int)lv_img_src_get_type((const void*)p));
                lv_img_set_src(bot, p);
                lv_obj_set_style_img_opa(bot, LV_OPA_COVER, 0);  // Make fully opaque
                lv_obj_clear_flag(bot, LV_OBJ_FLAG_HIDDEN);
            } else {
                ESP_LOGI(TAG_UIHOT, "[LVGL IMG] screen=%d bottom icon none", s);
                lv_obj_set_style_img_opa(bot, LV_OPA_TRANSP, 0);  // Make completely transparent
                lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
            }
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

// Apply visuals for a single screen index s (0-based).
// Called both from apply_all_screen_visuals() and directly for targeted single-screen saves.
bool apply_screen_visuals_for_one(int s) {
    Serial.printf("[APPLY_ONE] s=%d display_type=%d\n", s, screen_configs[s].display_type);
    Serial.flush();
    bool any = false;
    {
        bool a = apply_background_for_screen(s);
        bool b = false;
        if (screen_configs[s].display_type != DISPLAY_TYPE_GAUGE_NUMBER) {
            b = apply_icons_for_screen(s);
        }
        any = any || a || b;

        if (screen_configs[s].display_type == DISPLAY_TYPE_NUMBER) {
            Serial.printf("[APPLY_ONE] s=%d → NUMBER\n", s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            number_display_create(s);
            reset_number_display_tracking(s + 1);
            force_update_number_display(s + 1);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_DUAL) {
            Serial.printf("[APPLY_ONE] s=%d → DUAL\n", s);
            number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            dual_number_display_create(s,
                screen_configs[s].dual_top_font_size, screen_configs[s].dual_top_font_color,
                screen_configs[s].dual_bottom_font_size, screen_configs[s].dual_bottom_font_color,
                screen_configs[s].number_bg_color);
            Serial.printf("[APPLY_ONE] s=%d DUAL created, invalidating screen\n", s);
            lv_obj_invalidate(get_screen_obj(s));
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_QUAD) {
            Serial.printf("[APPLY_ONE] s=%d → QUAD\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            quad_number_display_create(s,
                screen_configs[s].quad_tl_font_size, screen_configs[s].quad_tl_font_color,
                screen_configs[s].quad_tr_font_size, screen_configs[s].quad_tr_font_color,
                screen_configs[s].quad_bl_font_size, screen_configs[s].quad_bl_font_color,
                screen_configs[s].quad_br_font_size, screen_configs[s].quad_br_font_color,
                screen_configs[s].number_bg_color);
            Serial.printf("[APPLY_ONE] s=%d QUAD created, invalidating screen\n", s);
            lv_obj_invalidate(get_screen_obj(s));
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_GAUGE_NUMBER) {
            ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] Handling screen=%d as GAUGE_NUMBER", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            graph_display_destroy(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *bot = get_bottom_icon_obj_for_screen(s);
            if (bot) lv_obj_add_flag(bot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            const char *p = screen_configs[s].icon_paths[0];
            ESP_LOGI(TAG_UIHOT, "[GAUGE_NUM] screen=%d icon_paths[0]=%p, value='%s', len=%d",
                s, p, (p ? p : "NULL"), (p ? strlen(p) : -1));
            if (top_icon) {
                if (p && p[0] != '\0') {
                    lv_img_set_src(top_icon, p);
                    lv_obj_set_style_img_opa(top_icon, LV_OPA_COVER, 0);
                    lv_obj_set_size(top_icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_align(top_icon, LV_ALIGN_CENTER, 0, -70);
                    lv_obj_clear_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_set_style_img_opa(top_icon, LV_OPA_TRANSP, 0);
                    lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(top_icon, -5000, -5000);
                    lv_obj_set_size(top_icon, 0, 0);
                }
            }
            gauge_number_display_create(s,
                screen_configs[s].gauge_num_center_font_size,
                screen_configs[s].gauge_num_center_font_color);
            Serial.printf("[APPLY_ONE] s=%d GAUGE_NUMBER created, invalidating screen\n", s);
            lv_obj_invalidate(get_screen_obj(s));
            if (top_icon && (!p || p[0] == '\0')) {
                if (!lv_obj_has_flag(top_icon, LV_OBJ_FLAG_HIDDEN))
                    lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            }
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_GRAPH) {
            Serial.printf("[APPLY_ONE] s=%d → GRAPH\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);
            graph_display_create(s);
            Serial.printf("[APPLY_ONE] s=%d GRAPH created, invalidating screen\n", s);
            lv_obj_invalidate(get_screen_obj(s));
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_COMPASS) {
            Serial.printf("[APPLY_ONE] s=%d → COMPASS\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            position_display_destroy(s);
            mark_position_display_destroyed(s + 1);
            compass_display_create(s);
            mark_compass_display_created(s + 1);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_POSITION) {
            Serial.printf("[APPLY_ONE] s=%d → POSITION\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            compass_display_destroy(s);
            mark_compass_display_destroyed(s + 1);
            ais_display_destroy(s);
            position_display_create(s);
            mark_position_display_created(s + 1);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_AIS) {
            Serial.printf("[APPLY_ONE] s=%d → AIS\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            compass_display_destroy(s);
            mark_compass_display_destroyed(s + 1);
            position_display_destroy(s);
            mark_position_display_destroyed(s + 1);
            attitude_display_destroy(s);
            ais_display_create(s);
            any = true;
        } else if (screen_configs[s].display_type == DISPLAY_TYPE_ATTITUDE) {
            Serial.printf("[APPLY_ONE] s=%d \u2192 ATTITUDE\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            compass_display_destroy(s);
            mark_compass_display_destroyed(s + 1);
            position_display_destroy(s);
            mark_position_display_destroyed(s + 1);
            ais_display_destroy(s);            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_add_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            if (lower_needle) lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *top_icon = get_top_icon_obj_for_screen(s);
            lv_obj_t *bot_icon = get_bottom_icon_obj_for_screen(s);
            if (top_icon) lv_obj_add_flag(top_icon, LV_OBJ_FLAG_HIDDEN);
            if (bot_icon) lv_obj_add_flag(bot_icon, LV_OBJ_FLAG_HIDDEN);            attitude_display_create(s);
            any = true;
        } else {
            // DISPLAY_TYPE_GAUGE (default) — destroy all non-gauge overlays and restore needles
            Serial.printf("[APPLY_ONE] s=%d → GAUGE\n", s);
            number_display_destroy(s);
            dual_number_display_destroy(s);
            quad_number_display_destroy(s);
            gauge_number_display_destroy(s);
            graph_display_destroy(s);
            compass_display_destroy(s);
            mark_compass_display_destroyed(s + 1);
            position_display_destroy(s);
            mark_position_display_destroyed(s + 1);
            ais_display_destroy(s);
            attitude_display_destroy(s);
            // Restore upper needle visibility (was hidden when switching away from GAUGE)
            lv_obj_t *upper_needle = get_upper_needle_obj_for_screen(s);
            if (upper_needle) lv_obj_clear_flag(upper_needle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *lower_needle = get_lower_needle_obj_for_screen(s);
            if (lower_needle) {
                if (screen_configs[s].show_bottom)
                    lv_obj_clear_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_add_flag(lower_needle, LV_OBJ_FLAG_HIDDEN);
            }
            any = true;
        }
    }
    ESP_LOGI(TAG_UIHOT, "[APPLY_ONE] Completed screen %d", s);
    // Ensure night mode overlay stays on top after new objects were created
    if (brightness_level > 0) {
        night_mode_init_overlays();
    }
    return any;
}

// Apply visuals for all screens. Returns true if at least one target object was present.
// Only the currently active screen is applied immediately; inactive screens are flagged
// for lazy application when the user navigates to them (avoids double-apply / overdraw).
bool apply_all_screen_visuals() {
    int active_screen = ui_get_current_screen(); // 1-based
    int active_idx = active_screen - 1;
    Serial.printf("[APPLY_ALL] start active=%d PSRAM free=%u iRAM=%u\n",
        active_screen,
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.flush();

    // Mark ALL screens as needing apply.
    for (int s = 0; s < NUM_SCREENS; ++s)
        g_screens_need_apply[s] = true;

    // Apply the active screen right now (LVGL will render it this frame).
    bool any = false;
    if (active_idx >= 0 && active_idx < NUM_SCREENS) {
        g_screens_need_apply[active_idx] = false;
        esp_task_wdt_reset();
        any = apply_screen_visuals_for_one(active_idx);
    }

    Serial.printf("[APPLY_ALL] complete PSRAM free=%u iRAM=%u\n",
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.flush();
    return any;
}
