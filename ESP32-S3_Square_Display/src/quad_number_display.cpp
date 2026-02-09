#include "quad_number_display.h"
#include "screen_config_c_api.h"
#include "ui.h"
#include <stdio.h>

// Storage for quad display components (top-left, top-right, bottom-left, bottom-right for each screen)
static lv_obj_t* quad_tl_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_tl_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_tl_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_tr_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_tr_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_tr_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_bl_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_bl_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_bl_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_br_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_br_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_br_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_bg_panels[NUM_SCREENS] = {nullptr};
static lv_obj_t* quad_containers[NUM_SCREENS] = {nullptr};

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
static char prev_quad_tl_text[NUM_SCREENS][64] = {0};
static char prev_quad_tl_unit[NUM_SCREENS][32] = {0};
static char prev_quad_tl_description[NUM_SCREENS][128] = {0};
static char prev_quad_tr_text[NUM_SCREENS][64] = {0};
static char prev_quad_tr_unit[NUM_SCREENS][32] = {0};
static char prev_quad_tr_description[NUM_SCREENS][128] = {0};
static char prev_quad_bl_text[NUM_SCREENS][64] = {0};
static char prev_quad_bl_unit[NUM_SCREENS][32] = {0};
static char prev_quad_bl_description[NUM_SCREENS][128] = {0};
static char prev_quad_br_text[NUM_SCREENS][64] = {0};
static char prev_quad_br_unit[NUM_SCREENS][32] = {0};
static char prev_quad_br_description[NUM_SCREENS][128] = {0};

// Font size to zoom mapping - all native fonts, no scaling (LVGL uses 256 as 100% scale)
static int font_size_to_zoom(uint8_t font_size) {
    // Always return 256 (1x) - using native font sizes for crisp rendering
    return 256;
}

// Font size to LVGL font mapping - all native sizes
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

// Parse hex color string to lv_color_t
static lv_color_t parse_hex_color(const char* hex) {
    if (!hex || hex[0] != '#') {
        return lv_color_white();
    }
    
    unsigned int r, g, b;
    if (sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3) {
        return lv_color_make(r, g, b);
    }
    return lv_color_white();
}

