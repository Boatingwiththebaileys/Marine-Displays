#include "needle_style.h"
#include "network_setup.h"
#include "ui.h"
#include <Preferences.h>

extern Preferences preferences;

// Defaults matching current hard-coded values in main/UI
static const uint16_t DEFAULT_CX = 240;
static const uint16_t DEFAULT_CY = 240;
// Top needle defaults
static const int16_t DEFAULT_TOP_INNER = 142;
static const int16_t DEFAULT_TOP_OUTER = 210;
static const uint16_t DEFAULT_TOP_WIDTH = 10;
// Bottom needle defaults
static const int16_t DEFAULT_BOT_INNER = 142;
static const int16_t DEFAULT_BOT_OUTER = 200;
static const uint16_t DEFAULT_BOT_WIDTH = 8;

void needle_style_init_defaults() {
    // Nothing to do; defaults are applied on-the-fly
}

static String pref_key_color(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_color", s, g); return String(b); }
static String pref_key_width(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_width", s, g); return String(b); }
static String pref_key_inner(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_inner", s, g); return String(b); }
static String pref_key_outer(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_outer", s, g); return String(b); }
static String pref_key_cx(int s) { char b[32]; snprintf(b,sizeof(b),"n_s%d_cx", s); return String(b); }
static String pref_key_cy(int s) { char b[32]; snprintf(b,sizeof(b),"n_s%d_cy", s); return String(b); }
static String pref_key_rounded(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_rounded", s, g); return String(b); }
static String pref_key_gradient(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_gradient", s, g); return String(b); }
static String pref_key_fg(int s, int g) { char b[32]; snprintf(b,sizeof(b),"n_s%d_g%d_fg", s, g); return String(b); }

NeedleStyle get_needle_style(int screen, int gauge) {
    NeedleStyle s;
    // defaults
    s.cx = DEFAULT_CX;
    s.cy = DEFAULT_CY;
    if (gauge == 0) {
        s.inner = DEFAULT_TOP_INNER;
        s.outer = DEFAULT_TOP_OUTER;
        s.width = DEFAULT_TOP_WIDTH;
        s.color = String("#FFFFFF");
    } else {
        s.inner = DEFAULT_BOT_INNER;
        s.outer = DEFAULT_BOT_OUTER;
        s.width = DEFAULT_BOT_WIDTH;
        s.color = String("#FF8800");
    }
    s.rounded = false;
    s.gradient = false;
    s.foreground = true; // default foreground

    if (preferences.begin("settings", true)) {
        s.color = preferences.getString(pref_key_color(screen,gauge).c_str(), s.color);
        s.width = preferences.getUShort(pref_key_width(screen,gauge).c_str(), s.width);
        s.inner = preferences.getShort(pref_key_inner(screen,gauge).c_str(), s.inner);
        s.outer = preferences.getShort(pref_key_outer(screen,gauge).c_str(), s.outer);
        s.cx = preferences.getUShort(pref_key_cx(screen).c_str(), s.cx);
        s.cy = preferences.getUShort(pref_key_cy(screen).c_str(), s.cy);
        s.rounded = preferences.getUShort(pref_key_rounded(screen,gauge).c_str(), s.rounded ? 1 : 0) != 0;
        s.gradient = preferences.getUShort(pref_key_gradient(screen,gauge).c_str(), s.gradient ? 1 : 0) != 0;
        s.foreground = preferences.getUShort(pref_key_fg(screen,gauge).c_str(), s.foreground ? 1 : 0) != 0;
        preferences.end();
    }
    return s;
}

void apply_needle_style_to_obj(lv_obj_t* obj, int screen, int gauge) {
    if (!obj) return;
    NeedleStyle s = get_needle_style(screen, gauge);
    // Apply color
    lv_color_t c = lv_color_hex((uint32_t)strtol(s.color.substring(1).c_str(), NULL, 16));
    lv_obj_set_style_line_color(obj, c, 0);
    lv_obj_set_style_line_width(obj, s.width, 0);
    lv_obj_set_style_line_rounded(obj, s.rounded, 0);
    if (s.foreground) lv_obj_move_foreground(obj); else lv_obj_move_background(obj);
}

void apply_all_needle_styles() {
    // Top needles: ui_Needle (screen 0), ui_Needle2 (screen1)...
    apply_needle_style_to_obj(ui_Needle, 0, 0);
    apply_needle_style_to_obj(ui_Lower_Needle, 0, 1);
    apply_needle_style_to_obj(ui_Needle2, 1, 0);
    apply_needle_style_to_obj(ui_Lower_Needle2, 1, 1);
    apply_needle_style_to_obj(ui_Needle3, 2, 0);
    apply_needle_style_to_obj(ui_Lower_Needle3, 2, 1);
    apply_needle_style_to_obj(ui_Needle4, 3, 0);
    apply_needle_style_to_obj(ui_Lower_Needle4, 3, 1);
    apply_needle_style_to_obj(ui_Needle5, 4, 0);
    apply_needle_style_to_obj(ui_Lower_Needle5, 4, 1);
}

void save_needle_style_from_args(int screen, int gauge, const String& color, uint16_t width, int16_t inner, int16_t outer, uint16_t cx, uint16_t cy, bool rounded, bool gradient, bool fg) {
    if (!preferences.begin("settings", false)) return;
    preferences.putString(pref_key_color(screen,gauge).c_str(), color);
    preferences.putUShort(pref_key_width(screen,gauge).c_str(), width);
    preferences.putShort(pref_key_inner(screen,gauge).c_str(), inner);
    preferences.putShort(pref_key_outer(screen,gauge).c_str(), outer);
    preferences.putUShort(pref_key_cx(screen).c_str(), cx);
    preferences.putUShort(pref_key_cy(screen).c_str(), cy);
    preferences.putUShort(pref_key_rounded(screen,gauge).c_str(), rounded ? 1 : 0);
    preferences.putUShort(pref_key_gradient(screen,gauge).c_str(), gradient ? 1 : 0);
    preferences.putUShort(pref_key_fg(screen,gauge).c_str(), fg ? 1 : 0);
    preferences.end();
}

