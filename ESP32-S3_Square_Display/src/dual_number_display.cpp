#include "dual_number_display.h"
#include "screen_config_c_api.h"
#include "ui.h"
#include <stdio.h>

// Storage for dual display components (top and bottom for each screen)
static lv_obj_t* dual_top_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_top_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_top_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_bottom_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_bottom_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_bottom_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_bg_panels[NUM_SCREENS] = {nullptr};
static lv_obj_t* dual_containers[NUM_SCREENS] = {nullptr};

// Get the screen object for a given screen number (0-4)
static lv_obj_t* get_screen_obj(int screen_num) {
    switch(screen_num) {
        case 0: return ui_Screen1;
        case 1: return ui_Screen2;
        case 2: return ui_Screen3;
        case 3: return ui_Screen4;
        case 4: return ui_Screen5;
        default: return nullptr;
    }
}

// Previous values for change detection
static char prev_dual_top_text[NUM_SCREENS][64] = {0};
static char prev_dual_top_unit[NUM_SCREENS][32] = {0};
static char prev_dual_top_description[NUM_SCREENS][128] = {0};
static char prev_dual_bottom_text[NUM_SCREENS][64] = {0};
static char prev_dual_bottom_unit[NUM_SCREENS][32] = {0};
static char prev_dual_bottom_description[NUM_SCREENS][128] = {0};

// Font size to LVGL font mapping - all native sizes, no scaling
static const lv_font_t* get_font_for_size(uint8_t size) {
    switch (size) {
        case 0: return &inter_48;      // Small (48pt native)
        case 1: return &inter_72;      // Medium (72pt native)
        case 2: return &inter_96;      // Large (96pt native)
        case 3: return &inter_120;     // X-Large (120pt native)
        case 4: return &inter_144;     // XX-Large (144pt native)
        default: return &inter_48;
    }
}

// Font size to zoom level mapping - all native fonts, no scaling
static uint16_t get_zoom_for_size(uint8_t size) {
    // Always return 256 (1x) - using native font sizes
    return 256;
}

// Parse hex color to lv_color_t
static lv_color_t parse_hex_color(const char* hex) {
    if (!hex || hex[0] != '#') return lv_color_white();
    
    unsigned int r, g, b;
    if (sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3) {
        return lv_color_make(r, g, b);
    }
    return lv_color_white();
}

