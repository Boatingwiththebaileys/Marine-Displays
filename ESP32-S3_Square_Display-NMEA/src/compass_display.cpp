#include "compass_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include "unit_convert.h"
#include <Arduino.h>
#include <math.h>
#include <esp_attr.h>

// ─── Sweeping-tape compass geometry ───────────────────────────────────────────
// The compass rose lives on a large virtual arc whose pivot sits BELOW the
// 480×480 screen.  As heading changes the tape sweeps left/right, with a fixed
// triangle pointer at the top centre — identical to the Raymarine p70/p70s.
//
//  PIVOT:  (240, 570)  ← 90 px below the bottom edge
//  ARC_R:  390 px      → tape top at y = 570-390 = 180
//  Visible half-angle: arcsin(240/390) ≈ 38°  (screen clips the rest)
// ─────────────────────────────────────────────────────────────────────────────
#define PIVOT_X         240
#define PIVOT_Y         570
#define Y_SHIFT           5     // shift entire tape up: arc top = 570-550-5 = 15 px
#define ARC_R_OUT       550     // outer edge of tape  (flatter: half-angle ≈26° vs 38°)
#define ARC_R_MAJ_IN    518     // major tick (10°) inner end  → 32 px long
#define ARC_R_MIN_IN    535     // minor tick (5°)  inner end  → 15 px long
#define ARC_R_LABEL     480     // label centre (inside tape)

#define TICK_HALF        50
#define TICK_STEP         5
#define N_TICKS          21     // (50*2/5)+1

#define LBL_HALF         45
#define LBL_STEP         10
#define N_LABELS         10     // (45*2/10)+1 + 1 safety

// ─── LVGL objects ─────────────────────────────────────────────────────────────
static lv_obj_t*   c_bg[NUM_SCREENS]                   = {};
static lv_obj_t*   c_tape_arc[NUM_SCREENS]             = {};
static lv_obj_t*   c_tick[NUM_SCREENS][N_TICKS]        = {};
static EXT_RAM_ATTR lv_point_t  c_tick_pts[NUM_SCREENS][N_TICKS][2];
static lv_obj_t*   c_deg_lbl[NUM_SCREENS][N_LABELS]    = {};
static lv_obj_t*   c_ptr_l[NUM_SCREENS]                = {};
static lv_obj_t*   c_ptr_r[NUM_SCREENS]                = {};
static EXT_RAM_ATTR lv_point_t  c_ptr_pts_l[NUM_SCREENS][2];
static EXT_RAM_ATTR lv_point_t  c_ptr_pts_r[NUM_SCREENS][2];
static lv_obj_t*   c_hdg_lbl[NUM_SCREENS]              = {};
static lv_obj_t*   c_card_lbl[NUM_SCREENS]             = {};
static lv_obj_t*   c_src_lbl[NUM_SCREENS]              = {};
// Bottom-left / bottom-right extra data fields (path from quad_bl/br_path)
static lv_obj_t*   c_bl_desc[NUM_SCREENS]              = {};
static lv_obj_t*   c_bl_val[NUM_SCREENS]               = {};
static lv_obj_t*   c_bl_unit[NUM_SCREENS]              = {};
static lv_obj_t*   c_br_desc[NUM_SCREENS]              = {};
static lv_obj_t*   c_br_val[NUM_SCREENS]               = {};
static lv_obj_t*   c_br_unit[NUM_SCREENS]              = {};

// ── Helpers ───────────────────────────────────────────────────────────────────
static lv_obj_t* screen_obj(int n) {
    switch (n) {
        case 0: return ui_Screen1;  case 1: return ui_Screen2;
        case 2: return ui_Screen3;  case 3: return ui_Screen4;
        case 4: return ui_Screen5;  default: return NULL;
    }
}

