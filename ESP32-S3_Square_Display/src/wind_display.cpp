// wind_display.cpp — built-in wind screen.
//
//   Top:      AWS (large, centred, "AWS kt" above), AWA top-left (light blue), TWA top-right (amber),
//             with the side letter (S green / P red) a size smaller
//   Dial:     0-180° each side of the bow, red/green close-hauled zones (20-60°),
//             apparent (A) and true (T) pointers with a sharp tip at the scale
//   Centre:   boat outline with drift speed inside; a short teal arrow circles
//             the boat, pointing the way the current pushes it (relative to bow)
//   Bottom:   two fields, TWS and TWD by default, or any data the user picks
//
// True wind and drift are worked out here when the network doesn't send them;
// values worked out on the display get an amber "calc" tag. Anything missing
// or out of date shows "--". Shared by the Signal K and NMEA 2000 firmware.

#include "wind_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include "unit_convert.h"
#include <Arduino.h>
#include <math.h>
#include <esp_attr.h>

// ─── Geometry ───────────────────────────────────────────────────────────────
#define CX            240
#define CY            275     // dial centre: top of the dial sits just under AWS
#define R_BAND_OUT    190
#define BAND_W         16
#define R_TICK_OUT    187
#define R_TICK_MAJ    172
#define R_TICK_MIN    178
#define R_LABEL       148
#define R_PTR_TIP     186     // pointer point reaches into the scale
#define R_PTR_BASE    162
#define R_PTR_TAIL    120
#define R_PTR_W         5     // pointer line width
#define R_PTR_HEAD      9     // half-width of the pointer head
#define R_BADGE       108     // A / T badge at the back of the pointer
#define BADGE_R        12
#define R_DRIFT_IN     66     // drift arrow ring, just outside the boat
#define R_DRIFT_OUT    96
#define N_TICKS        36
#define N_LABELS       10
#define N_HULL         28

#define DRIFT_MIN_MS   0.05f  // ~0.1 kn: below this there's no meaningful direction

// ─── Colours (shared with the other built-in screens) ───────────────────────
#define COL_NAVY       lv_color_make(25, 35, 55)
#define COL_SCALE      lv_color_make(150, 170, 200)
#define COL_LABEL      lv_color_make(150, 150, 150)
#define COL_LABEL_DIM  lv_color_make(120, 120, 120)
#define COL_APPARENT   lv_color_make(180, 220, 255)
#define COL_TRUE       lv_color_make(255, 200, 50)
#define COL_DRIFT      lv_color_make(100, 220, 180)
#define COL_PORT       lv_color_make(220, 60, 60)
#define COL_STBD       lv_color_make(60, 190, 90)

// ─── Per-screen state ───────────────────────────────────────────────────────
typedef struct {
    lv_obj_t* bg;
    lv_obj_t* dyn;                       // pointers + drift arrow, drawn in a draw event
    lv_obj_t* tick[N_TICKS];
    lv_obj_t* hull;
    lv_obj_t* aws_val;  lv_obj_t* aws_unit;
    lv_obj_t* awa_val;  lv_obj_t* awa_side;
    lv_obj_t* twa_val;  lv_obj_t* twa_side; lv_obj_t* twa_calc;
    lv_obj_t* drift_val;
    lv_obj_t* f_desc[2]; lv_obj_t* f_calc[2]; lv_obj_t* f_val[2]; lv_obj_t* f_unit[2];
    // Last drawn dynamic state (NAN = hidden)
    float awa, twa, drift_rel;
    // Latest computed values, used by the default bottom fields
    float tws_ms; int true_calc;
    float twd_deg; int twd_mag, twd_calc;
} WindScreen;

static WindScreen w[NUM_SCREENS];
static EXT_RAM_ATTR lv_point_t w_tick_pts[NUM_SCREENS][N_TICKS][2];
static EXT_RAM_ATTR lv_point_t w_hull_pts[NUM_SCREENS][N_HULL];

// ─── Helpers ────────────────────────────────────────────────────────────────
static lv_obj_t* screen_obj(int n) {
    switch (n) {
        case 0: return ui_Screen1;  case 1: return ui_Screen2;
        case 2: return ui_Screen3;  case 3: return ui_Screen4;
        case 4: return ui_Screen5;  default: return NULL;
    }
}