void dual_number_display_create(int screen_num, 
                                  uint8_t top_font_size, const char* top_font_color,
                                  uint8_t bottom_font_size, const char* bottom_font_color,
                                  const char* bg_color) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    lv_obj_t* screen = get_screen_obj(screen_num);
    if (!screen) return;
    
    // Clean up existing display if any
    dual_number_display_destroy(screen_num);
    
    // Create container for this dual display on the correct screen
    dual_containers[screen_num] = lv_obj_create(screen);
    lv_obj_set_size(dual_containers[screen_num], 480, 480);
    lv_obj_align(dual_containers[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(dual_containers[screen_num], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dual_containers[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow swipe through
    // Remove padding and set background to match panel to prevent white edges
    lv_obj_set_style_pad_all(dual_containers[screen_num], 0, 0);
    lv_obj_set_style_bg_color(dual_containers[screen_num], parse_hex_color(bg_color), 0);
    lv_obj_set_style_bg_opa(dual_containers[screen_num], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dual_containers[screen_num], 0, 0);
    lv_obj_set_style_radius(dual_containers[screen_num], 0, 0);
    
    // Create background panel
    dual_bg_panels[screen_num] = lv_obj_create(dual_containers[screen_num]);
    lv_obj_set_size(dual_bg_panels[screen_num], 480, 480);
    lv_obj_align(dual_bg_panels[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dual_bg_panels[screen_num], parse_hex_color(bg_color), 0);
    lv_obj_set_style_bg_opa(dual_bg_panels[screen_num], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dual_bg_panels[screen_num], 0, 0);
    lv_obj_set_style_pad_all(dual_bg_panels[screen_num], 0, 0);
    lv_obj_clear_flag(dual_bg_panels[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow swipe through
    lv_obj_clear_flag(dual_bg_panels[screen_num], LV_OBJ_FLAG_SCROLLABLE);
    
    // TOP DISPLAY (y: 0-240)
    // Top description label (top-left of top half)
    dual_top_description_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_top_description_labels[screen_num], "");
    lv_obj_set_size(dual_top_description_labels[screen_num], 460, 36);
    lv_label_set_long_mode(dual_top_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(dual_top_description_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(dual_top_description_labels[screen_num], parse_hex_color(top_font_color), 0);
    lv_obj_align(dual_top_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(dual_top_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Top value label (centered in top half)
    dual_top_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_top_labels[screen_num], "---");
    lv_obj_set_size(dual_top_labels[screen_num], 460, LV_SIZE_CONTENT);
    lv_label_set_long_mode(dual_top_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(dual_top_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dual_top_labels[screen_num], get_font_for_size(top_font_size), 0);
    lv_obj_set_style_text_color(dual_top_labels[screen_num], parse_hex_color(top_font_color), 0);
    lv_obj_set_style_transform_pivot_x(dual_top_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(dual_top_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(dual_top_labels[screen_num], get_zoom_for_size(top_font_size), 0);
    lv_obj_align(dual_top_labels[screen_num], LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_add_flag(dual_top_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Top unit label (bottom-right of top half)
    dual_top_unit_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_top_unit_labels[screen_num], "");
    lv_obj_set_size(dual_top_unit_labels[screen_num], 200, 36);
    lv_label_set_long_mode(dual_top_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(dual_top_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(dual_top_unit_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(dual_top_unit_labels[screen_num], parse_hex_color(top_font_color), 0);
    lv_obj_align(dual_top_unit_labels[screen_num], LV_ALIGN_TOP_RIGHT, -10, 200);
    lv_obj_add_flag(dual_top_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // BOTTOM DISPLAY (y: 240-480)
    // Bottom description label (top-left of bottom half)
    dual_bottom_description_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_bottom_description_labels[screen_num], "");
    lv_obj_set_size(dual_bottom_description_labels[screen_num], 460, 36);
    lv_label_set_long_mode(dual_bottom_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(dual_bottom_description_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(dual_bottom_description_labels[screen_num], parse_hex_color(bottom_font_color), 0);
    lv_obj_align(dual_bottom_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 250);
    lv_obj_add_flag(dual_bottom_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Bottom value label (centered in bottom half)
    dual_bottom_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_bottom_labels[screen_num], "---");
    lv_obj_set_size(dual_bottom_labels[screen_num], 460, LV_SIZE_CONTENT);
    lv_label_set_long_mode(dual_bottom_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(dual_bottom_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(dual_bottom_labels[screen_num], get_font_for_size(bottom_font_size), 0);
    lv_obj_set_style_text_color(dual_bottom_labels[screen_num], parse_hex_color(bottom_font_color), 0);
    lv_obj_set_style_transform_pivot_x(dual_bottom_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(dual_bottom_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(dual_bottom_labels[screen_num], get_zoom_for_size(bottom_font_size), 0);
    lv_obj_align(dual_bottom_labels[screen_num], LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_add_flag(dual_bottom_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Bottom unit label (bottom-right of bottom half)
    dual_bottom_unit_labels[screen_num] = lv_label_create(dual_bg_panels[screen_num]);
    lv_label_set_text(dual_bottom_unit_labels[screen_num], "");
    lv_obj_set_size(dual_bottom_unit_labels[screen_num], 200, 36);
    lv_label_set_long_mode(dual_bottom_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(dual_bottom_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(dual_bottom_unit_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(dual_bottom_unit_labels[screen_num], parse_hex_color(bottom_font_color), 0);
    lv_obj_align(dual_bottom_unit_labels[screen_num], LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(dual_bottom_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Clear previous values
    prev_dual_top_text[screen_num][0] = '\0';
    prev_dual_top_unit[screen_num][0] = '\0';
    prev_dual_top_description[screen_num][0] = '\0';
    prev_dual_bottom_text[screen_num][0] = '\0';
    prev_dual_bottom_unit[screen_num][0] = '\0';
    prev_dual_bottom_description[screen_num][0] = '\0';
}

void dual_number_display_update_top(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!dual_top_labels[screen_num]) return;
    
    // Format value text
    char text[64];
    snprintf(text, sizeof(text), "%.1f", value);
    
    // Only update if changed
    bool needs_update = false;
    if (strcmp(text, prev_dual_top_text[screen_num]) != 0) {
        lv_label_set_text(dual_top_labels[screen_num], text);
        lv_obj_align(dual_top_labels[screen_num], LV_ALIGN_TOP_MID, 0, 120);
        strncpy(prev_dual_top_text[screen_num], text, sizeof(prev_dual_top_text[screen_num]) - 1);
        needs_update = true;
    }
    
    if (strcmp(unit, prev_dual_top_unit[screen_num]) != 0) {
        lv_label_set_text(dual_top_unit_labels[screen_num], unit);
        strncpy(prev_dual_top_unit[screen_num], unit, sizeof(prev_dual_top_unit[screen_num]) - 1);
        needs_update = true;
    }
    
    if (strcmp(description, prev_dual_top_description[screen_num]) != 0) {
        lv_label_set_text(dual_top_description_labels[screen_num], description);
        strncpy(prev_dual_top_description[screen_num], description, sizeof(prev_dual_top_description[screen_num]) - 1);
        needs_update = true;
    }
}

void dual_number_display_update_bottom(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!dual_bottom_labels[screen_num]) return;
    
    // Format value text
    char text[64];
    snprintf(text, sizeof(text), "%.1f", value);
    
    // Only update if changed
    bool needs_update = false;
    if (strcmp(text, prev_dual_bottom_text[screen_num]) != 0) {
        lv_label_set_text(dual_bottom_labels[screen_num], text);
        lv_obj_align(dual_bottom_labels[screen_num], LV_ALIGN_TOP_MID, 0, 360);
        strncpy(prev_dual_bottom_text[screen_num], text, sizeof(prev_dual_bottom_text[screen_num]) - 1);
        needs_update = true;
    }
    
    if (strcmp(unit, prev_dual_bottom_unit[screen_num]) != 0) {
        lv_label_set_text(dual_bottom_unit_labels[screen_num], unit);
        strncpy(prev_dual_bottom_unit[screen_num], unit, sizeof(prev_dual_bottom_unit[screen_num]) - 1);
        needs_update = true;
    }
    
    if (strcmp(description, prev_dual_bottom_description[screen_num]) != 0) {
        lv_label_set_text(dual_bottom_description_labels[screen_num], description);
        strncpy(prev_dual_bottom_description[screen_num], description, sizeof(prev_dual_bottom_description[screen_num]) - 1);
        needs_update = true;
    }
}

void dual_number_display_destroy(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    if (dual_containers[screen_num]) {
        lv_obj_del(dual_containers[screen_num]);
        dual_containers[screen_num] = nullptr;
    }
    
    // Reset pointers (they're deleted with container)
    dual_top_labels[screen_num] = nullptr;
    dual_top_unit_labels[screen_num] = nullptr;
    dual_top_description_labels[screen_num] = nullptr;
    dual_bottom_labels[screen_num] = nullptr;
    dual_bottom_unit_labels[screen_num] = nullptr;
    dual_bottom_description_labels[screen_num] = nullptr;
    dual_bg_panels[screen_num] = nullptr;
    
    // Clear previous values
    prev_dual_top_text[screen_num][0] = '\0';
    prev_dual_top_unit[screen_num][0] = '\0';
    prev_dual_top_description[screen_num][0] = '\0';
    prev_dual_bottom_text[screen_num][0] = '\0';
    prev_dual_bottom_unit[screen_num][0] = '\0';
    prev_dual_bottom_description[screen_num][0] = '\0';
}
