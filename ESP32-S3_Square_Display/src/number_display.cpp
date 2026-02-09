#include "number_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include <Arduino.h>

// Storage for number display labels (one per screen)
static lv_obj_t* number_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* unit_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* description_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* bg_panels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};

// Previous values for change detection to avoid unnecessary redraws
static char prev_number_text[NUM_SCREENS][32] = {0};
static char prev_unit_text[NUM_SCREENS][32] = {0};
static char prev_description_text[NUM_SCREENS][128] = {0};

// Helper to convert hex color string to lv_color_t
static lv_color_t hex_to_lv_color(const char* hex) {
    if (!hex || hex[0] != '#' || strlen(hex) != 7) {
        return lv_color_white();
    }
    unsigned long color_val = strtoul(hex + 1, NULL, 16);
    uint8_t r = (color_val >> 16) & 0xFF;
    uint8_t g = (color_val >> 8) & 0xFF;
    uint8_t b = color_val & 0xFF;
    return lv_color_make(r, g, b);
}

// Get the screen object for a given screen number (0-4)
static lv_obj_t* get_screen_obj(int screen_num) {
    switch(screen_num) {
        case 0: return ui_Screen1;
        case 1: return ui_Screen2;
        case 2: return ui_Screen3;
        case 3: return ui_Screen4;
        case 4: return ui_Screen5;
        default: return NULL;
    }
}

