#include "gauge_number_display.h"
#include "screen_config_c_api.h"
#include "ui.h"
#include <Arduino.h>
#include <stdio.h>

// Storage for gauge+number display components (center number for each screen)
static lv_obj_t* gauge_num_center_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* gauge_num_center_unit_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* gauge_num_center_description_labels[NUM_SCREENS] = {nullptr};
static lv_obj_t* gauge_num_containers[NUM_SCREENS] = {nullptr};

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
static char prev_gauge_num_center_text[NUM_SCREENS][64] = {0};
static char prev_gauge_num_center_unit[NUM_SCREENS][32] = {0};
static char prev_gauge_num_center_description[NUM_SCREENS][128] = {0};

// Font size to LVGL font mapping
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

// Convert description to acronym (first letter of each word, uppercase)
static String description_to_acronym(const char* description) {
    if (!description || description[0] == '\0') return String("");
    
    String result = "";
    bool new_word = true;
    
    for (int i = 0; description[i] != '\0'; i++) {
        char c = description[i];
        
        if (c == ' ' || c == '-' || c == '_') {
            new_word = true;
        } else if (new_word) {
            // Add uppercase first letter of word
            if (c >= 'a' && c <= 'z') {
                result += (char)(c - 32);  // Convert to uppercase
            } else {
                result += c;  // Already uppercase or not a letter
            }
            new_word = false;
        }
    }
    
    return result;
}

void gauge_number_display_create(int screen_num, 
                                   uint8_t center_font_size, 
                                   const char* center_font_color) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    lv_obj_t* screen = get_screen_obj(screen_num);
    if (!screen) return;
    
    // Clean up existing display if any
    gauge_number_display_destroy(screen_num);
    
    // Create container for center number display on the correct screen
    // Full screen container with transparent background so gauge shows through
    gauge_num_containers[screen_num] = lv_obj_create(screen);
    lv_obj_set_size(gauge_num_containers[screen_num], 480, 480);  // Full screen
    lv_obj_align(gauge_num_containers[screen_num], LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(gauge_num_containers[screen_num], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(gauge_num_containers[screen_num], LV_OBJ_FLAG_CLICKABLE);  // Allow swipe through
    // Transparent background so gauge shows through
    lv_obj_set_style_pad_all(gauge_num_containers[screen_num], 0, 0);
    lv_obj_set_style_bg_opa(gauge_num_containers[screen_num], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gauge_num_containers[screen_num], 0, 0);
    lv_obj_set_style_radius(gauge_num_containers[screen_num], 0, 0);
    
    // Center description label (top-left of screen)
    gauge_num_center_description_labels[screen_num] = lv_label_create(gauge_num_containers[screen_num]);
    lv_label_set_text(gauge_num_center_description_labels[screen_num], "");
    lv_obj_set_size(gauge_num_center_description_labels[screen_num], 460, 36);
    lv_label_set_long_mode(gauge_num_center_description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gauge_num_center_description_labels[screen_num], LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(gauge_num_center_description_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(gauge_num_center_description_labels[screen_num], parse_hex_color(center_font_color), 0);
    lv_obj_align(gauge_num_center_description_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(gauge_num_center_description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Center value label (centered in middle of entire screen)
    gauge_num_center_labels[screen_num] = lv_label_create(gauge_num_containers[screen_num]);
    lv_label_set_text(gauge_num_center_labels[screen_num], "---");
    lv_obj_set_size(gauge_num_center_labels[screen_num], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_long_mode(gauge_num_center_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gauge_num_center_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(gauge_num_center_labels[screen_num], get_font_for_size(center_font_size), 0);
    lv_obj_set_style_text_color(gauge_num_center_labels[screen_num], parse_hex_color(center_font_color), 0);
    lv_obj_set_style_transform_pivot_x(gauge_num_center_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(gauge_num_center_labels[screen_num], LV_PCT(50), 0);
    lv_obj_set_style_transform_zoom(gauge_num_center_labels[screen_num], get_zoom_for_size(center_font_size), 0);
    lv_obj_align(gauge_num_center_labels[screen_num], LV_ALIGN_CENTER, -20, 0);  // Slight left offset for unit spacing
    lv_obj_add_flag(gauge_num_center_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Center unit label (positioned right after the number)
    gauge_num_center_unit_labels[screen_num] = lv_label_create(gauge_num_containers[screen_num]);
    lv_label_set_text(gauge_num_center_unit_labels[screen_num], "");
    lv_obj_set_size(gauge_num_center_unit_labels[screen_num], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_label_set_long_mode(gauge_num_center_unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(gauge_num_center_unit_labels[screen_num], LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(gauge_num_center_unit_labels[screen_num], &inter_48, 0);  // Larger unit font
    lv_obj_set_style_text_color(gauge_num_center_unit_labels[screen_num], parse_hex_color(center_font_color), 0);
    lv_obj_align_to(gauge_num_center_unit_labels[screen_num], gauge_num_center_labels[screen_num], LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_add_flag(gauge_num_center_unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Clear previous values
    prev_gauge_num_center_text[screen_num][0] = '\0';
    prev_gauge_num_center_unit[screen_num][0] = '\0';
    prev_gauge_num_center_description[screen_num][0] = '\0';
}

void gauge_number_display_update_center(int screen_num, float value, const char* unit, const char* description) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!gauge_num_center_labels[screen_num]) return;
    
    // Format value text
    char text[64];
    snprintf(text, sizeof(text), "%.1f", value);
    
    // Only update if changed
    bool needs_update = false;
    if (strcmp(text, prev_gauge_num_center_text[screen_num]) != 0) {
        lv_label_set_text(gauge_num_center_labels[screen_num], text);
        strncpy(prev_gauge_num_center_text[screen_num], text, sizeof(prev_gauge_num_center_text[screen_num]) - 1);
        needs_update = true;
    }
    
    if (strcmp(unit, prev_gauge_num_center_unit[screen_num]) != 0) {
        lv_label_set_text(gauge_num_center_unit_labels[screen_num], unit);
        strncpy(prev_gauge_num_center_unit[screen_num], unit, sizeof(prev_gauge_num_center_unit[screen_num]) - 1);
        needs_update = true;
    }
    
    // Re-align number and unit whenever either changes to keep them centered together
    if (needs_update) {
        lv_obj_align(gauge_num_center_labels[screen_num], LV_ALIGN_CENTER, -20, 0);
        lv_obj_align_to(gauge_num_center_unit_labels[screen_num], gauge_num_center_labels[screen_num], LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    }
    
    if (strcmp(description, prev_gauge_num_center_description[screen_num]) != 0) {
        // Convert description to acronym (e.g., "Apparent wind speed" -> "AWS")
        String acronym = description_to_acronym(description);
        lv_label_set_text(gauge_num_center_description_labels[screen_num], acronym.c_str());
        strncpy(prev_gauge_num_center_description[screen_num], description, sizeof(prev_gauge_num_center_description[screen_num]) - 1);
    }
}

void gauge_number_display_destroy(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    if (gauge_num_containers[screen_num]) {
        lv_obj_del(gauge_num_containers[screen_num]);
        gauge_num_containers[screen_num] = nullptr;
    }
    
    // Child objects are automatically deleted with parent container
    gauge_num_center_labels[screen_num] = nullptr;
    gauge_num_center_unit_labels[screen_num] = nullptr;
    gauge_num_center_description_labels[screen_num] = nullptr;
    
    // Clear previous values
    prev_gauge_num_center_text[screen_num][0] = '\0';
    prev_gauge_num_center_unit[screen_num][0] = '\0';
    prev_gauge_num_center_description[screen_num][0] = '\0';
}