static lv_color_t parse_color(const char* hex, lv_color_t fallback) {
    if (!hex || hex[0] != '#') return fallback;
    unsigned int r, g, b;
    if (sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return fallback;
}

static float norm360(float a) {
    a = fmodf(a, 360.0f);
    if (a < 0) a += 360.0f;
    return a;
}

static float norm180(float a) {          // -180 < a <= 180, starboard positive
    a = norm360(a);
    return a > 180.0f ? a - 360.0f : a;
}

static bool ok(float v) { return !isnan(v); }

// Angle from the bow (0 = up, clockwise) + radius → screen point
static lv_point_t polar(float deg, float r) {
    float rad = deg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf(CX + r * sinf(rad));
    p.y = (lv_coord_t)lroundf(CY - r * cosf(rad));
    return p;
}

// Point offset sideways (+ = clockwise side) from the radial line at deg
static lv_point_t polar_side(float deg, float r, float side) {
    float rad = deg * (float)M_PI / 180.0f;
    float x = CX + r * sinf(rad) + side * cosf(rad);
    float y = CY - r * cosf(rad) + side * sinf(rad);
    lv_point_t p = { (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y) };
    return p;
}

static lv_obj_t* make_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t col, const char* txt) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t* make_arc(lv_obj_t* parent, uint16_t start, uint16_t end, lv_color_t col) {
    lv_obj_t* a = lv_arc_create(parent);
    lv_obj_set_size(a, R_BAND_OUT * 2, R_BAND_OUT * 2);
    lv_obj_set_pos(a, CX - R_BAND_OUT, CY - R_BAND_OUT);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_angles(a, start, start);              // no indicator
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(a, BAND_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, col, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return a;
}

// Angle from the bow as "18°" plus a side letter ("S", "P", or "" dead ahead/astern)
static int angle_side(float deg, const char** side) {
    float a = norm180(deg);
    int v = (int)lroundf(fabsf(a));
    *side = (v == 0 || v == 180) ? "" : (a > 0 ? "S" : "P");
    return v;
}

static void set_if_changed(lv_obj_t* l, const char* txt);

// Big angle number with a smaller side letter, coloured green (S) / red (P).
// right: the pair is right-aligned to the screen edge, else left-aligned.
// Baselines: inter_48 sits 11 px above its box bottom, inter_24 6 px → -5.
#define TOP_VAL_Y  26
#define TOP_LBL_Y   3     // top label row (AWA, AWS kt, TWA)
static void set_angle_pair(lv_obj_t* val, lv_obj_t* side_lbl, float deg, bool right) {
    char buf[12];
    const char* side = "";
    if (ok(deg)) snprintf(buf, sizeof(buf), "%d\xc2\xb0", angle_side(deg, &side));
    else strcpy(buf, "--");
    set_if_changed(val, buf);
    set_if_changed(side_lbl, side);
    lv_obj_set_style_text_color(side_lbl, side[0] == 'S' ? COL_STBD : COL_PORT, 0);
    lv_obj_update_layout(val);
    lv_obj_update_layout(side_lbl);
    int gap = side[0] ? 4 : 0;
    if (right) {
        lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -12 - lv_obj_get_width(side_lbl) - gap, TOP_VAL_Y);
    } else {
        lv_obj_set_pos(val, 12, TOP_VAL_Y);
    }
    lv_obj_align_to(side_lbl, val, LV_ALIGN_OUT_RIGHT_BOTTOM, gap, -5);
}

static void layout_field(WindScreen& s, int side);

