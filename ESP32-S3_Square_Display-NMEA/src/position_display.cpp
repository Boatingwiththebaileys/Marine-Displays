#include "position_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ── Storage ──────────────────────────────────────────────────────────────────
static lv_obj_t* p_bg[NUM_SCREENS]       = {};
static lv_obj_t* p_title[NUM_SCREENS]    = {};
static lv_obj_t* p_lat_lbl[NUM_SCREENS]  = {};   // small "LAT"
static lv_obj_t* p_lat_val[NUM_SCREENS]  = {};   // large value
static lv_obj_t* p_lon_lbl[NUM_SCREENS]  = {};   // small "LON"
static lv_obj_t* p_lon_val[NUM_SCREENS]  = {};   // large value
static lv_obj_t* p_time_lbl[NUM_SCREENS] = {};   // "14:32:07 UTC"
static lv_obj_t* p_date_lbl[NUM_SCREENS] = {};   // "2026-03-04"

// ── Colour helpers ──────────────────────────────────────────────────────────
static lv_color_t hex_to_lv_color(const char* hex, lv_color_t fallback) {
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return fallback;
    unsigned int r = 0, g = 0, b = 0;
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ── Helpers ──────────────────────────────────────────────────────────────────
static lv_obj_t* screen_obj(int n) {
    switch (n) {
        case 0: return ui_Screen1; case 1: return ui_Screen2;
        case 2: return ui_Screen3; case 3: return ui_Screen4;
        case 4: return ui_Screen5; default: return NULL;
    }
}

// 0 = DD  : "51.503250° N"
// 1 = DMS : "51° 30' 11.7\" N"
// 2 = DDM : "51° 30.195' N"
static void format_latlon(float dec, bool is_lat, int fmt, char* out, size_t sz) {
    if (isnan(dec)) {
        if (fmt == 0) snprintf(out, sz, "---.------\xc2\xb0 -");
        else if (fmt == 1) snprintf(out, sz, "---\xc2\xb0 --' --\"  -");
        else            snprintf(out, sz, "---\xc2\xb0 --.---' -");
        return;
    }
    bool neg      = (dec < 0.0f);
    float abs_dec = fabsf(dec);
    char  hem     = is_lat ? (neg ? 'S' : 'N') : (neg ? 'W' : 'E');

    if (fmt == 0) {
        // Decimal Degrees: "51.503250° N"
        if (is_lat)
            snprintf(out, sz, "%09.6f\xc2\xb0 %c", abs_dec, hem);
        else
            snprintf(out, sz, "%010.6f\xc2\xb0 %c", abs_dec, hem);
    } else if (fmt == 1) {
        // Degrees, Minutes, Seconds: "51° 30' 11.7\" N"
        int   deg  = (int)abs_dec;
        float fmin = (abs_dec - deg) * 60.0f;
        int   min  = (int)fmin;
        float sec  = (fmin - min) * 60.0f;
        if (is_lat)
            snprintf(out, sz, "%02d\xc2\xb0 %02d' %04.1f\" %c", deg, min, sec, hem);
        else
            snprintf(out, sz, "%03d\xc2\xb0 %02d' %04.1f\" %c", deg, min, sec, hem);
    } else {
        // Degrees, Decimal Minutes (default): "51° 30.195' N"
        int   deg = (int)abs_dec;
        float min = (abs_dec - deg) * 60.0f;
        if (is_lat)
            snprintf(out, sz, "%02d\xc2\xb0 %06.3f' %c", deg, min, hem);
        else
            snprintf(out, sz, "%03d\xc2\xb0 %06.3f' %c", deg, min, hem);
    }
}

// Extract time "HH:MM:SS" from ISO string "2026-03-04T14:32:07.000Z"
static void parse_time(const char* dt, char* time_out, size_t tsz,
                       char* date_out, size_t dsz) {
    if (!dt || strlen(dt) < 19) {
        snprintf(time_out, tsz, "--:--:--");
        snprintf(date_out, dsz, "----/--/--");
        return;
    }
    // ISO format: "2026-03-04T14:32:07..."
    snprintf(date_out, dsz, "%.10s", dt); // "2026-03-04"
    snprintf(time_out, tsz, "%.8s", dt + 11); // "14:32:07"
}

// ── API ───────────────────────────────────────────────────────────────────────
void position_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    position_display_destroy(n);

    lv_obj_t* scr = screen_obj(n);
    if (!scr) return;

    // Hide gauge/icon objects
    extern lv_obj_t* ui_Needle;    extern lv_obj_t* ui_Needle2;
    extern lv_obj_t* ui_Needle3;   extern lv_obj_t* ui_Needle4;
    extern lv_obj_t* ui_Needle5;
    extern lv_obj_t* ui_Lower_Needle;  extern lv_obj_t* ui_Lower_Needle2;
    extern lv_obj_t* ui_Lower_Needle3; extern lv_obj_t* ui_Lower_Needle4;
    extern lv_obj_t* ui_Lower_Needle5;
    extern lv_obj_t* ui_TopIcon1; extern lv_obj_t* ui_TopIcon2;
    extern lv_obj_t* ui_TopIcon3; extern lv_obj_t* ui_TopIcon4;
    extern lv_obj_t* ui_TopIcon5;
    extern lv_obj_t* ui_BottomIcon1; extern lv_obj_t* ui_BottomIcon2;
    extern lv_obj_t* ui_BottomIcon3; extern lv_obj_t* ui_BottomIcon4;
    extern lv_obj_t* ui_BottomIcon5;
    lv_obj_t* tn[] = {ui_Needle,ui_Needle2,ui_Needle3,ui_Needle4,ui_Needle5};
    lv_obj_t* bn[] = {ui_Lower_Needle,ui_Lower_Needle2,ui_Lower_Needle3,
                      ui_Lower_Needle4,ui_Lower_Needle5};
    lv_obj_t* ti[] = {ui_TopIcon1,ui_TopIcon2,ui_TopIcon3,ui_TopIcon4,ui_TopIcon5};
    lv_obj_t* bi[] = {ui_BottomIcon1,ui_BottomIcon2,ui_BottomIcon3,
                      ui_BottomIcon4,ui_BottomIcon5};
    if (tn[n]) lv_obj_add_flag(tn[n], LV_OBJ_FLAG_HIDDEN);
    if (bn[n]) lv_obj_add_flag(bn[n], LV_OBJ_FLAG_HIDDEN);
    if (ti[n]) lv_obj_add_flag(ti[n], LV_OBJ_FLAG_HIDDEN);
    if (bi[n]) lv_obj_add_flag(bi[n], LV_OBJ_FLAG_HIDDEN);

    // Background — respects 'Custom Color' selection (same mechanism as number display)
    p_bg[n] = lv_obj_create(scr);
    lv_obj_set_size(p_bg[n], LV_PCT(100), LV_PCT(100));
    {
        lv_color_t bg_col = lv_color_black();
        if (strcmp(screen_configs[n].background_path, "Custom Color") == 0)
            bg_col = hex_to_lv_color(screen_configs[n].number_bg_color, lv_color_black());
        lv_obj_set_style_bg_color(p_bg[n], bg_col, 0);
    }
    lv_obj_set_style_bg_opa(p_bg[n], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p_bg[n], 0, 0);
    lv_obj_clear_flag(p_bg[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(p_bg[n], 0, 0);

    // Resolved colours (fall back to original hardcoded values if fields are empty)
    lv_color_t col_latlon  = hex_to_lv_color(screen_configs[n].pos_latlon_color,  lv_color_white());
    lv_color_t col_time    = hex_to_lv_color(screen_configs[n].pos_time_color,    lv_color_make(100, 220, 180));
    lv_color_t col_divider = hex_to_lv_color(screen_configs[n].pos_divider_color, lv_color_make(50,  70,  120));

    // Title
    p_title[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_title[n], "POSITION & TIME");
    lv_obj_set_style_text_font(p_title[n], &inter_24, 0);
    lv_obj_set_style_text_color(p_title[n], col_divider, 0);
    lv_obj_align(p_title[n], LV_ALIGN_TOP_MID, 0, 12);

    // Divider line (thin horizontal rect)
    lv_obj_t* div = lv_obj_create(p_bg[n]);
    lv_obj_set_size(div, 440, 2);
    lv_obj_align(div, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_bg_color(div, col_divider, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // "LAT" small label
    p_lat_lbl[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_lat_lbl[n], "LAT");
    lv_obj_set_style_text_font(p_lat_lbl[n], &inter_24, 0);
    lv_obj_set_style_text_color(p_lat_lbl[n], col_latlon, 0);
    lv_obj_align(p_lat_lbl[n], LV_ALIGN_TOP_LEFT, 24, 60);

    // Latitude value
    p_lat_val[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_lat_val[n], "---° --.---' -");
    lv_obj_set_style_text_font(p_lat_val[n], &inter_48, 0);
    lv_obj_set_style_text_color(p_lat_val[n], col_latlon, 0);
    lv_obj_align(p_lat_val[n], LV_ALIGN_TOP_MID, 0, 86);

    // "LON" small label
    p_lon_lbl[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_lon_lbl[n], "LON");
    lv_obj_set_style_text_font(p_lon_lbl[n], &inter_24, 0);
    lv_obj_set_style_text_color(p_lon_lbl[n], col_latlon, 0);
    lv_obj_align(p_lon_lbl[n], LV_ALIGN_TOP_LEFT, 24, 172);

    // Longitude value
    p_lon_val[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_lon_val[n], "---° --.---' -");
    lv_obj_set_style_text_font(p_lon_val[n], &inter_48, 0);
    lv_obj_set_style_text_color(p_lon_val[n], col_latlon, 0);
    lv_obj_align(p_lon_val[n], LV_ALIGN_TOP_MID, 0, 198);

    // Second divider
    lv_obj_t* div2 = lv_obj_create(p_bg[n]);
    lv_obj_set_size(div2, 440, 2);
    lv_obj_align(div2, LV_ALIGN_TOP_MID, 0, 285);
    lv_obj_set_style_bg_color(div2, col_divider, 0);
    lv_obj_set_style_border_width(div2, 0, 0);
    lv_obj_clear_flag(div2, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // UTC time label
    p_time_lbl[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_time_lbl[n], "--:--:-- UTC");
    lv_obj_set_style_text_font(p_time_lbl[n], &inter_48, 0);
    lv_obj_set_style_text_color(p_time_lbl[n], col_time, 0);
    lv_obj_align(p_time_lbl[n], LV_ALIGN_TOP_MID, 0, 300);

    // Date label
    p_date_lbl[n] = lv_label_create(p_bg[n]);
    lv_label_set_text(p_date_lbl[n], "----/--/--");
    lv_obj_set_style_text_font(p_date_lbl[n], &inter_24, 0);
    lv_obj_set_style_text_color(p_date_lbl[n], lv_color_make(150, 150, 150), 0);
    lv_obj_align(p_date_lbl[n], LV_ALIGN_TOP_MID, 0, 364);
}

void position_display_update(int n, float lat, float lon,
                             const char* datetime_str) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (!p_lat_val[n]) return;

    int fmt = (int)screen_configs[n].number_font_size; // 0=DD, 1=DMS, 2=DDM

    char buf[48];
    format_latlon(lat, true,  fmt, buf, sizeof(buf));
    lv_label_set_text(p_lat_val[n], buf);

    format_latlon(lon, false, fmt, buf, sizeof(buf));
    lv_label_set_text(p_lon_val[n], buf);

    char time_buf[16], date_buf[16];
    parse_time(datetime_str, time_buf, sizeof(time_buf),
               date_buf, sizeof(date_buf));

    snprintf(buf, sizeof(buf), "%s UTC", time_buf);
    lv_label_set_text(p_time_lbl[n], buf);
    lv_label_set_text(p_date_lbl[n], date_buf);
}

void position_display_destroy(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (p_bg[n]) { lv_obj_del(p_bg[n]); p_bg[n] = NULL; }
    p_title[n] = p_lat_lbl[n] = p_lat_val[n] = NULL;
    p_lon_lbl[n] = p_lon_val[n] = NULL;
    p_time_lbl[n] = p_date_lbl[n] = NULL;
}

// Lifecycle hooks called by ui_hotupdate to track active compass/position displays.
// Currently no-op; can be wired to SignalK subscription management in future.
extern "C" void mark_position_display_created(int /*screen_num*/) {}
extern "C" void mark_position_display_destroyed(int /*screen_num*/) {}
