#include "gauge_number_display.h"
#include "screen_config_c_api.h"
#include "unit_convert.h"
#include "ui.h"
#include <Arduino.h>
#include <stdio.h>
#include <esp_attr.h>

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
// EXT_RAM_ATTR → PSRAM, freeing ~1.1 KB of internal RAM
EXT_RAM_ATTR static char prev_gauge_num_center_text[NUM_SCREENS][64];
EXT_RAM_ATTR static char prev_gauge_num_center_unit[NUM_SCREENS][32];
EXT_RAM_ATTR static char prev_gauge_num_center_description[NUM_SCREENS][128];

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

// Standard marine abbreviations keyed by the n2k_field_label() strings
struct AbbrevEntry { const char* label; const char* abbrev; };
static const AbbrevEntry marine_abbrevs[] = {
    // Engine #1
    {"Engine RPM",              "RPM"},
    {"Engine Boost",            "BOOST"},
    {"Tilt / Trim",             "TRIM"},
    {"Oil Pressure",            "OIL P"},
    {"Oil Temperature",         "OIL T"},
    {"Coolant Temperature",     "CLT"},
    {"Alternator Voltage",      "ALT V"},
    {"Fuel Rate",               "FUEL R"},
    {"Engine Hours",            "HRS"},
    {"Engine Load",             "LOAD"},
    {"Engine Torque",           "TRQ"},
    {"Exhaust Temperature",     "EGT"},
    // Engine #2
    {"Engine 2 RPM",            "RPM 2"},
    {"Engine 2 Oil Pressure",   "OIL P2"},
    {"Engine 2 Oil Temp",       "OIL T2"},
    {"Engine 2 Coolant Temp",   "CLT 2"},
    {"Engine 2 Alt Voltage",    "ALT V2"},
    {"Engine 2 Fuel Rate",      "FUEL R2"},
    {"Engine 2 Hours",          "HRS 2"},
    {"Engine 2 Exhaust Temp",   "EGT 2"},
    // Transmission
    {"Transmission Gear",       "GEAR"},
    {"Trans Oil Pressure",      "TR OIL"},
    {"Trans Oil Temp",          "TR TMP"},
    // Trip / Fuel
    {"Trip Fuel Used",          "TRIP F"},
    {"Avg Fuel Rate",           "AVG FR"},
    {"Fuel Economy",            "ECON"},
    {"Fuel Level",              "FUEL"},
    {"Fuel Capacity",           "FUEL CAP"},
    // Fluid levels
    {"Fresh Water Level",       "FW LVL"},
    {"Waste Water Level",       "WW LVL"},
    {"Oil Level",               "OIL LVL"},
    {"Black Water Level",       "BW LVL"},
    {"Fresh Water Capacity",    "FW CAP"},
    {"Live Well Level",         "LWELL"},
    // Navigation
    {"Heading",                 "HDG"},
    {"Mag Deviation",           "DEV"},
    {"Mag Variation (Hdg)",     "VAR"},
    {"Rate of Turn",            "ROT"},
    {"Magnetic Variation",      "VAR"},
    {"Latitude",                "LAT"},
    {"Longitude",               "LON"},
    {"COG",                     "COG"},
    {"SOG",                     "SOG"},
    {"GNSS Date/Time",          "GPS DT"},
    {"GNSS Altitude",           "GPS ALT"},
    {"GNSS Satellites",         "SATS"},
    {"GNSS HDOP",               "HDOP"},
    {"Cross Track Error",       "XTE"},
    {"Waypoint Distance",       "WPT D"},
    {"Waypoint Bearing",        "WPT B"},
    {"VMG to Waypoint",         "VMG"},
    {"ETA",                     "ETA"},
    {"Speed (Water)",           "STW"},
    {"Speed (Ground)",          "SOG"},
    {"Total Log",               "LOG"},
    {"Trip Log",                "TRIP"},
    {"Leeway",                  "LEE"},
    // Depth
    {"Water Depth",             "DPT"},
    {"Depth Offset",            "OFFS"},
    // Wind
    {"Wind Speed (Apparent)",   "AWS"},
    {"Wind Angle (Apparent)",   "AWA"},
    {"Wind Speed (True)",       "TWS"},
    {"Wind Angle (True)",       "TWA"},
    // Environment
    {"Temperature",             "TEMP"},
    {"Sea Temperature",         "SEA T"},
    {"Set Temperature",         "SET T"},
    {"Outside Air Temp",        "AIR T"},
    {"Humidity",                "HUM"},
    {"Atmospheric Pressure",    "BARO"},
    {"Barometric Pressure",     "BARO"},
    // Attitude
    {"Pitch",                   "PITCH"},
    {"Roll",                    "ROLL"},
    {"Yaw",                     "YAW"},
    // Rudder / Trim tabs
    {"Rudder Position",         "RDR"},
    {"Trim Tab (Port)",         "TT P"},
    {"Trim Tab (Starboard)",    "TT S"},
    // Battery / Electrical
    {"Battery Voltage",         "BAT V"},
    {"Battery Current",         "BAT A"},
    {"Battery Temperature",     "BAT T"},
    {"Battery 2 Voltage",       "BAT V2"},
    {"Battery 2 Current",       "BAT A2"},
    {"Battery SOC",             "SOC"},
    {"Battery Time Remaining",  "BAT TR"},
    {"DC Source Voltage",       "DC V"},
    {"DC Source Current",       "DC A"},
    // Time
    {"System Time",             "TIME"},
    {nullptr, nullptr}
};

static String description_to_acronym(const char* description) {
    if (!description || description[0] == '\0') return String("");

    // Look up standard marine abbreviation
    for (const AbbrevEntry* e = marine_abbrevs; e->label; ++e) {
        if (strcmp(description, e->label) == 0)
            return String(e->abbrev);
    }

    // Fallback: first letter of each word, uppercased
    String result = "";
    bool new_word = true;
    for (int i = 0; description[i] != '\0'; i++) {
        char c = description[i];
        if (c == ' ' || c == '-' || c == '_') {
            new_word = true;
        } else if (new_word) {
            result += (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
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
    format_display_value(text, sizeof(text), value, description);
    
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