void number_display_create(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    lv_obj_t* screen = get_screen_obj(screen_num);
    if (!screen) return;
    
    // Clean up existing labels if they exist
    number_display_destroy(screen_num);
    
    // Hide gauge elements (needles and icons)
    lv_obj_t* top_needles[] = {ui_Needle, ui_Needle2, ui_Needle3, ui_Needle4, ui_Needle5};
    lv_obj_t* bottom_needles[] = {ui_Lower_Needle, ui_Lower_Needle2, ui_Lower_Needle3, ui_Lower_Needle4, ui_Lower_Needle5};
    lv_obj_t* top_icons[] = {ui_TopIcon1, ui_TopIcon2, ui_TopIcon3, ui_TopIcon4, ui_TopIcon5};
    lv_obj_t* bottom_icons[] = {ui_BottomIcon1, ui_BottomIcon2, ui_BottomIcon3, ui_BottomIcon4, ui_BottomIcon5};
    
    if (top_needles[screen_num]) lv_obj_add_flag(top_needles[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (bottom_needles[screen_num]) lv_obj_add_flag(bottom_needles[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (top_icons[screen_num]) lv_obj_add_flag(top_icons[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (bottom_icons[screen_num]) lv_obj_add_flag(bottom_icons[screen_num], LV_OBJ_FLAG_HIDDEN);
    
    // Get configuration for this screen
    ScreenConfig& cfg = screen_configs[screen_num];
    
    // Handle background based on background_path field
    String bg_image = String(cfg.background_path);
    if (bg_image == "Custom Color") {
        // Create solid color background panel
        bg_panels[screen_num] = lv_obj_create(screen);
        lv_obj_set_size(bg_panels[screen_num], LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(bg_panels[screen_num], hex_to_lv_color(cfg.number_bg_color), 0);
        lv_obj_set_style_bg_opa(bg_panels[screen_num], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bg_panels[screen_num], 0, 0);
        lv_obj_clear_flag(bg_panels[screen_num], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bg_panels[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow touch events to pass through
    } else if (bg_image.length() > 0 && bg_image != "Default") {
        // TODO: Load bin file background image if needed
        // For now, just use default screen background
    }
    // For empty/"Default", use the screen's existing background
    
    // All sizes use custom Inter fonts with scaling for larger sizes
    // Use Inter fonts - all native sizes, no scaling for crisp text
    const lv_font_t* value_font = &inter_48;  // Default to 48pt
    const lv_font_t* unit_font = &inter_24;
    int zoom_scale = 256;  // 256 = 1x (100%), LVGL uses 256 as base (always 1x now)
    
    if (cfg.number_font_size == 0) {  // Small - 48pt native
        value_font = &inter_48;
    } else if (cfg.number_font_size == 1) {  // Medium - 72pt native
        value_font = &inter_72;
    } else if (cfg.number_font_size == 2) {  // Large - 96pt native
        value_font = &inter_96;
    } else if (cfg.number_font_size == 3) {  // X-Large - 120pt native
        value_font = &inter_120;
    } else if (cfg.number_font_size == 4) {  // XX-Large - 144pt native
        value_font = &inter_144;
    }
    // zoom_scale stays at 256 (1x) for all sizes - no scaling needed
    
    // Get font color
    lv_color_t font_color = hex_to_lv_color(cfg.number_font_color);
    
    // Create description label (top left corner)
    description_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(description_labels[screen_num], "");
    lv_obj_set_size(description_labels[screen_num], 460, 36);  // Fixed width AND height
    lv_label_set_long_mode(description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(description_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(description_labels[screen_num], font_color, 0);
    lv_obj_align(description_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);  // Prevent layout updates
    
    // Create large number label (centered)
    number_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(number_labels[screen_num], "---");
    lv_obj_set_size(number_labels[screen_num], 460, LV_SIZE_CONTENT);  // Fixed width, content height
    lv_label_set_long_mode(number_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(number_labels[screen_num], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(number_labels[screen_num], value_font, 0);
    lv_obj_set_style_text_color(number_labels[screen_num], font_color, 0);
    lv_obj_set_style_transform_pivot_x(number_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(number_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(number_labels[screen_num], zoom_scale, 0);
    lv_obj_align(number_labels[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(number_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    Serial.printf("[NUMBER_DISPLAY] Applied zoom %d to label (font_size setting: %d)\n", 
                  zoom_scale, cfg.number_font_size);
    
    // Create unit label (bottom right corner)
    unit_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(unit_labels[screen_num], "");
    lv_obj_set_size(unit_labels[screen_num], 200, 36);  // Fixed width AND height
    lv_label_set_long_mode(unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(unit_labels[screen_num], unit_font, 0);
    lv_obj_set_style_text_color(unit_labels[screen_num], font_color, 0);
    lv_obj_align(unit_labels[screen_num], LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_flag(unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    Serial.printf("[NUMBER_DISPLAY] Created for screen %d (bg=%s, font_size=%d)\n", 
                  screen_num, cfg.background_path, cfg.number_font_size);
}

void number_display_update(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!number_labels[screen_num]) return;
    
    // Format the number with appropriate precision
    char buf[32];
    if (value >= 1000.0f || value <= -1000.0f) {
        snprintf(buf, sizeof(buf), "%.0f", value);  // No decimals for large numbers
    } else if (value >= 100.0f || value <= -100.0f) {
        snprintf(buf, sizeof(buf), "%.1f", value);  // 1 decimal
    } else {
        snprintf(buf, sizeof(buf), "%.2f", value);  // 2 decimals
    }
    
    // Only update if value changed to avoid unnecessary redraws
    if (strcmp(buf, prev_number_text[screen_num]) != 0) {
        lv_label_set_text(number_labels[screen_num], buf);
        strncpy(prev_number_text[screen_num], buf, sizeof(prev_number_text[screen_num]) - 1);
        // Note: Removed re-align to prevent jumping - label is already centered at creation
    }
    
    // Update unit if provided and changed
    if (unit && unit_labels[screen_num]) {
        if (strcmp(unit, prev_unit_text[screen_num]) != 0) {
            lv_label_set_text(unit_labels[screen_num], unit);
            strncpy(prev_unit_text[screen_num], unit, sizeof(prev_unit_text[screen_num]) - 1);
        }
    }
    
    // Update description if provided and changed
    if (description && description_labels[screen_num]) {
        if (strcmp(description, prev_description_text[screen_num]) != 0) {
            lv_label_set_text(description_labels[screen_num], description);
            strncpy(prev_description_text[screen_num], description, sizeof(prev_description_text[screen_num]) - 1);
        }
    }
}

void number_display_destroy(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    if (bg_panels[screen_num]) {
        lv_obj_del(bg_panels[screen_num]);
        bg_panels[screen_num] = NULL;
    }
    
    if (number_labels[screen_num]) {
        lv_obj_del(number_labels[screen_num]);
        number_labels[screen_num] = NULL;
    }
    
    if (unit_labels[screen_num]) {
        lv_obj_del(unit_labels[screen_num]);
        unit_labels[screen_num] = NULL;
    }
    
    if (description_labels[screen_num]) {
        lv_obj_del(description_labels[screen_num]);
        description_labels[screen_num] = NULL;
    }
    
    // Restore gauge elements visibility
    lv_obj_t* top_needles[] = {ui_Needle, ui_Needle2, ui_Needle3, ui_Needle4, ui_Needle5};
    lv_obj_t* bottom_needles[] = {ui_Lower_Needle, ui_Lower_Needle2, ui_Lower_Needle3, ui_Lower_Needle4, ui_Lower_Needle5};
    lv_obj_t* top_icons[] = {ui_TopIcon1, ui_TopIcon2, ui_TopIcon3, ui_TopIcon4, ui_TopIcon5};
    lv_obj_t* bottom_icons[] = {ui_BottomIcon1, ui_BottomIcon2, ui_BottomIcon3, ui_BottomIcon4, ui_BottomIcon5};
    
    if (top_needles[screen_num]) lv_obj_clear_flag(top_needles[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (bottom_needles[screen_num]) lv_obj_clear_flag(bottom_needles[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (top_icons[screen_num]) lv_obj_clear_flag(top_icons[screen_num], LV_OBJ_FLAG_HIDDEN);
    if (bottom_icons[screen_num]) lv_obj_clear_flag(bottom_icons[screen_num], LV_OBJ_FLAG_HIDDEN);
}
