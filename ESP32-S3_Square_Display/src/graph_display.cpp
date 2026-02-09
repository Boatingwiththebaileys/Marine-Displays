#include "graph_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include <Arduino.h>

// Storage for graph display objects (one per screen)
static lv_obj_t* graph_charts[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_chart_series_t* graph_series[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_chart_series_t* graph_series_2[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};  // Second series
static lv_obj_t* unit_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* description_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* unit_labels_2[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};  // Second series labels
static lv_obj_t* description_labels_2[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* bg_panels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* y_min_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};
static lv_obj_t* y_max_labels[NUM_SCREENS] = {NULL, NULL, NULL, NULL, NULL};

// Auto-ranging tracking
static float data_min[NUM_SCREENS] = {0, 0, 0, 0, 0};
static float data_max[NUM_SCREENS] = {100, 100, 100, 100, 100};
static int data_count[NUM_SCREENS] = {0, 0, 0, 0, 0};

// Time-based sampling tracking
static unsigned long last_sample_time[NUM_SCREENS] = {0, 0, 0, 0, 0};
static unsigned long sample_intervals[6] = {
    100,    // 10s: sample every 100ms (100 points)
    100,    // 30s: sample every 100ms (300 points)
    200,    // 1m: sample every 200ms (300 points)
    1000,   // 5m: sample every 1s (300 points)
    2000,   // 10m: sample every 2s (300 points)
    6000    // 30m: sample every 6s (300 points)
};
static int point_counts[6] = {100, 300, 300, 300, 300, 300};

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

void graph_display_create(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    lv_obj_t* screen = get_screen_obj(screen_num);
    if (!screen) return;
    
    // Clean up existing objects if they exist
    graph_display_destroy(screen_num);
    
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
        lv_obj_clear_flag(bg_panels[screen_num], LV_OBJ_FLAG_CLICKABLE);
    }
    
    // Get font color
    lv_color_t font_color = hex_to_lv_color(cfg.number_font_color);
    
    // Create description label (top left corner)
    description_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(description_labels[screen_num], "");
    lv_obj_set_size(description_labels[screen_num], 460, 36);
    lv_label_set_long_mode(description_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(description_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(description_labels[screen_num], font_color, 0);
    lv_obj_align(description_labels[screen_num], LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(description_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Create unit label (top right corner)
    unit_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(unit_labels[screen_num], "");
    lv_obj_set_size(unit_labels[screen_num], 200, 36);
    lv_label_set_long_mode(unit_labels[screen_num], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(unit_labels[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(unit_labels[screen_num], &inter_24, 0);
    lv_obj_set_style_text_color(unit_labels[screen_num], font_color, 0);
    lv_obj_align(unit_labels[screen_num], LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_flag(unit_labels[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    
    // Create LVGL chart (centered, takes up most of the screen)
    graph_charts[screen_num] = lv_chart_create(screen);
    lv_obj_set_size(graph_charts[screen_num], 440, 360);  // Slightly smaller to make room for labels
    lv_obj_align(graph_charts[screen_num], LV_ALIGN_CENTER, 0, 10);
    
    // Configure chart type based on configuration
    lv_chart_type_t chart_type = LV_CHART_TYPE_LINE;  // Default
    if (cfg.graph_chart_type == 1) {
        chart_type = LV_CHART_TYPE_BAR;
    } else if (cfg.graph_chart_type == 2) {
        chart_type = LV_CHART_TYPE_SCATTER;
    }
    lv_chart_set_type(graph_charts[screen_num], chart_type);
    
    // Set point count based on time range selection
    uint8_t time_range = cfg.graph_time_range;
    if (time_range > 5) time_range = 0;  // Safety check
    int points = point_counts[time_range];
    lv_chart_set_point_count(graph_charts[screen_num], points);
    
    // Start with default range (will auto-adjust)
    lv_chart_set_range(graph_charts[screen_num], LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    data_min[screen_num] = 0;
    data_max[screen_num] = 100;
    data_count[screen_num] = 0;
    
    // Style the chart
    lv_obj_set_style_bg_color(graph_charts[screen_num], lv_color_black(), 0);
    lv_obj_set_style_bg_opa(graph_charts[screen_num], LV_OPA_50, 0);
    lv_obj_set_style_border_width(graph_charts[screen_num], 0, 0);  // No border
    lv_obj_set_style_pad_all(graph_charts[screen_num], 10, 0);
    
    // Grid lines - half brightness of main color
    uint8_t r = (font_color.ch.red >> 1);    // Divide by 2 for half brightness
    uint8_t g = (font_color.ch.green >> 1);
    uint8_t b = (font_color.ch.blue >> 1);
    lv_color_t grid_color = lv_color_make(r, g, b);
    
    lv_obj_set_style_line_color(graph_charts[screen_num], grid_color, LV_PART_MAIN);
    lv_obj_set_style_line_width(graph_charts[screen_num], 1, LV_PART_MAIN);
    
    lv_chart_set_div_line_count(graph_charts[screen_num], 5, 10);  // 5 horizontal, 10 vertical grid lines
    
    // Add first data series (line color matches font color)
    graph_series[screen_num] = lv_chart_add_series(graph_charts[screen_num], font_color, LV_CHART_AXIS_PRIMARY_Y);
    
    // Add second series if path is configured
    bool has_series_2 = (strlen(cfg.graph_path_2) > 0);
    if (has_series_2) {
        lv_color_t series_2_color = hex_to_lv_color(cfg.graph_color_2);
        graph_series_2[screen_num] = lv_chart_add_series(graph_charts[screen_num], series_2_color, LV_CHART_AXIS_PRIMARY_Y);
    }
    
    // Set series style based on chart type
    if (cfg.graph_chart_type == 0) {  // Line chart
        lv_obj_set_style_line_width(graph_charts[screen_num], 3, LV_PART_ITEMS);
        lv_obj_set_style_size(graph_charts[screen_num], 0, LV_PART_INDICATOR);  // Hide point markers
    } else if (cfg.graph_chart_type == 1) {  // Bar chart
        lv_obj_set_style_size(graph_charts[screen_num], 0, LV_PART_INDICATOR);  // No point markers
    } else if (cfg.graph_chart_type == 2) {  // Scatter plot
        lv_obj_set_style_size(graph_charts[screen_num], 8, LV_PART_INDICATOR);  // Show point markers
        lv_obj_set_style_line_width(graph_charts[screen_num], 0, LV_PART_ITEMS); // No connecting lines
    }
    
    // Initialize all points to 0
    for(int i = 0; i < points; i++) {
        lv_chart_set_next_value(graph_charts[screen_num], graph_series[screen_num], 0);
        if (has_series_2) {
            lv_chart_set_next_value(graph_charts[screen_num], graph_series_2[screen_num], 0);
        }
    }
    
    // Initialize sampling timer
    last_sample_time[screen_num] = 0;
    
    // Create Y-axis labels (min and max values)
    y_min_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(y_min_labels[screen_num], "0");
    lv_obj_set_style_text_font(y_min_labels[screen_num], &inter_16, 0);
    lv_obj_set_style_text_color(y_min_labels[screen_num], font_color, 0);
    lv_obj_align(y_min_labels[screen_num], LV_ALIGN_LEFT_MID, 5, 160);  // Bottom left of chart
    
    y_max_labels[screen_num] = lv_label_create(screen);
    lv_label_set_text(y_max_labels[screen_num], "100");
    lv_obj_set_style_text_font(y_max_labels[screen_num], &inter_16, 0);
    lv_obj_set_style_text_color(y_max_labels[screen_num], font_color, 0);
    lv_obj_align(y_max_labels[screen_num], LV_ALIGN_LEFT_MID, 5, -160);  // Top left of chart
    
    // Create second series labels at bottom if configured
    if (has_series_2) {
        description_labels_2[screen_num] = lv_label_create(screen);
        lv_label_set_text(description_labels_2[screen_num], "");
        lv_obj_set_size(description_labels_2[screen_num], 240, 30);
        lv_label_set_long_mode(description_labels_2[screen_num], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(description_labels_2[screen_num], &inter_24, 0);
        lv_obj_set_style_text_color(description_labels_2[screen_num], hex_to_lv_color(cfg.graph_color_2), 0);
        lv_obj_align(description_labels_2[screen_num], LV_ALIGN_BOTTOM_LEFT, 10, -10);
        lv_obj_add_flag(description_labels_2[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
        
        unit_labels_2[screen_num] = lv_label_create(screen);
        lv_label_set_text(unit_labels_2[screen_num], "");
        lv_obj_set_size(unit_labels_2[screen_num], 200, 30);
        lv_label_set_long_mode(unit_labels_2[screen_num], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(unit_labels_2[screen_num], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(unit_labels_2[screen_num], &inter_24, 0);
        lv_obj_set_style_text_color(unit_labels_2[screen_num], hex_to_lv_color(cfg.graph_color_2), 0);
        lv_obj_align(unit_labels_2[screen_num], LV_ALIGN_BOTTOM_RIGHT, -10, -10);
        lv_obj_add_flag(unit_labels_2[screen_num], LV_OBJ_FLAG_IGNORE_LAYOUT);
    }
    
    Serial.printf("[GRAPH_DISPLAY] Created for screen %d (type=%d, series2=%d)\n", 
                  screen_num, cfg.graph_chart_type, has_series_2 ? 1 : 0);
}

void graph_display_update(int screen_num, float value, const char* unit, const char* description,
                          float value2, const char* unit2, const char* description2) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    if (!graph_charts[screen_num] || !graph_series[screen_num]) return;
    
    bool has_series_2 = (graph_series_2[screen_num] != NULL && !isnan(value2));
    
    // Check if enough time has passed based on selected time range
    uint8_t time_range = screen_configs[screen_num].graph_time_range;
    if (time_range > 5) time_range = 0;
    
    unsigned long now = millis();
    unsigned long interval = sample_intervals[time_range];
    
    // Only add a new sample if enough time has elapsed
    if (last_sample_time[screen_num] == 0 || (now - last_sample_time[screen_num]) >= interval) {
        last_sample_time[screen_num] = now;
        
        // Update description label
        if (description_labels[screen_num] && description) {
            lv_label_set_text(description_labels[screen_num], description);
        }
        
        // Update unit label
        if (unit_labels[screen_num] && unit) {
            lv_label_set_text(unit_labels[screen_num], unit);
        }
        
        // Update second series labels if present
        if (has_series_2) {
            if (description_labels_2[screen_num] && description2) {
                lv_label_set_text(description_labels_2[screen_num], description2);
            }
            if (unit_labels_2[screen_num] && unit2) {
                lv_label_set_text(unit_labels_2[screen_num], unit2);
            }
        }
        
        // Add new values to chart first
        int32_t scaled_value = (int32_t)value;
        lv_chart_set_next_value(graph_charts[screen_num], graph_series[screen_num], scaled_value);
        
        if (has_series_2) {
            int32_t scaled_value2 = (int32_t)value2;
            lv_chart_set_next_value(graph_charts[screen_num], graph_series_2[screen_num], scaled_value2);
        }
        
        // Recalculate min/max from actual chart data (so range can shrink when data decreases)
        uint16_t point_count = lv_chart_get_point_count(graph_charts[screen_num]);
        lv_coord_t* y_array = lv_chart_get_y_array(graph_charts[screen_num], graph_series[screen_num]);
        
        lv_coord_t actual_min = y_array[0];
        lv_coord_t actual_max = y_array[0];
        
        // Find min/max from first series
        for (uint16_t i = 1; i < point_count; i++) {
            if (y_array[i] < actual_min) actual_min = y_array[i];
            if (y_array[i] > actual_max) actual_max = y_array[i];
        }
        
        // Include second series if present
        if (has_series_2) {
            lv_coord_t* y_array2 = lv_chart_get_y_array(graph_charts[screen_num], graph_series_2[screen_num]);
            for (uint16_t i = 0; i < point_count; i++) {
                if (y_array2[i] < actual_min) actual_min = y_array2[i];
                if (y_array2[i] > actual_max) actual_max = y_array2[i];
            }
        }
        
        // Add 10% margin to range for better visualization
        float range = (float)(actual_max - actual_min);
        if (range < 0.1f) range = 0.1f;  // Minimum range to avoid division by zero
        float margin = range * 0.1f;
        int32_t y_min = (int32_t)((float)actual_min - margin);
        int32_t y_max = (int32_t)((float)actual_max + margin);
        
        // Update Y-axis range
        lv_chart_set_range(graph_charts[screen_num], LV_CHART_AXIS_PRIMARY_Y, y_min, y_max);
        
        // Update Y-axis labels
        if (y_min_labels[screen_num]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", y_min);
            lv_label_set_text(y_min_labels[screen_num], buf);
        }
        if (y_max_labels[screen_num]) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", y_max);
            lv_label_set_text(y_max_labels[screen_num], buf);
        }
        
        lv_chart_refresh(graph_charts[screen_num]);
    }
}

void graph_display_destroy(int screen_num) {
    if (screen_num < 0 || screen_num >= NUM_SCREENS) return;
    
    if (graph_charts[screen_num]) {
        lv_obj_del(graph_charts[screen_num]);
        graph_charts[screen_num] = NULL;
        graph_series[screen_num] = NULL;  // Series is deleted with chart
    }
    
    if (unit_labels[screen_num]) {
        lv_obj_del(unit_labels[screen_num]);
        unit_labels[screen_num] = NULL;
    }
    
    if (description_labels[screen_num]) {
        lv_obj_del(description_labels[screen_num]);
        description_labels[screen_num] = NULL;
    }
    
    if (y_min_labels[screen_num]) {
        lv_obj_del(y_min_labels[screen_num]);
        y_min_labels[screen_num] = NULL;
    }
    
    if (y_max_labels[screen_num]) {
        lv_obj_del(y_max_labels[screen_num]);
        y_max_labels[screen_num] = NULL;
    }
    
    if (bg_panels[screen_num]) {
        lv_obj_del(bg_panels[screen_num]);
        bg_panels[screen_num] = NULL;
    }
    
    // Reset auto-ranging data
    data_min[screen_num] = 0;
    data_max[screen_num] = 100;
    data_count[screen_num] = 0;
}