// ─── Dynamic drawing: pointers and drift arrow ──────────────────────────────
// badge_side: sideways shift of the letter badge, so A and T stay readable
// when the two pointers line up (e.g. boat stopped: true = apparent).
static void draw_pointer(lv_draw_ctx_t* ctx, float deg, lv_color_t col, const char* letter,
                         float badge_side = 0) {
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col; ld.width = R_PTR_W; ld.round_start = 1; ld.round_end = 1;
    lv_point_t a = polar(deg, R_PTR_TAIL), b = polar(deg, R_PTR_BASE);
    lv_draw_line(ctx, &ld, &a, &b);

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col; rd.bg_opa = LV_OPA_COVER;
    lv_point_t tri[3] = { polar(deg, R_PTR_TIP), polar_side(deg, R_PTR_BASE - 2, R_PTR_HEAD),
                          polar_side(deg, R_PTR_BASE - 2, -R_PTR_HEAD) };
    lv_draw_triangle(ctx, &rd, tri);

    // Badge at the back of the pointer
    lv_point_t c = polar_side(deg, R_BADGE, badge_side);
    lv_area_t ba = { (lv_coord_t)(c.x - BADGE_R), (lv_coord_t)(c.y - BADGE_R),
                     (lv_coord_t)(c.x + BADGE_R), (lv_coord_t)(c.y + BADGE_R) };
    rd.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(ctx, &rd, &ba);
    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.color = lv_color_black(); td.font = &inter_16; td.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t ta = { ba.x1, (lv_coord_t)(c.y - 9), ba.x2, (lv_coord_t)(c.y + 9) };
    lv_draw_label(ctx, &td, &ta, letter, NULL);
}

static void draw_drift_arrow(lv_draw_ctx_t* ctx, float rel) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = COL_DRIFT; rd.bg_opa = LV_OPA_COVER;
    const float r_head = R_DRIFT_OUT - 18;
    // Shaft (convex quad) + head (triangle): the polygon drawer needs convex shapes
    lv_point_t shaft[4] = { polar_side(rel, R_DRIFT_IN, -11), polar_side(rel, r_head, -11),
                            polar_side(rel, r_head, 11),      polar_side(rel, R_DRIFT_IN, 11) };
    lv_draw_polygon(ctx, &rd, shaft, 4);
    lv_point_t head[3] = { polar(rel, R_DRIFT_OUT), polar_side(rel, r_head, 21),
                           polar_side(rel, r_head, -21) };
    lv_draw_triangle(ctx, &rd, head);
}

static void dyn_draw_cb(lv_event_t* e) {
    int n = (int)(intptr_t)lv_event_get_user_data(e);
    if (n < 0 || n >= NUM_SCREENS) return;
    lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
    if (ok(w[n].drift_rel)) draw_drift_arrow(ctx, w[n].drift_rel);
    // Pointers within a few degrees: put the badges side by side
    float t_side = 0, a_side = 0;
    if (ok(w[n].twa) && ok(w[n].awa)) {
        float d = norm180(w[n].twa - w[n].awa);
        if (fabsf(d) < 8.0f) {
            t_side = (d >= 0) ? (BADGE_R + 2) : -(BADGE_R + 2);
            a_side = -t_side;
        }
    }
    if (ok(w[n].twa)) draw_pointer(ctx, w[n].twa, COL_TRUE, "T", t_side);
    if (ok(w[n].awa)) draw_pointer(ctx, w[n].awa, COL_APPARENT, "A", a_side);   // apparent on top
}

