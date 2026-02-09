#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "lvgl.h"

struct NeedleStyle {
    String color;      // hex string like #RRGGBB
    uint16_t width;    // px
    int16_t inner;     // inner radius (px)
    int16_t outer;     // outer radius (px)
    uint16_t cx;       // center X
    uint16_t cy;       // center Y
    bool rounded;      // rounded end caps
    bool gradient;     // pseudo-gradient enabled (not implemented fully)
    bool foreground;   // true -> move to foreground
};

// Initialize defaults (called internally)
void needle_style_init_defaults();

// Return the style for given screen (0-based) and gauge (0=top,1=bottom)
NeedleStyle get_needle_style(int screen, int gauge);

// Apply style to a specific lv line object
void apply_needle_style_to_obj(lv_obj_t* obj, int screen, int gauge);

// Apply styles to all needle objects (ui_Needle, ui_Needle2, ...)
void apply_all_needle_styles();

// Persist settings via Preferences (namespace "settings") - helpers used by WebUI
void save_needle_style_from_args(int screen, int gauge, const String& color, uint16_t width, int16_t inner, int16_t outer, uint16_t cx, uint16_t cy, bool rounded, bool gradient, bool fg);