void quad_number_display_create(int screen_num,
                                  uint8_t tl_font_size, const char* tl_font_color,
                                  uint8_t tr_font_size, const char* tr_font_color,
                                  uint8_t bl_font_size, const char* bl_font_color,
                                  uint8_t br_font_size, const char* br_font_color,
                                  const char* bg_color) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    lv_obj_t* screen = get_screen_obj(screen_num);
    if (!screen) return;
    
    // Clean up existing display if any
    quad_number_display_destroy(screen_num);
    
    // Create container for this quad display on the correct screen
    quad_containers[screen_num] = lv_obj_create(screen);
    lv_obj_set_size(quad_containers[screen_num], 480, 480);
    lv_obj_align(quad_containers[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(quad_containers[screen_num], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(quad_containers[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow swipe through
    // Remove padding and set background to match panel to prevent white edges
    lv_obj_set_style_pad_all(quad_containers[screen_num], 0, 0);
    lv_obj_set_style_bg_color(quad_containers[screen_num], parse_hex_color(bg_color), 0);
    lv_obj_set_style_bg_opa(quad_containers[screen_num], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(quad_containers[screen_num], 0, 0);
    lv_obj_set_style_radius(quad_containers[screen_num], 0, 0);
    
    // Create background panel
    quad_bg_panels[screen_num] = lv_obj_create(quad_containers[screen_num]);
    lv_obj_set_size(quad_bg_panels[screen_num], 480, 480);
    lv_obj_align(quad_bg_panels[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(quad_bg_panels[screen_num], parse_hex_color(bg_color), 0);
    lv_obj_set_style_bg_opa(quad_bg_panels[screen_num], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(quad_bg_panels[screen_num], 0, 0);
    lv_obj_set_style_pad_all(quad_bg_panels[screen_num], 0, 0);
    lv_obj_clear_flag(quad_bg_panels[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow swipe through
    lv_obj_clear_flag(quad_bg_panels[screen_num], LV_OBJ_FLAG_SCROLLABLE);
    
    // All quadrants use Inter fonts - native sizes, no scaling
    const lv_font_t* small_font = &inter_16;
    
    // TOP-LEFT QUADRANT (0-240, 0-240)
    // Description label (top-left corner of TL quadrant)
    quad_tl_description_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tl_description_labels[screen_num], "");
    lv_obj_set_size(quad_tl_description_labels[screen_num], 230, 24);
    lv_label_set_long_mode(quad_tl_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(quad_tl_description_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_tl_description_labels[screen_num], parse_hex_color(tl_font_color), 0);
    lv_obj_align(quad_tl_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 5, 5);
    lv_obj_add_flag(quad_tl_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Value label (centered in TL quadrant)
    quad_tl_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tl_labels[screen_num], "---");
    lv_obj_set_size(quad_tl_labels[screen_num], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(quad_tl_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_tl_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(quad_tl_labels[screen_num], get_font_for_size(tl_font_size), 0);
    lv_obj_set_style_text_color(quad_tl_labels[screen_num], parse_hex_color(tl_font_color), 0);
    lv_obj_set_style_transform_pivot_x(quad_tl_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(quad_tl_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(quad_tl_labels[screen_num], font_size_to_zoom(tl_font_size), 0);
    lv_obj_align(quad_tl_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 100);
    lv_obj_add_flag(quad_tl_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Unit label (bottom-right of TL quadrant)
    quad_tl_unit_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tl_unit_labels[screen_num], "");
    lv_obj_set_size(quad_tl_unit_labels[screen_num], 100, 24);
    lv_label_set_long_mode(quad_tl_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_tl_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(quad_tl_unit_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_tl_unit_labels[screen_num], parse_hex_color(tl_font_color), 0);
    lv_obj_align(quad_tl_unit_labels[screen_num], LV_ALIGN_TOP_LEFT, 135, 210);
    lv_obj_add_flag(quad_tl_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // TOP-RIGHT QUADRANT (240-480, 0-240)
    // Description label (top-left corner of TR quadrant)
    quad_tr_description_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tr_description_labels[screen_num], "");
    lv_obj_set_size(quad_tr_description_labels[screen_num], 230, 24);
    lv_label_set_long_mode(quad_tr_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(quad_tr_description_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_tr_description_labels[screen_num], parse_hex_color(tr_font_color), 0);
    lv_obj_align(quad_tr_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 245, 5);
    lv_obj_add_flag(quad_tr_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Value label (centered in TR quadrant)
    quad_tr_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tr_labels[screen_num], "---");
    lv_obj_set_size(quad_tr_labels[screen_num], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(quad_tr_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_tr_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(quad_tr_labels[screen_num], get_font_for_size(tr_font_size), 0);
    lv_obj_set_style_text_color(quad_tr_labels[screen_num], parse_hex_color(tr_font_color), 0);
    lv_obj_set_style_transform_pivot_x(quad_tr_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(quad_tr_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(quad_tr_labels[screen_num], font_size_to_zoom(tr_font_size), 0);
    lv_obj_align(quad_tr_labels[screen_num], LV_ALIGN_TOP_LEFT, 250, 100);
    lv_obj_add_flag(quad_tr_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Unit label (bottom-right of TR quadrant)
    quad_tr_unit_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_tr_unit_labels[screen_num], "");
    lv_obj_set_size(quad_tr_unit_labels[screen_num], 100, 24);
    lv_label_set_long_mode(quad_tr_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_tr_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(quad_tr_unit_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_tr_unit_labels[screen_num], parse_hex_color(tr_font_color), 0);
    lv_obj_align(quad_tr_unit_labels[screen_num], LV_ALIGN_TOP_LEFT, 375, 210);
    lv_obj_add_flag(quad_tr_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // BOTTOM-LEFT QUADRANT (0-240, 240-480)
    // Description label (top-left corner of BL quadrant)
    quad_bl_description_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_bl_description_labels[screen_num], "");
    lv_obj_set_size(quad_bl_description_labels[screen_num], 230, 24);
    lv_label_set_long_mode(quad_bl_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(quad_bl_description_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_bl_description_labels[screen_num], parse_hex_color(bl_font_color), 0);
    lv_obj_align(quad_bl_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 5, 245);
    lv_obj_add_flag(quad_bl_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Value label (centered in BL quadrant)
    quad_bl_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_bl_labels[screen_num], "---");
    lv_obj_set_size(quad_bl_labels[screen_num], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(quad_bl_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_bl_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(quad_bl_labels[screen_num], get_font_for_size(bl_font_size), 0);
    lv_obj_set_style_text_color(quad_bl_labels[screen_num], parse_hex_color(bl_font_color), 0);
    lv_obj_set_style_transform_pivot_x(quad_bl_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(quad_bl_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(quad_bl_labels[screen_num], font_size_to_zoom(bl_font_size), 0);
    lv_obj_align(quad_bl_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 340);
    lv_obj_add_flag(quad_bl_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Unit label (bottom-right of BL quadrant)
    quad_bl_unit_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_bl_unit_labels[screen_num], "");
    lv_obj_set_size(quad_bl_unit_labels[screen_num], 100, 24);
    lv_label_set_long_mode(quad_bl_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_bl_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(quad_bl_unit_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_bl_unit_labels[screen_num], parse_hex_color(bl_font_color), 0);
    lv_obj_align(quad_bl_unit_labels[screen_num], LV_ALIGN_TOP_LEFT, 135, 450);
    lv_obj_add_flag(quad_bl_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // BOTTOM-RIGHT QUADRANT (240-480, 240-480)
    // Description label (top-left corner of BR quadrant)
    quad_br_description_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_br_description_labels[screen_num], "");
    lv_obj_set_size(quad_br_description_labels[screen_num], 230, 24);
    lv_label_set_long_mode(quad_br_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(quad_br_description_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_br_description_labels[screen_num], parse_hex_color(br_font_color), 0);
    lv_obj_align(quad_br_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 245, 245);
    lv_obj_add_flag(quad_br_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Value label (centered in BR quadrant)
    quad_br_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_br_labels[screen_num], "---");
    lv_obj_set_size(quad_br_labels[screen_num], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(quad_br_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_br_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(quad_br_labels[screen_num], get_font_for_size(br_font_size), 0);
    lv_obj_set_style_text_color(quad_br_labels[screen_num], parse_hex_color(br_font_color), 0);
    lv_obj_set_style_transform_pivot_x(quad_br_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(quad_br_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(quad_br_labels[screen_num], font_size_to_zoom(br_font_size), 0);
    lv_obj_align(quad_br_labels[screen_num], LV_ALIGN_TOP_LEFT, 250, 340);
    lv_obj_add_flag(quad_br_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Unit label (bottom-right of BR quadrant)
    quad_br_unit_labels[screen_num] = lv_label_create(quad_bg_panels[screen_num]);
    lv_label_set_text(quad_br_unit_labels[screen_num], "");
    lv_obj_set_size(quad_br_unit_labels[screen_num], 100, 24);
    lv_label_set_long_mode(quad_br_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(quad_br_unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(quad_br_unit_labels[screen_num], small_font, 0);
    lv_obj_set_style_text_color(quad_br_unit_labels[screen_num], parse_hex_color(br_font_color), 0);
    lv_obj_align(quad_br_unit_labels[screen_num], LV_ALIGN_TOP_LEFT, 375, 450);
    lv_obj_add_flag(quad_br_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
}

void quad_number_display_destroy(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    if (quad_tl_labels[screen_num]) { lv_obj_del(quad_tl_labels[screen_num]); quad_tl_labels[screen_num] = nullptr; }
    if (quad_tl_unit_labels[screen_num]) { lv_obj_del(quad_tl_unit_labels[screen_num]); quad_tl_unit_labels[screen_num] = nullptr; }
    if (quad_tl_description_labels[screen_num]) { lv_obj_del(quad_tl_description_labels[screen_num]); quad_tl_description_labels[screen_num] = nullptr; }
    if (quad_tr_labels[screen_num]) { lv_obj_del(quad_tr_labels[screen_num]); quad_tr_labels[screen_num] = nullptr; }
    if (quad_tr_unit_labels[screen_num]) { lv_obj_del(quad_tr_unit_labels[screen_num]); quad_tr_unit_labels[screen_num] = nullptr; }
    if (quad_tr_description_labels[screen_num]) { lv_obj_del(quad_tr_description_labels[screen_num]); quad_tr_description_labels[screen_num] = nullptr; }
    if (quad_bl_labels[screen_num]) { lv_obj_del(quad_bl_labels[screen_num]); quad_bl_labels[screen_num] = nullptr; }
    if (quad_bl_unit_labels[screen_num]) { lv_obj_del(quad_bl_unit_labels[screen_num]); quad_bl_unit_labels[screen_num] = nullptr; }
    if (quad_bl_description_labels[screen_num]) { lv_obj_del(quad_bl_description_labels[screen_num]); quad_bl_description_labels[screen_num] = nullptr; }
    if (quad_br_labels[screen_num]) { lv_obj_del(quad_br_labels[screen_num]); quad_br_labels[screen_num] = nullptr; }
    if (quad_br_unit_labels[screen_num]) { lv_obj_del(quad_br_unit_labels[screen_num]); quad_br_unit_labels[screen_num] = nullptr; }
    if (quad_br_description_labels[screen_num]) { lv_obj_del(quad_br_description_labels[screen_num]); quad_br_description_labels[screen_num] = nullptr; }
    if (quad_bg_panels[screen_num]) { lv_obj_del(quad_bg_panels[screen_num]); quad_bg_panels[screen_num] = nullptr; }
    if (quad_containers[screen_num]) { lv_obj_del(quad_containers[screen_num]); quad_containers[screen_num] = nullptr; }
    
    // Clear previous values
    prev_quad_tl_text[screen_num][0] = '\0';
    prev_quad_tl_unit[screen_num][0] = '\0';
    prev_quad_tl_description[screen_num][0] = '\0';
    prev_quad_tr_text[screen_num][0] = '\0';
    prev_quad_tr_unit[screen_num][0] = '\0';
    prev_quad_tr_description[screen_num][0] = '\0';
    prev_quad_bl_text[screen_num][0] = '\0';
    prev_quad_bl_unit[screen_num][0] = '\0';
    prev_quad_bl_description[screen_num][0] = '\0';
    prev_quad_br_text[screen_num][0] = '\0';
    prev_quad_br_unit[screen_num][0] = '\0';
    prev_quad_br_description[screen_num][0] = '\0';
}

void quad_number_display_update_tl(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!quad_tl_labels[screen_num]) return;
    
    char value_text[64];
    snprintf(value_text, sizeof(value_text), "%.1f", value);
    
    // Only update if value changed
    if (strcmp(value_text, prev_quad_tl_text[screen_num]) != 0) {
        lv_label_set_text(quad_tl_labels[screen_num], value_text);
        strncpy(prev_quad_tl_text[screen_num], value_text, sizeof(prev_quad_tl_text[screen_num]) - 1);
    }
    
    // Only update if unit changed
    if (strcmp(unit, prev_quad_tl_unit[screen_num]) != 0) {
        lv_label_set_text(quad_tl_unit_labels[screen_num], unit);
        strncpy(prev_quad_tl_unit[screen_num], unit, sizeof(prev_quad_tl_unit[screen_num]) - 1);
    }
    
    // Only update if description changed
    if (strcmp(description, prev_quad_tl_description[screen_num]) != 0) {
        lv_label_set_text(quad_tl_description_labels[screen_num], description);
        strncpy(prev_quad_tl_description[screen_num], description, sizeof(prev_quad_tl_description[screen_num]) - 1);
    }
}

void quad_number_display_update_tr(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!quad_tr_labels[screen_num]) return;
    
    char value_text[64];
    snprintf(value_text, sizeof(value_text), "%.1f", value);
    
    if (strcmp(value_text, prev_quad_tr_text[screen_num]) != 0) {
        lv_label_set_text(quad_tr_labels[screen_num], value_text);
        strncpy(prev_quad_tr_text[screen_num], value_text, sizeof(prev_quad_tr_text[screen_num]) - 1);
    }
    
    if (strcmp(unit, prev_quad_tr_unit[screen_num]) != 0) {
        lv_label_set_text(quad_tr_unit_labels[screen_num], unit);
        strncpy(prev_quad_tr_unit[screen_num], unit, sizeof(prev_quad_tr_unit[screen_num]) - 1);
    }
    
    if (strcmp(description, prev_quad_tr_description[screen_num]) != 0) {
        lv_label_set_text(quad_tr_description_labels[screen_num], description);
        strncpy(prev_quad_tr_description[screen_num], description, sizeof(prev_quad_tr_description[screen_num]) - 1);
    }
}

void quad_number_display_update_bl(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!quad_bl_labels[screen_num]) return;
    
    char value_text[64];
    snprintf(value_text, sizeof(value_text), "%.1f", value);
    
    if (strcmp(value_text, prev_quad_bl_text[screen_num]) != 0) {
        lv_label_set_text(quad_bl_labels[screen_num], value_text);
        strncpy(prev_quad_bl_text[screen_num], value_text, sizeof(prev_quad_bl_text[screen_num]) - 1);
    }
    
    if (strcmp(unit, prev_quad_bl_unit[screen_num]) != 0) {
        lv_label_set_text(quad_bl_unit_labels[screen_num], unit);
        strncpy(prev_quad_bl_unit[screen_num], unit, sizeof(prev_quad_bl_unit[screen_num]) - 1);
    }
    
    if (strcmp(description, prev_quad_bl_description[screen_num]) != 0) {
        lv_label_set_text(quad_bl_description_labels[screen_num], description);
        strncpy(prev_quad_bl_description[screen_num], description, sizeof(prev_quad_bl_description[screen_num]) - 1);
    }
}

void quad_number_display_update_br(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!quad_br_labels[screen_num]) return;
    
    char value_text[64];
    snprintf(value_text, sizeof(value_text), "%.1f", value);
    
    if (strcmp(value_text, prev_quad_br_text[screen_num]) != 0) {
        lv_label_set_text(quad_br_labels[screen_num], value_text);
        strncpy(prev_quad_br_text[screen_num], value_text, sizeof(prev_quad_br_text[screen_num]) - 1);
    }
    
    if (strcmp(unit, prev_quad_br_unit[screen_num]) != 0) {
        lv_label_set_text(quad_br_unit_labels[screen_num], unit);
        strncpy(prev_quad_br_unit[screen_num], unit, sizeof(prev_quad_br_unit[screen_num]) - 1);
    }
    
    if (strcmp(description, prev_quad_br_description[screen_num]) != 0) {
        lv_label_set_text(quad_br_description_labels[screen_num], description);
        strncpy(prev_quad_br_description[screen_num], description, sizeof(prev_quad_br_description[screen_num]) - 1);
    }
}