// ─── Create / destroy ───────────────────────────────────────────────────────
void wind_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    wind_display_destroy(n);
    lv_obj_t* scr = screen_obj(n);
    if (!scr) return;

    // Hide the gauge needles and icons on this screen
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
    lv_obj_t* bot_n[] = {ui_Lower_Needle,ui_Lower_Needle2,ui_Lower_Needle3,ui_Lower_Needle4,ui_Lower_Needle5};
    lv_obj_t* top_i[] = {ui_TopIcon1,ui_TopIcon2,ui_TopIcon3,ui_TopIcon4,ui_TopIcon5};
    lv_obj_t* bot_i[] = {ui_BottomIcon1,ui_BottomIcon2,ui_BottomIcon3,ui_BottomIcon4,ui_BottomIcon5};
    lv_obj_t* hide[] = { top_n[n], bot_n[n], top_i[n], bot_i[n] };
    for (lv_obj_t* o : hide) if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);

    WindScreen& s = w[n];
    const ScreenConfig& cfg = screen_configs[n];

    // Background (screen colour setting, default black)
    s.bg = lv_obj_create(scr);
    lv_obj_set_size(s.bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s.bg, 0, 0);
    lv_obj_set_style_bg_color(s.bg, parse_color(cfg.number_bg_color, lv_color_black()), 0);
    lv_obj_set_style_bg_opa(s.bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s.bg, 0, 0);
    lv_obj_set_style_radius(s.bg, 0, 0);
    lv_obj_set_style_pad_all(s.bg, 0, 0);
    lv_obj_clear_flag(s.bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Dial band and close-hauled zones (LVGL arc angles: 0 = 3 o'clock, clockwise)
    make_arc(s.bg, 0, 360, COL_NAVY);
    make_arc(s.bg, 210, 250, COL_PORT);    // port 60-20°
    make_arc(s.bg, 290, 330, COL_STBD);    // starboard 20-60°

    // Ticks every 10°, longer every 30°
    for (int i = 0; i < N_TICKS; i++) {
        float deg = i * 10.0f;
        bool major = (i % 3) == 0;
        w_tick_pts[n][i][0] = polar(deg, major ? R_TICK_MAJ : R_TICK_MIN);
        w_tick_pts[n][i][1] = polar(deg, R_TICK_OUT);
        s.tick[i] = lv_line_create(s.bg);
        lv_line_set_points(s.tick[i], w_tick_pts[n][i], 2);
        lv_obj_set_style_line_color(s.tick[i], COL_SCALE, 0);
        lv_obj_set_style_line_width(s.tick[i], major ? 3 : 2, 0);
    }

    // Scale numbers: 30-150 each side
    static const int vals[5] = {30, 60, 90, 120, 150};
    for (int i = 0; i < N_LABELS; i++) {
        int v = vals[i % 5];
        float deg = (i < 5) ? v : -v;
        char b[8]; snprintf(b, sizeof(b), "%d", v);
        lv_obj_t* l = make_label(s.bg, &inter_24, COL_SCALE, b);
        lv_obj_update_layout(l);
        lv_point_t p = polar(deg, R_LABEL);
        lv_obj_set_pos(l, p.x - lv_obj_get_width(l) / 2, p.y - lv_obj_get_height(l) / 2);
    }

    // Boat outline: pointed bow, widest about two thirds back, slightly narrower
    // stern. One closed line: bow → port side → stern → starboard side → bow.
    {
        const float bow_y = CY - 62, stern_y = CY + 46, beam = 32, stern = 27;
        const int half = N_HULL / 2;          // points per side
        for (int i = 0; i < half; i++) {
            float t = (float)i / (half - 1);
            float y = bow_y + t * (stern_y - bow_y);
            float hw = (t < 0.65f) ? beam * sinf(t / 0.65f * (float)M_PI / 2)
                                   : beam - (beam - stern) * (t - 0.65f) / 0.35f;
            w_hull_pts[n][i] = { (lv_coord_t)lroundf(CX - hw), (lv_coord_t)lroundf(y) };
            w_hull_pts[n][N_HULL - 1 - i] = { (lv_coord_t)lroundf(CX + hw), (lv_coord_t)lroundf(y) };
        }
        s.hull = lv_line_create(s.bg);
        lv_line_set_points(s.hull, w_hull_pts[n], N_HULL);
        lv_obj_set_style_line_color(s.hull, COL_SCALE, 0);
        lv_obj_set_style_line_width(s.hull, 2, 0);
    }

    // Drift speed inside the boat
    s.drift_val = make_label(s.bg, &inter_24, COL_DRIFT, "--");
    lv_obj_set_width(s.drift_val, 64);
    lv_obj_set_style_text_align(s.drift_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.drift_val, CX - 32, CY - 14);
    lv_obj_t* dl = make_label(s.bg, &inter_16, COL_LABEL, "drift");
    lv_obj_set_width(dl, 64);
    lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(dl, CX - 32, CY + 14);

    // Pointers and drift arrow: one full-screen layer drawn in a draw event
    s.dyn = lv_obj_create(s.bg);
    lv_obj_remove_style_all(s.dyn);
    lv_obj_set_size(s.dyn, 480, 480);
    lv_obj_set_pos(s.dyn, 0, 0);
    lv_obj_clear_flag(s.dyn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.dyn, dyn_draw_cb, LV_EVENT_DRAW_MAIN, (void*)(intptr_t)n);

    // AWS across the top
    s.aws_val = make_label(s.bg, &inter_72, lv_color_white(), "--");
    lv_obj_set_width(s.aws_val, 220);
    lv_obj_set_style_text_align(s.aws_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.aws_val, CX - 110, 10);    // centred under its label
    // "AWS kt" centred on the top label row, in line with AWA and TWA
    s.aws_unit = make_label(s.bg, &inter_16, COL_LABEL, "");
    lv_obj_set_width(s.aws_unit, 160);
    lv_obj_set_style_text_align(s.aws_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s.aws_unit, CX - 80, TOP_LBL_Y);
    {
        char t[24]; snprintf(t, sizeof(t), "AWS  %s", speed_unit_label());
        lv_label_set_text(s.aws_unit, t);
    }

    // AWA top-left, TWA top-right, each in its pointer's colour, same size as
    // the bottom fields
    lv_obj_t* awal = make_label(s.bg, &inter_16, COL_LABEL, "AWA");
    lv_obj_set_pos(awal, 12, TOP_LBL_Y);
    s.awa_val  = make_label(s.bg, &inter_48, COL_APPARENT, "--");
    s.awa_side = make_label(s.bg, &inter_24, COL_STBD, "");
    set_angle_pair(s.awa_val, s.awa_side, NAN, false);

    lv_obj_t* twal = make_label(s.bg, &inter_16, COL_LABEL, "TWA");
    lv_obj_align(twal, LV_ALIGN_TOP_RIGHT, -12, TOP_LBL_Y);
    s.twa_calc = make_label(s.bg, &inter_16, COL_TRUE, "calc");
    lv_obj_align_to(s.twa_calc, twal, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_add_flag(s.twa_calc, LV_OBJ_FLAG_HIDDEN);
    s.twa_val  = make_label(s.bg, &inter_48, COL_TRUE, "--");
    s.twa_side = make_label(s.bg, &inter_24, COL_STBD, "");
    set_angle_pair(s.twa_val, s.twa_side, NAN, true);

    // Bottom fields
    for (int side = 0; side < 2; side++) {
        const char* colhex = side == 0 ? cfg.quad_bl_font_color : cfg.quad_br_font_color;
        lv_color_t col = parse_color(colhex, lv_color_white());
        // Always the small font: anything bigger runs into the dial. (The Quad
        // screen's font size setting shares this storage, so it's ignored here.)
        const lv_font_t* vf = &inter_48;
        lv_align_t al = side == 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT;
        int dx = side == 0 ? 12 : -12;

        s.f_val[side] = make_label(s.bg, vf, col, "--");
        lv_obj_align(s.f_val[side], al, dx, 4);   // right side is re-placed in layout_field
        s.f_unit[side] = make_label(s.bg, &inter_16, COL_LABEL, "");
        s.f_desc[side] = make_label(s.bg, &inter_16, COL_LABEL, "");
        s.f_calc[side] = make_label(s.bg, &inter_16, COL_TRUE, "calc");
        lv_obj_add_flag(s.f_calc[side], LV_OBJ_FLAG_HIDDEN);
        layout_field(s, side);
    }

    s.awa = s.twa = s.drift_rel = NAN;
    s.tws_ms = s.twd_deg = NAN;
    s.true_calc = s.twd_mag = s.twd_calc = 0;
}

void wind_display_destroy(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (w[n].bg) lv_obj_del(w[n].bg);
    memset(&w[n], 0, sizeof(w[n]));
}

// ─── Updates ────────────────────────────────────────────────────────────────
// Re-position a bottom field's labels after its text changed
static void layout_field(WindScreen& s, int side) {
    lv_obj_update_layout(s.f_val[side]);
    lv_obj_update_layout(s.f_unit[side]);
    lv_obj_update_layout(s.f_desc[side]);
    if (side == 0) {
        lv_obj_align_to(s.f_unit[side], s.f_val[side], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -12);
        lv_obj_align_to(s.f_desc[side], s.f_val[side], LV_ALIGN_OUT_TOP_LEFT, 0, 6);
        lv_obj_align_to(s.f_calc[side], s.f_desc[side], LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    } else {
        // Unit sits after the number, against the right edge ("239° M", "5.2 m")
        bool has_unit = lv_label_get_text(s.f_unit[side])[0] != '\0';
        int unit_w = has_unit ? lv_obj_get_width(s.f_unit[side]) + 4 : 0;
        lv_obj_align(s.f_val[side], LV_ALIGN_BOTTOM_RIGHT, -12 - unit_w, 4);
        lv_obj_align_to(s.f_unit[side], s.f_val[side], LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -12);
        lv_obj_update_layout(s.f_val[side]);
        lv_obj_align(s.f_desc[side], LV_ALIGN_TOP_RIGHT, -12,
                     lv_obj_get_y(s.f_val[side]) - lv_obj_get_height(s.f_desc[side]) + 6);
        lv_obj_align_to(s.f_calc[side], s.f_desc[side], LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
}

static void set_if_changed(lv_obj_t* l, const char* txt) {
    if (strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
}

void wind_display_update(int n, const WindInputs* in) {
    if (n < 0 || n >= NUM_SCREENS || !w[n].bg || !in) return;
    WindScreen& s = w[n];
    char buf[24];

    // Heading: true if possible (magnetic + variation), else whatever we have
    float hdg_t = NAN;
    if (ok(in->hdg_deg)) {
        if (in->hdg_true) hdg_t = in->hdg_deg;
        else if (ok(in->variation_deg)) hdg_t = norm360(in->hdg_deg + in->variation_deg);
    }

    // ── Apparent wind ──
    float awa = ok(in->awa_deg) ? norm180(in->awa_deg) : NAN;
    float aws = in->aws_ms;

    // ── True wind angle / speed ──
    float twa = NAN, tws = NAN;
    int true_calc = 0;
    bool over_ground = false;
    if (ok(in->twa_deg) && ok(in->tws_ms)) {
        twa = norm180(in->twa_deg); tws = in->tws_ms;
    } else if (ok(awa) && ok(aws)) {
        // Apparent wind = true wind + wind from the boat's own motion.
        // Boat frame: x forward, y starboard; wind vectors point where it comes FROM.
        float v = ok(in->stw_ms) ? in->stw_ms : NAN;
        if (!ok(v) && ok(in->sog_ms)) { v = in->sog_ms; over_ground = true; }
        if (ok(v)) {
            float ar = awa * (float)M_PI / 180.0f;
            float tx = aws * cosf(ar) - v;
            float ty = aws * sinf(ar);
            tws = sqrtf(tx * tx + ty * ty);
            twa = (tws < 0.01f) ? 0.0f : atan2f(ty, tx) * 180.0f / (float)M_PI;
            true_calc = 1;
        }
    }

    // ── True wind direction ──
    float twd = NAN; bool twd_mag = false; int twd_calc = 0;
    if (ok(in->twd_deg)) {
        twd = norm360(in->twd_deg);
    } else if (ok(twa)) {
        if (over_ground && ok(in->cog_deg))  twd = norm360(in->cog_deg + twa);
        else if (ok(hdg_t))                  twd = norm360(hdg_t + twa);
        else if (ok(in->hdg_deg))          { twd = norm360(in->hdg_deg + twa); twd_mag = true; }
        else if (ok(in->cog_deg))            twd = norm360(in->cog_deg + twa);
        twd_calc = ok(twd) ? 1 : 0;
    }

    // ── Drift (current) ──
    float set_t = NAN, drift = NAN;
    if (ok(in->set_deg) && ok(in->drift_ms)) {
        set_t = norm360(in->set_deg); drift = in->drift_ms;
    } else if (ok(hdg_t) && ok(in->stw_ms) && ok(in->sog_ms) && ok(in->cog_deg)) {
        // Current = movement over the ground − movement through the water
        float hr = hdg_t * (float)M_PI / 180.0f, cr = in->cog_deg * (float)M_PI / 180.0f;
        float n_ = in->sog_ms * cosf(cr) - in->stw_ms * cosf(hr);
        float e_ = in->sog_ms * sinf(cr) - in->stw_ms * sinf(hr);
        drift = sqrtf(n_ * n_ + e_ * e_);
        set_t = norm360(atan2f(e_, n_) * 180.0f / (float)M_PI);
    }
    float hdg_any = ok(hdg_t) ? hdg_t : in->hdg_deg;
    float drift_rel = (ok(drift) && drift >= DRIFT_MIN_MS && ok(set_t) && ok(hdg_any))
                      ? norm180(set_t - hdg_any) : NAN;

    // ── Text ──
    if (ok(aws)) snprintf(buf, sizeof(buf), "%.1f", convert_speed(aws)); else strcpy(buf, "--");
    set_if_changed(s.aws_val, buf);
    {
        char t[24]; snprintf(t, sizeof(t), "AWS  %s", speed_unit_label());
        set_if_changed(s.aws_unit, t);
    }

    set_angle_pair(s.awa_val, s.awa_side, awa, false);
    set_angle_pair(s.twa_val, s.twa_side, twa, true);
    if (true_calc && ok(twa)) lv_obj_clear_flag(s.twa_calc, LV_OBJ_FLAG_HIDDEN);
    else                      lv_obj_add_flag(s.twa_calc, LV_OBJ_FLAG_HIDDEN);

    if (ok(drift)) snprintf(buf, sizeof(buf), "%.1f", convert_speed(drift)); else strcpy(buf, "--");
    set_if_changed(s.drift_val, buf);

    // Remember for the default bottom fields
    s.tws_ms = tws; s.true_calc = true_calc;
    s.twd_deg = twd; s.twd_mag = twd_mag; s.twd_calc = twd_calc;

    // ── Redraw pointers / arrow only when they move by a degree or more ──
    auto moved = [](float a, float b) {
        if (isnan(a) != isnan(b)) return true;
        if (isnan(a)) return false;
        return fabsf(norm180(a - b)) >= 1.0f;
    };
    if (moved(s.awa, awa) || moved(s.twa, twa) || moved(s.drift_rel, drift_rel)) {
        s.awa = awa; s.twa = twa; s.drift_rel = drift_rel;
        lv_area_t a = { CX - R_BAND_OUT, CY - R_BAND_OUT, CX + R_BAND_OUT, CY + R_BAND_OUT };
        lv_obj_invalidate_area(s.dyn, &a);
    }
}

void wind_display_update_field(int n, int side, int use_default,
                               float value, const char* unit, const char* description) {
    if (n < 0 || n >= NUM_SCREENS || side < 0 || side > 1 || !w[n].bg) return;
    WindScreen& s = w[n];
    char vbuf[24];
    const char* u = "";
    const char* d = "";
    bool calc = false;

    if (use_default) {
        if (side == 0) {
            d = "TWS";
            if (ok(s.tws_ms)) { snprintf(vbuf, sizeof(vbuf), "%.1f", convert_speed(s.tws_ms)); u = speed_unit_label(); }
            else strcpy(vbuf, "--");
            calc = s.true_calc && ok(s.tws_ms);
        } else {
            d = "TWD";
            if (ok(s.twd_deg)) {
                snprintf(vbuf, sizeof(vbuf), "%03d\xc2\xb0", (int)lroundf(s.twd_deg) % 360);
                u = s.twd_mag ? "M" : "T";
            } else strcpy(vbuf, "--");
            calc = s.twd_calc && ok(s.twd_deg);
        }
    } else {
        if (isnan(value)) strcpy(vbuf, "--");
        else snprintf(vbuf, sizeof(vbuf), "%.1f", value);
        u = unit ? unit : "";
        d = description ? description : "";
    }

    bool changed = strcmp(lv_label_get_text(s.f_val[side]), vbuf) != 0 ||
                   strcmp(lv_label_get_text(s.f_unit[side]), u) != 0 ||
                   strcmp(lv_label_get_text(s.f_desc[side]), d) != 0 ||
                   lv_obj_has_flag(s.f_calc[side], LV_OBJ_FLAG_HIDDEN) == calc;
    if (!changed) return;
    lv_label_set_text(s.f_val[side], vbuf);
    lv_label_set_text(s.f_unit[side], u);
    lv_label_set_text(s.f_desc[side], d);
    if (calc) lv_obj_clear_flag(s.f_calc[side], LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s.f_calc[side], LV_OBJ_FLAG_HIDDEN);
    layout_field(s, side);
}