static lv_color_t compass_parse_color(const char* hex) {
    if (!hex || hex[0] != '#') return lv_color_white();
    unsigned int r, g, b;
    if (sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return lv_color_white();
}

static float norm360(float a) {
    while (a <   0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

// Convert offset-from-12-o'clock (degrees) + radius → screen pixel coords
static void arc_xy(float offset_deg, float r, lv_coord_t* px, lv_coord_t* py) {
    float rad = offset_deg * (float)M_PI / 180.0f;
    *px = (lv_coord_t)(PIVOT_X + r * sinf(rad) + 0.5f);
    *py = (lv_coord_t)(PIVOT_Y - r * cosf(rad) - Y_SHIFT + 0.5f);
}

static const char* cardinal_for(float deg) {
    static const char* cards[] = {"N","NE","E","SE","S","SW","W","NW"};
    int i = (int)((norm360(deg) + 22.5f) / 45.0f) % 8;
    return cards[i];
}

// ── Public API ────────────────────────────────────────────────────────────────
void compass_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    compass_display_destroy(n);

    lv_obj_t* scr = screen_obj(n);
    if (!scr) return;

    // Hide gauge needles and icons
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
    lv_obj_t* top_n[] = {ui_Needle,ui_Needle2,ui_Needle3,ui_Needle4,ui_Needle5};
    lv_obj_t* bot_n[] = {ui_Lower_Needle,ui_Lower_Needle2,ui_Lower_Needle3,
                         ui_Lower_Needle4,ui_Lower_Needle5};
    lv_obj_t* top_i[] = {ui_TopIcon1,ui_TopIcon2,ui_TopIcon3,ui_TopIcon4,ui_TopIcon5};
    lv_obj_t* bot_i[] = {ui_BottomIcon1,ui_BottomIcon2,ui_BottomIcon3,
                         ui_BottomIcon4,ui_BottomIcon5};
    if (top_n[n]) lv_obj_add_flag(top_n[n], LV_OBJ_FLAG_HIDDEN);
    if (bot_n[n]) lv_obj_add_flag(bot_n[n], LV_OBJ_FLAG_HIDDEN);
    if (top_i[n]) lv_obj_add_flag(top_i[n], LV_OBJ_FLAG_HIDDEN);
    if (bot_i[n]) lv_obj_add_flag(bot_i[n], LV_OBJ_FLAG_HIDDEN);

    // Full-screen background panel (colour from number_bg_color, default black)
    c_bg[n] = lv_obj_create(scr);
    lv_obj_set_size(c_bg[n], LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(c_bg[n], compass_parse_color(screen_configs[n].number_bg_color[0] ? screen_configs[n].number_bg_color : "#000000"), 0);
    lv_obj_set_style_bg_opa(c_bg[n], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c_bg[n], 0, 0);
    lv_obj_set_style_pad_all(c_bg[n], 0, 0);
    lv_obj_clear_flag(c_bg[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(c_bg[n], 0, 0);

    // ── Tape-arc band ────────────────────────────────────────────────────────
    // Large arc centred on virtual pivot (240, 570) — 90 px below screen bottom.
    // Object pos shifted up by Y_SHIFT: (240-550, 570-550-15) = (-310, 5).
    // Visible segment ≈ 215°–325° in LVGL angles (0°=right, clockwise).
    c_tape_arc[n] = lv_arc_create(c_bg[n]);
    lv_obj_set_size(c_tape_arc[n], 1100, 1100);
    lv_obj_set_pos(c_tape_arc[n], -310, 15);
    lv_arc_set_bg_angles(c_tape_arc[n], 215, 325);
    lv_arc_set_angles(c_tape_arc[n], 0, 0);          // indicator: invisible
    lv_obj_set_style_arc_color(c_tape_arc[n], lv_color_make(25, 35, 55), LV_PART_MAIN);
    lv_obj_set_style_arc_width(c_tape_arc[n], 100, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(c_tape_arc[n], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(c_tape_arc[n], lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(c_tape_arc[n], 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(c_tape_arc[n], LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(c_tape_arc[n], 0, LV_PART_KNOB);
    lv_obj_clear_flag(c_tape_arc[n], LV_OBJ_FLAG_CLICKABLE);

    // ── Tick lines (positioned by compass_display_update) ───────────────────
    for (int i = 0; i < N_TICKS; i++) {
        c_tick_pts[n][i][0] = {0, 0};
        c_tick_pts[n][i][1] = {0, 0};
        c_tick[n][i] = lv_line_create(c_bg[n]);
        lv_line_set_points(c_tick[n][i], c_tick_pts[n][i], 2);
        lv_obj_set_style_line_width(c_tick[n][i], 2, 0);
        lv_obj_set_style_line_color(c_tick[n][i], lv_color_make(150, 170, 200), 0);
    }

    // ── Labels on tape (positioned by compass_display_update) ───────────────
    for (int i = 0; i < N_LABELS; i++) {
        c_deg_lbl[n][i] = lv_label_create(c_bg[n]);
        lv_label_set_text(c_deg_lbl[n][i], "");
        lv_obj_set_style_text_font(c_deg_lbl[n][i], &inter_24, 0);
        lv_obj_set_style_text_color(c_deg_lbl[n][i], lv_color_make(150, 170, 200), 0);
    }

    // ── Fixed pointer triangle ───────────────────────────────────────────────
    // Tip at (240, 13) centre of arc band; base 26 px inward, ±13 px wide
    lv_coord_t tip_x = 240, tip_y = 13, base_y = tip_y + 26;
    c_ptr_pts_l[n][0] = {tip_x, tip_y};
    c_ptr_pts_l[n][1] = {(lv_coord_t)(tip_x - 13), base_y};
    c_ptr_pts_r[n][0] = {tip_x, tip_y};
    c_ptr_pts_r[n][1] = {(lv_coord_t)(tip_x + 13), base_y};
    c_ptr_l[n] = lv_line_create(c_bg[n]);
    lv_line_set_points(c_ptr_l[n], c_ptr_pts_l[n], 2);
    lv_obj_set_style_line_width(c_ptr_l[n], 3, 0);
    lv_obj_set_style_line_color(c_ptr_l[n], lv_color_white(), 0);
    c_ptr_r[n] = lv_line_create(c_bg[n]);
    lv_line_set_points(c_ptr_r[n], c_ptr_pts_r[n], 2);
    lv_obj_set_style_line_width(c_ptr_r[n], 3, 0);
    lv_obj_set_style_line_color(c_ptr_r[n], lv_color_white(), 0);

    // ── Large heading number (below tape) ────────────────────────────────────
    c_hdg_lbl[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_hdg_lbl[n], "---\xc2\xb0");
    lv_obj_set_style_text_font(c_hdg_lbl[n], &inter_96, 0);
    lv_obj_set_style_text_color(c_hdg_lbl[n], lv_color_white(), 0);
    lv_obj_align(c_hdg_lbl[n], LV_ALIGN_TOP_MID, 0, 125);

    // ── Cardinal direction ────────────────────────────────────────────────────
    c_card_lbl[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_card_lbl[n], "---");
    lv_obj_set_style_text_font(c_card_lbl[n], &inter_24, 0);
    lv_obj_set_style_text_color(c_card_lbl[n], lv_color_make(180, 220, 255), 0);
    lv_obj_align(c_card_lbl[n], LV_ALIGN_TOP_RIGHT, -12, 13);

    // ── Source label (top-left) ───────────────────────────────────────────────
    ScreenConfig& cfg = screen_configs[n];
    bool is_true = (String(cfg.number_path).indexOf("True") >= 0 ||
                    String(cfg.number_path).indexOf("true") >= 0);
    c_src_lbl[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_src_lbl[n], is_true ? "HDG \xc2\xb0T" : "HDG \xc2\xb0M");
    lv_obj_set_style_text_font(c_src_lbl[n], &inter_24, 0);
    lv_obj_set_style_text_color(c_src_lbl[n], lv_color_make(120, 120, 120), 0);
    lv_obj_align(c_src_lbl[n], LV_ALIGN_TOP_LEFT, 12, 8);

    // ── Bottom-left extra data field ─────────────────────────────────────────
    // Uses quad_bl_path / quad_bl_font_size / quad_bl_font_color from ScreenConfig.
    // Positioned identically to the BL quadrant of the quad display.
    // BL/BR font size helper (mirrors quad display font sizes)
    auto compass_font = [](uint8_t sz) -> const lv_font_t* {
        switch (sz) {
            case 0: return &inter_48;
            case 1: return &inter_72;
            case 2: return &inter_96;
            default: return &inter_48;
        }
    };
    const lv_font_t* small_fnt = &inter_16;
    lv_color_t bl_col = compass_parse_color(cfg.quad_bl_font_color[0] ? cfg.quad_bl_font_color : "#FFFFFF");
    lv_color_t br_col = compass_parse_color(cfg.quad_br_font_color[0] ? cfg.quad_br_font_color : "#FFFFFF");

    c_bl_desc[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_bl_desc[n], "");
    lv_obj_set_size(c_bl_desc[n], 230, 24);
    lv_label_set_long_mode(c_bl_desc[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(c_bl_desc[n], small_fnt, 0);
    lv_obj_set_style_text_color(c_bl_desc[n], bl_col, 0);
    lv_obj_align(c_bl_desc[n], LV_ALIGN_TOP_LEFT, 5, 245);
    lv_obj_add_flag(c_bl_desc[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    c_bl_val[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_bl_val[n], "---");
    lv_obj_set_size(c_bl_val[n], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(c_bl_val[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(c_bl_val[n], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(c_bl_val[n], compass_font(cfg.quad_bl_font_size), 0);
    lv_obj_set_style_text_color(c_bl_val[n], bl_col, 0);
    lv_obj_align(c_bl_val[n], LV_ALIGN_TOP_LEFT, 10, 340);
    lv_obj_add_flag(c_bl_val[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    c_bl_unit[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_bl_unit[n], "");
    lv_obj_set_size(c_bl_unit[n], 100, 24);
    lv_label_set_long_mode(c_bl_unit[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(c_bl_unit[n], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(c_bl_unit[n], small_fnt, 0);
    lv_obj_set_style_text_color(c_bl_unit[n], bl_col, 0);
    lv_obj_align(c_bl_unit[n], LV_ALIGN_TOP_LEFT, 135, 450);
    lv_obj_add_flag(c_bl_unit[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    // ── Bottom-right extra data field ────────────────────────────────────────
    c_br_desc[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_br_desc[n], "");
    lv_obj_set_size(c_br_desc[n], 230, 24);
    lv_label_set_long_mode(c_br_desc[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(c_br_desc[n], small_fnt, 0);
    lv_obj_set_style_text_color(c_br_desc[n], br_col, 0);
    lv_obj_align(c_br_desc[n], LV_ALIGN_TOP_LEFT, 245, 245);
    lv_obj_add_flag(c_br_desc[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    c_br_val[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_br_val[n], "---");
    lv_obj_set_size(c_br_val[n], 220, LV_SIZE_CONTENT);
    lv_label_set_long_mode(c_br_val[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(c_br_val[n], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(c_br_val[n], compass_font(cfg.quad_br_font_size), 0);
    lv_obj_set_style_text_color(c_br_val[n], br_col, 0);
    lv_obj_align(c_br_val[n], LV_ALIGN_TOP_LEFT, 250, 340);
    lv_obj_add_flag(c_br_val[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    c_br_unit[n] = lv_label_create(c_bg[n]);
    lv_label_set_text(c_br_unit[n], "");
    lv_obj_set_size(c_br_unit[n], 100, 24);
    lv_label_set_long_mode(c_br_unit[n], LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(c_br_unit[n], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(c_br_unit[n], small_fnt, 0);
    lv_obj_set_style_text_color(c_br_unit[n], br_col, 0);
    lv_obj_align(c_br_unit[n], LV_ALIGN_TOP_LEFT, 375, 450);
    lv_obj_add_flag(c_br_unit[n], LV_OBJ_FLAG_IGNORE_LAYOUT);

    // Draw initial position (heading = 0)
    compass_display_update(n, 0.0f, 0);
}

void compass_display_update(int n, float heading_deg, int heading_true) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (!c_hdg_lbl[n]) return;

    heading_deg = norm360(heading_deg);

    // Heading number
    char buf[16];
    snprintf(buf, sizeof(buf), "%03.0f\xc2\xb0", heading_deg);
    lv_label_set_text(c_hdg_lbl[n], buf);

    // Cardinal label
    lv_label_set_text(c_card_lbl[n], cardinal_for(heading_deg));

    // ── Tick marks ───────────────────────────────────────────────────────────
    float tick_start = floorf((heading_deg - TICK_HALF) / TICK_STEP) * TICK_STEP;
    for (int i = 0; i < N_TICKS; i++) {
        float bearing = norm360(tick_start + i * TICK_STEP);
        float offset  = bearing - heading_deg;
        if (offset >  180.0f) offset -= 360.0f;
        if (offset < -180.0f) offset += 360.0f;

        bool major = (fmodf(fabsf(bearing) + 0.5f, 10.0f) < 1.0f);
        float r_in = major ? ARC_R_MAJ_IN : ARC_R_MIN_IN;

        lv_coord_t ox, oy, ix, iy;
        arc_xy(offset, ARC_R_OUT, &ox, &oy);
        arc_xy(offset, r_in,      &ix, &iy);
        c_tick_pts[n][i][0] = {ox, oy};
        c_tick_pts[n][i][1] = {ix, iy};
        lv_line_set_points(c_tick[n][i], c_tick_pts[n][i], 2);
        lv_obj_invalidate(c_tick[n][i]);
    }

    // ── Degree / cardinal labels ──────────────────────────────────────────────
    float lbl_start = floorf((heading_deg - LBL_HALF) / LBL_STEP) * LBL_STEP;
    for (int i = 0; i < N_LABELS; i++) {
        float bearing = norm360(lbl_start + i * LBL_STEP);
        float offset  = bearing - heading_deg;
        if (offset >  180.0f) offset -= 360.0f;
        if (offset < -180.0f) offset += 360.0f;

        if (fabsf(offset) > 37.0f) {
            lv_label_set_text(c_deg_lbl[n][i], "");
            continue;
        }

        bool is_card = (fmodf(bearing + 0.5f, 45.0f) < 1.0f);
        char lbuf[8];
        const char* txt;
        lv_color_t col;
        if (is_card) {
            txt = cardinal_for(bearing);
            col = (bearing < 1.0f || bearing > 359.0f)
                  ? lv_color_make(220, 60, 60)
                  : lv_color_white();
            lv_obj_set_style_text_font(c_deg_lbl[n][i], &inter_48, 0);
        } else {
            snprintf(lbuf, sizeof(lbuf), "%.0f", bearing);
            txt = lbuf;
            col = lv_color_make(140, 160, 190);
            lv_obj_set_style_text_font(c_deg_lbl[n][i], &inter_24, 0);
        }
        lv_label_set_text(c_deg_lbl[n][i], txt);
        lv_obj_set_style_text_color(c_deg_lbl[n][i], col, 0);

        lv_obj_update_layout(c_deg_lbl[n][i]);
        lv_coord_t lx, ly;
        arc_xy(offset, ARC_R_LABEL, &lx, &ly);
        int w = lv_obj_get_width(c_deg_lbl[n][i]);
        int h = lv_obj_get_height(c_deg_lbl[n][i]);
        lv_obj_set_pos(c_deg_lbl[n][i], lx - w / 2, ly - h / 2);
    }

    (void)heading_true;
}

void compass_display_update_bl(int n, float value, const char* unit, const char* description) {
    if (n < 0 || n >= NUM_SCREENS || !c_bl_val[n]) return;
    char buf[64];
    if (isnan(value)) snprintf(buf, sizeof(buf), "---");
    else              format_display_value(buf, sizeof(buf), value, description);
    lv_label_set_text(c_bl_val[n],  buf);
    lv_label_set_text(c_bl_unit[n], unit        ? unit        : "");
    lv_label_set_text(c_bl_desc[n], description ? description : "");
}

void compass_display_update_br(int n, float value, const char* unit, const char* description) {
    if (n < 0 || n >= NUM_SCREENS || !c_br_val[n]) return;
    char buf[64];
    if (isnan(value)) snprintf(buf, sizeof(buf), "---");
    else              format_display_value(buf, sizeof(buf), value, description);
    lv_label_set_text(c_br_val[n],  buf);
    lv_label_set_text(c_br_unit[n], unit        ? unit        : "");
    lv_label_set_text(c_br_desc[n], description ? description : "");
}

void compass_display_destroy(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (c_bg[n]) { lv_obj_del(c_bg[n]); c_bg[n] = NULL; }
    c_tape_arc[n] = NULL;
    c_ptr_l[n]    = NULL;
    c_ptr_r[n]    = NULL;
    c_hdg_lbl[n]  = NULL;
    c_card_lbl[n] = NULL;
    c_src_lbl[n]  = NULL;
    c_bl_desc[n]  = NULL; c_bl_val[n] = NULL; c_bl_unit[n] = NULL;
    c_br_desc[n]  = NULL; c_br_val[n] = NULL; c_br_unit[n] = NULL;
    for (int i = 0; i < N_TICKS;  i++) c_tick[n][i]    = NULL;
    for (int i = 0; i < N_LABELS; i++) c_deg_lbl[n][i] = NULL;
}

extern "C" void mark_compass_display_created(int /*screen_num*/) {}
extern "C" void mark_compass_display_destroyed(int /*screen_num*/) {}
