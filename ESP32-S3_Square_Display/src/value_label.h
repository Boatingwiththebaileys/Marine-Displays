#pragma once
// value_label.h — shared by the Number, Dual and Quad screens.
//
// Numbers always use the font size chosen in the settings. Text values (e.g.
// "motoring", "auto", "float") can be much wider, so they drop to the largest
// smaller font that fits the box, and are cut short with ".." if even the
// smallest font is too wide. Text never wraps onto a second line.

#include "lvgl.h"
#include "ui.h"
#include <string.h>

// Largest first. The configured font is always one of these.
static const lv_font_t* const VALUE_FONT_LADDER[] = {
    &inter_144, &inter_120, &inter_96, &inter_72, &inter_48, &inter_24,
};

static inline lv_coord_t value_text_width(const char* txt, const lv_font_t* f) {
    lv_point_t sz;
    lv_txt_get_size(&sz, txt, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

// Font for this text: the configured one for numbers; for text, the largest
// font no bigger than the configured one that fits in max_w.
static inline const lv_font_t* value_font_for(const char* txt, bool is_text,
                                              const lv_font_t* cfg, lv_coord_t max_w) {
    if (!is_text) return cfg;
    bool started = false;
    for (const lv_font_t* f : VALUE_FONT_LADDER) {
        if (f == cfg) started = true;
        if (!started) continue;
        if (value_text_width(txt, f) <= max_w) return f;
    }
    return &inter_24;
}

// Copy txt into out, cut short with ".." so it fits max_w in font f.
static inline void value_fit_text(char* out, size_t out_sz, const char* txt,
                                  const lv_font_t* f, lv_coord_t max_w) {
    strncpy(out, txt, out_sz - 1);
    out[out_sz - 1] = '\0';
    if (value_text_width(out, f) <= max_w) return;
    size_t n = strlen(out);
    while (n > 0) {
        n--;
        // don't leave half of a UTF-8 character behind
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
        if (n + 3 > out_sz) continue;
        out[n] = '.'; out[n + 1] = '.'; out[n + 2] = '\0';
        if (value_text_width(out, f) <= max_w) return;
    }
}

// Set a value label's text, picking the font as above. Returns the font used.
static inline const lv_font_t* set_value_label(lv_obj_t* lbl, const char* txt, bool is_text,
                                               const lv_font_t* cfg, lv_coord_t max_w) {
    const lv_font_t* f = value_font_for(txt, is_text, cfg, max_w);
    if (lv_obj_get_style_text_font(lbl, 0) != f) lv_obj_set_style_text_font(lbl, f, 0);
    if (is_text) {
        char buf[64];
        value_fit_text(buf, sizeof(buf), txt, f, max_w);
        lv_label_set_text(lbl, buf);
    } else {
        lv_label_set_text(lbl, txt);
    }
    return f;
}
