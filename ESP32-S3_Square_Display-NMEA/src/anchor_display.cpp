// anchor_display.cpp — Anchor alarm with top-down chart view
//
// Layout (480×480):
//   - Full black background
//   - Alarm circle inset at ALARM_RING_R px from centre (= 85% of half-screen)
//   - Anchor icon at centre (always)
//   - Boat arrow plotted relative to anchor using current GPS
//   - Track dots (PSRAM ring buffer, one per 5 s)
//   - Tap anywhere on map → set anchor lat/lon at tapped position
//   - [Drop Here] button → set anchor at current boat position
//   - [−] / [+] buttons → adjust alarm radius in 5 m steps
//   - [ALARM ON/OFF] toggle → arm/disarm buzzer
//   - Status bar: distance, bearing, max-drift, radius
//
// Scale: the alarm ring is always at ALARM_RING_R pixels from the screen
// centre.  1 pixel = alarm_radius_m / ALARM_RING_R  metres.

#include "anchor_display.h"
#include "screen_config_c_api.h"
#include "signalk_config.h"
#include "nmea2000_config.h"
#include "ui_Settings.h"
#include <math.h>
#include <Preferences.h>
#include <Arduino.h>

// ── UI screen object helpers ─────────────────────────────────────────────────
#include "ui.h"

// ── Constants ────────────────────────────────────────────────────────────────
#define SCR_W       480
#define SCR_H       480
#define CX          240
#define CY          240

// Alarm ring is drawn at 85% of half-screen width
#define ALARM_RING_R    204          // pixels (= 0.85 × 240)

// Boat icon is clipped to this radius (slightly outside alarm ring)
#define BOAT_CLIP_R     228          // pixels

// Bottom status/button band height
#define STATUS_H        90

// Map area height (above status band)
#define MAP_H           (SCR_H - STATUS_H)
#define MAP_CY          (MAP_H / 2)   // 206

// Track buffer (PSRAM ring, one point per 5 s)
#define TRACK_MAX       1200          // 100 minutes at 5 s/pt
#define TRACK_INTERVAL_MS   5000UL

// Radius limits and step
#define RADIUS_MIN_M    10.0f
#define RADIUS_MAX_M    95.0f
#define RADIUS_STEP_M   5.0f

// Alarm ring colour
#define ALARM_COL_ARMED     lv_color_make(220,  60,  60)   // red
#define ALARM_COL_DISARMED  lv_color_make( 80, 120, 200)   // muted blue

// Track dot colour (fading: newer = brighter)
#define TRACK_COL       lv_color_make(100, 200, 120)        // green
#define TRACK_COL_OLD   lv_color_make( 30,  80,  50)

// Boat dot colour
#define BOAT_COL        lv_color_white()

// Anchor dot colour
#define ANCHOR_COL      lv_color_make(255, 200,  50)        // amber

// ── Per-screen LVGL objects ───────────────────────────────────────────────────
static lv_obj_t* a_bg[NUM_SCREENS];        // full-screen bg
static lv_obj_t* a_canvas[NUM_SCREENS];    // drawing canvas
static lv_color_t* a_cbuf[NUM_SCREENS];    // PSRAM canvas buffer

// Status-band label (distance, bearing, max-drift, radius)
static lv_obj_t* a_status_lbl[NUM_SCREENS];

// Alarm toggle button label
static lv_obj_t* a_alarm_lbl[NUM_SCREENS];

// Radius label
static lv_obj_t* a_radius_lbl[NUM_SCREENS];

static bool a_created[NUM_SCREENS] = {};

// ── Per-screen anchor state ───────────────────────────────────────────────────
struct AnchorState {
    float   anchor_lat;          // NAN = not set
    float   anchor_lon;
    float   radius_m;            // alarm radius in metres
    bool    alarm_armed;
    float   max_drift_m;         // maximum observed distance since arm
    // Track ring buffer (PSRAM)
    float*  track_lat;
    float*  track_lon;
    uint16_t track_head;         // next write index
    uint16_t track_count;        // number of valid points
    uint32_t last_track_ms;      // last time a track point was stored
    uint32_t last_alarm_ms;       // last time buzzer was triggered
    // Last own-boat position (for tap-to-anchor coordinate conversion)
    float   last_own_lat;
    float   last_own_lon;
};

static AnchorState g_state[NUM_SCREENS];

// ── Haversine distance (metres) ──────────────────────────────────────────────
static float haversine_m(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f;
    float dlat = (lat2 - lat1) * (float)DEG_TO_RAD;
    float dlon = (lon2 - lon1) * (float)DEG_TO_RAD;
    float a = sinf(dlat / 2) * sinf(dlat / 2) +
              cosf(lat1 * (float)DEG_TO_RAD) * cosf(lat2 * (float)DEG_TO_RAD) *
              sinf(dlon / 2) * sinf(dlon / 2);
    return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

// ── Bearing from point1 to point2 (degrees, 0=N) ────────────────────────────
static float bearing_deg(float lat1, float lon1, float lat2, float lon2) {
    float dlon = (lon2 - lon1) * (float)DEG_TO_RAD;
    float y = sinf(dlon) * cosf(lat2 * (float)DEG_TO_RAD);
    float x = cosf(lat1 * (float)DEG_TO_RAD) * sinf(lat2 * (float)DEG_TO_RAD) -
              sinf(lat1 * (float)DEG_TO_RAD) * cosf(lat2 * (float)DEG_TO_RAD) * cosf(dlon);
    float b = atan2f(y, x) * (float)RAD_TO_DEG;
    if (b < 0) b += 360.0f;
    return b;
}

// ── Get screen parent object ─────────────────────────────────────────────────
static lv_obj_t* get_screen_obj(int s) {
    switch (s) {
        case 0: return ui_Screen1;
        case 1: return ui_Screen2;
        case 2: return ui_Screen3;
        case 3: return ui_Screen4;
        case 4: return ui_Screen5;
        default: return NULL;
    }
}

static lv_obj_t* get_bg_img_obj(int s) {
    switch (s) {
        case 0: return ui_RevTemp;
        case 1: return ui_RevFuel;
        case 2: return ui_TempExhaust;
        case 3: return ui_FuelTemp;
        case 4: return ui_OilTemp;
        default: return NULL;
    }
}

// ── Canvas helpers ───────────────────────────────────────────────────────────
static void canvas_px(lv_obj_t* cv, int x, int y, lv_color_t c) {
    if (x >= 0 && x < SCR_W && y >= 0 && y < MAP_H)
        lv_canvas_set_px_color(cv, x, y, c);
}

static void canvas_circle(lv_obj_t* cv, int cx, int cy, int r, lv_color_t c, int thickness) {
    for (int t = 0; t < thickness; t++) {
        int rr = r - t;
        if (rr <= 0) break;
        for (int i = 0; i < 720; i++) {
            float a = (i * 3.14159f * 2.0f) / 720.0f;
            canvas_px(cv, cx + (int)(rr * cosf(a)), cy + (int)(rr * sinf(a)), c);
        }
    }
}

// Draw a small filled square dot
static void canvas_dot(lv_obj_t* cv, int x, int y, int sz, lv_color_t c) {
    for (int dy = -sz; dy <= sz; dy++)
        for (int dx = -sz; dx <= sz; dx++)
            canvas_px(cv, x + dx, y + dy, c);
}

// Fill a triangle defined by three vertices
static void canvas_triangle(lv_obj_t* cv, int x0, int y0, int x1, int y1, int x2, int y2, lv_color_t c) {
    int minx = x0; if (x1 < minx) minx = x1; if (x2 < minx) minx = x2;
    int maxx = x0; if (x1 > maxx) maxx = x1; if (x2 > maxx) maxx = x2;
    int miny = y0; if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    int maxy = y0; if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            int d0 = (x1-x0)*(py-y0) - (y1-y0)*(px-x0);
            int d1 = (x2-x1)*(py-y1) - (y2-y1)*(px-x1);
            int d2 = (x0-x2)*(py-y2) - (y0-y2)*(px-x2);
            bool has_neg = (d0 < 0) || (d1 < 0) || (d2 < 0);
            bool has_pos = (d0 > 0) || (d1 > 0) || (d2 > 0);
            if (!(has_neg && has_pos)) canvas_px(cv, px, py, c);
        }
    }
}

// Draw a filled triangle (boat): tip points in direction of cog_deg (0=up=north)
static void canvas_arrow(lv_obj_t* cv, int cx, int cy, float cog_deg, lv_color_t c) {
    float fa   = (cog_deg - 90.0f) * (float)DEG_TO_RAD;
    float perp = fa + (float)M_PI_2;
    // Tip: 13px forward
    int tx = cx + (int)(13.0f * cosf(fa));
    int ty = cy + (int)(13.0f * sinf(fa));
    // Base centre: 8px back; base half-width: 7px
    float bx = cx - 8.0f * cosf(fa);
    float by = cy - 8.0f * sinf(fa);
    int lx = (int)(bx + 7.0f * cosf(perp)),  ly = (int)(by + 7.0f * sinf(perp));
    int rx = (int)(bx - 7.0f * cosf(perp)),  ry = (int)(by - 7.0f * sinf(perp));
    canvas_triangle(cv, tx, ty, lx, ly, rx, ry, c);
}

// Draw anchor icon (small cross + ring) at pixel position
static void canvas_anchor(lv_obj_t* cv, int cx, int cy, lv_color_t c) {
    // Vertical bar
    for (int i = -12; i <= 12; i++) canvas_px(cv, cx, cy + i, c);
    // Horizontal bar (top)
    for (int i = -7; i <= 7; i++)  canvas_px(cv, cx + i, cy - 12, c);
    // Flukes (angled lines at bottom)
    for (int i = 0; i < 8; i++) {
        canvas_px(cv, cx - i, cy + 8 + i / 2, c);
        canvas_px(cv, cx + i, cy + 8 + i / 2, c);
    }
    // Small ring at top
    canvas_circle(cv, cx, cy - 12, 4, c, 1);
}

// ── Save/load anchor state to NVS ────────────────────────────────────────────
static void anchor_save(int n) {
    char ns[16];
    snprintf(ns, sizeof(ns), "anchor_%d", n);
    Preferences prefs;
    if (prefs.begin(ns, false)) {
        prefs.putFloat("lat",    g_state[n].anchor_lat);
        prefs.putFloat("lon",    g_state[n].anchor_lon);
        prefs.putFloat("radius", g_state[n].radius_m);
        prefs.putBool ("armed",  g_state[n].alarm_armed);
        prefs.end();
    }
}

static void anchor_load(int n) {
    char ns[16];
    snprintf(ns, sizeof(ns), "anchor_%d", n);
    Preferences prefs;
    if (prefs.begin(ns, true)) {
        g_state[n].anchor_lat  = prefs.getFloat("lat",    NAN);
        g_state[n].anchor_lon  = prefs.getFloat("lon",    NAN);
        g_state[n].radius_m    = prefs.getFloat("radius", 30.0f);
        g_state[n].alarm_armed = prefs.getBool ("armed",  false);
        prefs.end();
    } else {
        g_state[n].anchor_lat  = NAN;
        g_state[n].anchor_lon  = NAN;
        g_state[n].radius_m    = 30.0f;
        g_state[n].alarm_armed = false;
    }
}

// ── Update radius label ───────────────────────────────────────────────────────
static void update_radius_lbl(int n) {
    if (!a_radius_lbl[n]) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f m", g_state[n].radius_m);
    lv_label_set_text(a_radius_lbl[n], buf);
}

static void update_alarm_lbl(int n) {
    if (!a_alarm_lbl[n]) return;
    lv_label_set_text(a_alarm_lbl[n],
        g_state[n].alarm_armed ? "ALARM ON" : "ALARM OFF");
    lv_obj_set_style_bg_color(lv_obj_get_parent(a_alarm_lbl[n]),
        g_state[n].alarm_armed ? lv_color_make(180, 30, 30) : lv_color_make(40, 60, 90), 0);
}

// ── Create ────────────────────────────────────────────────────────────────────
void anchor_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (a_created[n]) anchor_display_destroy(n);

    // Init state
    if (!g_state[n].track_lat) {
        g_state[n].track_lat = (float*)heap_caps_malloc(
            TRACK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_state[n].track_lon = (float*)heap_caps_malloc(
            TRACK_MAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_state[n].track_lat || !g_state[n].track_lon) {
            Serial.printf("[ANCHOR] PSRAM alloc failed for screen %d\n", n);
        }
    }
    g_state[n].track_head     = 0;
    g_state[n].track_count    = 0;
    g_state[n].last_track_ms  = 0;
    g_state[n].max_drift_m    = 0.0f;
    g_state[n].last_own_lat   = NAN;
    g_state[n].last_own_lon   = NAN;
    anchor_load(n);

    lv_obj_t* parent = get_screen_obj(n);
    if (!parent) return;

    lv_obj_t* bg_img = get_bg_img_obj(n);
    if (bg_img) lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);

    // ── Full-screen black background ─────────────────────────────────────────
    a_bg[n] = lv_obj_create(parent);
    lv_obj_remove_style_all(a_bg[n]);
    lv_obj_set_size(a_bg[n], SCR_W, SCR_H);
    lv_obj_set_pos(a_bg[n], 0, 0);
    lv_obj_set_style_bg_color(a_bg[n], lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a_bg[n], LV_OPA_COVER, 0);
    lv_obj_clear_flag(a_bg[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a_bg[n], LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_GESTURE_BUBBLE);

    // ── Canvas for map area ───────────────────────────────────────────────────
    size_t buf_sz = (size_t)SCR_W * MAP_H * sizeof(lv_color_t);
    a_cbuf[n] = (lv_color_t*)heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a_cbuf[n]) {
        Serial.printf("[ANCHOR] canvas PSRAM alloc failed s=%d\n", n);
        return;
    }
    a_canvas[n] = lv_canvas_create(a_bg[n]);
    lv_canvas_set_buffer(a_canvas[n], a_cbuf[n], SCR_W, MAP_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(a_canvas[n], 0, 0);
    lv_canvas_fill_bg(a_canvas[n], lv_color_black(), LV_OPA_COVER);
    // Canvas is NOT clickable — swipe gestures pass through to parent screen
    lv_obj_add_flag(a_canvas[n], LV_OBJ_FLAG_GESTURE_BUBBLE);

    // ── Status label — top of map area, drawn over canvas ────────────────────
    a_status_lbl[n] = lv_label_create(a_bg[n]);
    lv_obj_set_style_text_color(a_status_lbl[n], lv_color_make(200, 200, 200), 0);
    lv_obj_set_style_text_font(a_status_lbl[n], &inter_16, 0);
    lv_obj_set_pos(a_status_lbl[n], 6, 4);
    lv_label_set_text(a_status_lbl[n], "No GPS");

    // ── D-pad: anchor nudge buttons (bottom-right of map area) ───────────────
    // Each tap moves anchor by 10% of alarm radius. Uses symbol font (no inter_* needed).
    {
        struct DpadDef { int x, y, dir; const char* sym; };
        const DpadDef dp[5] = {
            { 408, 282, 0, LV_SYMBOL_UP    },   // ▲ North
            { 374, 318, 1, LV_SYMBOL_LEFT  },   // ◄ West
            { 442, 318, 2, LV_SYMBOL_RIGHT },   // ► East
            { 408, 354, 3, LV_SYMBOL_DOWN  },   // ▼ South
            { 408, 318, 4, LV_SYMBOL_HOME  },   // ⌂ Recentre anchor on boat
        };
        for (int d = 0; d < 5; d++) {
            lv_obj_t* btn = lv_obj_create(a_bg[n]);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 34, 34);
            lv_obj_set_pos(btn, dp[d].x, dp[d].y);
            lv_obj_set_style_bg_color(btn, lv_color_make(40, 50, 75), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
            lv_obj_set_style_radius(btn, 5, 0);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* lbl = lv_label_create(btn);
            lv_label_set_text(lbl, dp[d].sym);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
            lv_obj_center(lbl);
            // user_data: screen index in upper byte, direction in lower byte
            lv_obj_set_user_data(btn, (void*)(intptr_t)((n << 8) | dp[d].dir));
            lv_obj_add_event_cb(btn, [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
                intptr_t ud = (intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
                int s   = (int)(ud >> 8);
                int dir = (int)(ud & 0xFF);
                if (isnan(g_state[s].anchor_lat) || isnan(g_state[s].anchor_lon)) return;
                float step_m = g_state[s].radius_m * 0.10f;
                float dlat = step_m / 111111.0f;
                float dlon = step_m / (111111.0f * cosf(g_state[s].anchor_lat * (float)DEG_TO_RAD));
                switch (dir) {
                    case 0: g_state[s].anchor_lat += dlat; break;  // North
                    case 1: g_state[s].anchor_lon -= dlon; break;  // West
                    case 2: g_state[s].anchor_lon += dlon; break;  // East
                    case 3: g_state[s].anchor_lat -= dlat; break;  // South
                    case 4:  // Recentre: move anchor to current boat position
                        if (!isnan(g_state[s].last_own_lat) && !isnan(g_state[s].last_own_lon)) {
                            g_state[s].anchor_lat = g_state[s].last_own_lat;
                            g_state[s].anchor_lon = g_state[s].last_own_lon;
                        }
                        break;
                }
                g_state[s].max_drift_m = 0.0f;
                anchor_save(s);
            }, LV_EVENT_CLICKED, NULL);
        }
    }

    // ── Status band (bottom STATUS_H px) ─────────────────────────────────────
    lv_obj_t* band = lv_obj_create(a_bg[n]);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, SCR_W, STATUS_H);
    lv_obj_set_pos(band, 0, MAP_H);
    lv_obj_set_style_bg_color(band, lv_color_make(15, 15, 25), 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Button row: [−] radius [+]  |  [Drop Here]  |  [ALARM ON/OFF]
    // Buttons are 52×44 (± ) and 120×44 (Drop Here, Alarm)
    int by = (STATUS_H - 44) / 2;   // vertically centred in band

    // [−] button
    lv_obj_t* btn_minus = lv_obj_create(band);
    lv_obj_remove_style_all(btn_minus);
    lv_obj_set_size(btn_minus, 52, 44);
    lv_obj_set_pos(btn_minus, 6, by);
    lv_obj_set_style_bg_color(btn_minus, lv_color_make(50, 50, 70), 0);
    lv_obj_set_style_bg_opa(btn_minus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_minus, 6, 0);
    lv_obj_add_flag(btn_minus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* lbl_minus = lv_label_create(btn_minus);
    lv_label_set_text(lbl_minus, "-");
    lv_obj_set_style_text_color(lbl_minus, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_minus, &inter_16, 0);
    lv_obj_center(lbl_minus);

    // Radius label (between ± buttons, left cluster)
    a_radius_lbl[n] = lv_label_create(band);
    lv_obj_set_style_text_color(a_radius_lbl[n], lv_color_white(), 0);
    lv_obj_set_style_text_font(a_radius_lbl[n], &inter_16, 0);
    lv_obj_set_pos(a_radius_lbl[n], 62, by + 14);
    update_radius_lbl(n);

    // [+] button
    lv_obj_t* btn_plus = lv_obj_create(band);
    lv_obj_remove_style_all(btn_plus);
    lv_obj_set_size(btn_plus, 52, 44);
    lv_obj_set_pos(btn_plus, 118, by);
    lv_obj_set_style_bg_color(btn_plus, lv_color_make(50, 50, 70), 0);
    lv_obj_set_style_bg_opa(btn_plus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_plus, 6, 0);
    lv_obj_add_flag(btn_plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* lbl_plus = lv_label_create(btn_plus);
    lv_label_set_text(lbl_plus, "+");
    lv_obj_set_style_text_color(lbl_plus, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_plus, &inter_16, 0);
    lv_obj_center(lbl_plus);

    // [Drop Here] button — horizontally centred
    lv_obj_t* btn_drop = lv_obj_create(band);
    lv_obj_remove_style_all(btn_drop);
    lv_obj_set_size(btn_drop, 120, 44);
    lv_obj_set_pos(btn_drop, SCR_W / 2 - 60, by);
    lv_obj_set_style_bg_color(btn_drop, lv_color_make(30, 80, 150), 0);
    lv_obj_set_style_bg_opa(btn_drop, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_drop, 6, 0);
    lv_obj_add_flag(btn_drop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* lbl_drop = lv_label_create(btn_drop);
    lv_label_set_text(lbl_drop, "Drop Here");
    lv_obj_set_style_text_color(lbl_drop, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_drop, &inter_16, 0);
    lv_obj_center(lbl_drop);

    // [ALARM ON/OFF] button — right edge
    lv_obj_t* btn_alarm = lv_obj_create(band);
    lv_obj_remove_style_all(btn_alarm);
    lv_obj_set_size(btn_alarm, 120, 44);
    lv_obj_set_pos(btn_alarm, SCR_W - 128, by);
    lv_obj_set_style_bg_color(btn_alarm,
        g_state[n].alarm_armed ? lv_color_make(180, 30, 30) : lv_color_make(40, 60, 90), 0);
    lv_obj_set_style_bg_opa(btn_alarm, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_alarm, 6, 0);
    lv_obj_add_flag(btn_alarm, LV_OBJ_FLAG_CLICKABLE);
    a_alarm_lbl[n] = lv_label_create(btn_alarm);
    lv_obj_set_style_text_color(a_alarm_lbl[n], lv_color_white(), 0);
    lv_obj_set_style_text_font(a_alarm_lbl[n], &inter_16, 0);
    update_alarm_lbl(n);
    lv_obj_center(a_alarm_lbl[n]);

    // ── Event callbacks (lambdas via user_data = screen index) ────────────────
    // [−] radius
    lv_obj_set_user_data(btn_minus, (void*)(intptr_t)n);
    lv_obj_add_event_cb(btn_minus, [](lv_event_t* e) {
        int s = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            g_state[s].radius_m -= RADIUS_STEP_M;
            if (g_state[s].radius_m < RADIUS_MIN_M) g_state[s].radius_m = RADIUS_MIN_M;
            update_radius_lbl(s);
            anchor_save(s);
        }
    }, LV_EVENT_CLICKED, NULL);

    // [+] radius
    lv_obj_set_user_data(btn_plus, (void*)(intptr_t)n);
    lv_obj_add_event_cb(btn_plus, [](lv_event_t* e) {
        int s = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            g_state[s].radius_m += RADIUS_STEP_M;
            if (g_state[s].radius_m > RADIUS_MAX_M) g_state[s].radius_m = RADIUS_MAX_M;
            update_radius_lbl(s);
            anchor_save(s);
        }
    }, LV_EVENT_CLICKED, NULL);

    // [Drop Here]
    lv_obj_set_user_data(btn_drop, (void*)(intptr_t)n);
    lv_obj_add_event_cb(btn_drop, [](lv_event_t* e) {
        int s = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            float lat = g_state[s].last_own_lat;
            float lon = g_state[s].last_own_lon;
            if (!isnan(lat) && !isnan(lon)) {
                g_state[s].anchor_lat  = lat;
                g_state[s].anchor_lon  = lon;
                g_state[s].max_drift_m = 0.0f;
                anchor_save(s);
                Serial.printf("[ANCHOR] s=%d dropped at %.6f,%.6f\n", s, lat, lon);
            }
        }
    }, LV_EVENT_CLICKED, NULL);

    // [ALARM ON/OFF]
    lv_obj_set_user_data(btn_alarm, (void*)(intptr_t)n);
    lv_obj_add_event_cb(btn_alarm, [](lv_event_t* e) {
        int s = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            g_state[s].alarm_armed = !g_state[s].alarm_armed;
            g_state[s].max_drift_m = 0.0f;
            update_alarm_lbl(s);
            anchor_save(s);
        }
    }, LV_EVENT_CLICKED, NULL);

    a_created[n] = true;
    Serial.printf("[ANCHOR] Created on screen %d\n", n);
}

// ── Touch handler (tap on map area → set anchor at that lat/lon) ─────────────
void anchor_display_touch(int n, int x, int y) {
    if (!a_created[n]) return;
    // Only handle taps in the map area (above status band)
    if (y >= MAP_H) return;

    float lat = g_state[n].last_own_lat;
    float lon  = g_state[n].last_own_lon;

    // If we have no GPS yet, we can't convert pixels to lat/lon
    if (isnan(lat) || isnan(lon)) {
        // Try using anchor position as reference if set
        if (isnan(g_state[n].anchor_lat)) return;
        lat = g_state[n].anchor_lat;
        lon = g_state[n].anchor_lon;
    }

    // Pixel offset from centre of map
    float px_per_m = (float)ALARM_RING_R / g_state[n].radius_m;
    float dx_px = (float)(x - CX);
    float dy_px = (float)(y - MAP_CY);

    // Convert pixel offset to metres (dy inverted: screen y increases downward = south)
    float north_m = -dy_px / px_per_m;
    float east_m  =  dx_px / px_per_m;

    // Convert metres offset from last-known position to lat/lon
    // 1 degree lat ≈ 111111 m; 1 degree lon ≈ 111111 * cos(lat) m
    // Reference: anchor centre is at "reference lat/lon" in the current view.
    // The map centre is the anchor if set, otherwise the boat.
    float ref_lat = !isnan(g_state[n].anchor_lat) ? g_state[n].anchor_lat : lat;
    float ref_lon = !isnan(g_state[n].anchor_lon) ? g_state[n].anchor_lon : lon;

    float new_lat = ref_lat + north_m / 111111.0f;
    float new_lon = ref_lon + east_m  / (111111.0f * cosf(ref_lat * (float)DEG_TO_RAD));

    g_state[n].anchor_lat  = new_lat;
    g_state[n].anchor_lon  = new_lon;
    g_state[n].max_drift_m = 0.0f;
    anchor_save(n);
    Serial.printf("[ANCHOR] s=%d tap→anchor %.6f,%.6f\n", n, new_lat, new_lon);
}

// ── Update (called every loop tick) ─────────────────────────────────────────
void anchor_display_update(int n, float own_lat, float own_lon,
                           float cog_deg, float sog_ms) {
    if (!a_created[n] || !a_canvas[n]) return;

    // Store own position for tap-to-anchor conversion
    g_state[n].last_own_lat = own_lat;
    g_state[n].last_own_lon = own_lon;

    // Accumulate track point
    uint32_t now = millis();
    bool has_gps = (!isnan(own_lat) && !isnan(own_lon));
    if (has_gps && (now - g_state[n].last_track_ms >= TRACK_INTERVAL_MS)) {
        if (g_state[n].track_lat && g_state[n].track_lon) {
            g_state[n].track_lat[g_state[n].track_head] = own_lat;
            g_state[n].track_lon[g_state[n].track_head] = own_lon;
            g_state[n].track_head = (g_state[n].track_head + 1) % TRACK_MAX;
            if (g_state[n].track_count < TRACK_MAX) g_state[n].track_count++;
        }
        g_state[n].last_track_ms = now;
    }

    // Anchor state
    bool has_anchor = !isnan(g_state[n].anchor_lat) && !isnan(g_state[n].anchor_lon);
    float dist_m = NAN, brg_to_anchor = NAN;
    if (has_gps && has_anchor) {
        dist_m = haversine_m(own_lat, own_lon, g_state[n].anchor_lat, g_state[n].anchor_lon);
        brg_to_anchor = bearing_deg(own_lat, own_lon, g_state[n].anchor_lat, g_state[n].anchor_lon);
        if (dist_m > g_state[n].max_drift_m) g_state[n].max_drift_m = dist_m;

        // Trigger alarm if armed and outside radius — respect cooldown from buzzer settings
        if (g_state[n].alarm_armed && dist_m > g_state[n].radius_m) {
            uint32_t now_ms = millis();
            uint32_t cooldown_ms = (buzzer_cooldown_sec == 0) ? 0 : (uint32_t)buzzer_cooldown_sec * 1000UL;
            if (now_ms - g_state[n].last_alarm_ms >= cooldown_ms) {
                g_state[n].last_alarm_ms = now_ms;
                trigger_buzzer_alert();
            }
        }
    }

    // ── Redraw canvas ────────────────────────────────────────────────────────
    lv_canvas_fill_bg(a_canvas[n], lv_color_make(8, 12, 20), LV_OPA_COVER);

    // Scale: ALARM_RING_R pixels = radius_m metres
    float px_per_m = (float)ALARM_RING_R / g_state[n].radius_m;

    // Reference point for drawing (anchor if set, else boat)
    float ref_lat = has_anchor ? g_state[n].anchor_lat : (has_gps ? own_lat : 0.0f);
    float ref_lon = has_anchor ? g_state[n].anchor_lon : (has_gps ? own_lon : 0.0f);

    // ── Draw track dots ───────────────────────────────────────────────────────
    if (g_state[n].track_lat && g_state[n].track_count > 0) {
        uint16_t cnt = g_state[n].track_count;
        uint16_t head = g_state[n].track_head;
        for (uint16_t i = 0; i < cnt; i++) {
            // Oldest first (i=0 oldest when cnt==TRACK_MAX)
            uint16_t idx = (head + TRACK_MAX - cnt + i) % TRACK_MAX;
            float tlat = g_state[n].track_lat[idx];
            float tlon = g_state[n].track_lon[idx];
            // Offset from reference in metres
            float north = (tlat - ref_lat) * 111111.0f;
            float east  = (tlon - ref_lon) * 111111.0f * cosf(ref_lat * (float)DEG_TO_RAD);
            int tx = CX  + (int)(east  * px_per_m);
            int ty = MAP_CY - (int)(north * px_per_m);   // north = up = negative screen y
            // Fade colour: newer = brighter
            float frac = (float)i / (float)(cnt > 1 ? cnt - 1 : 1);
            uint8_t r = (uint8_t)(30  + frac * 70);
            uint8_t g2= (uint8_t)(80  + frac * 120);
            uint8_t b = (uint8_t)(50  + frac * 30);
            canvas_dot(a_canvas[n], tx, ty, 1, lv_color_make(r, g2, b));
        }
    }

    // ── Draw alarm ring ───────────────────────────────────────────────────────
    lv_color_t ring_col = g_state[n].alarm_armed ? ALARM_COL_ARMED : ALARM_COL_DISARMED;
    canvas_circle(a_canvas[n], CX, MAP_CY, ALARM_RING_R, ring_col, 2);

    // ── Draw anchor icon at centre ────────────────────────────────────────────
    if (has_anchor) {
        canvas_anchor(a_canvas[n], CX, MAP_CY, ANCHOR_COL);
    } else {
        // Show a crosshair until anchor is set
        for (int i = -12; i <= 12; i++) {
            canvas_px(a_canvas[n], CX + i, MAP_CY, lv_color_make(100, 100, 100));
            canvas_px(a_canvas[n], CX, MAP_CY + i, lv_color_make(100, 100, 100));
        }
    }

    // ── Draw boat icon ────────────────────────────────────────────────────────
    if (has_gps && has_anchor) {
        float north = (own_lat - ref_lat) * 111111.0f;
        float east  = (own_lon - ref_lon) * 111111.0f * cosf(ref_lat * (float)DEG_TO_RAD);
        int bx2 = CX  + (int)(east  * px_per_m);
        int by2 = MAP_CY - (int)(north * px_per_m);
        // Clamp to BOAT_CLIP_R from map centre
        float d = sqrtf((float)((bx2 - CX) * (bx2 - CX) + (by2 - MAP_CY) * (by2 - MAP_CY)));
        if (d > BOAT_CLIP_R) {
            bx2 = CX  + (int)((bx2 - CX)  * BOAT_CLIP_R / d);
            by2 = MAP_CY + (int)((by2 - MAP_CY) * BOAT_CLIP_R / d);
        }
        float cog = isnan(cog_deg) ? 0.0f : cog_deg;
        canvas_arrow(a_canvas[n], bx2, by2, cog, BOAT_COL);
    }

    lv_obj_invalidate(a_canvas[n]);

    // ── Status label ──────────────────────────────────────────────────────────
    if (!a_status_lbl[n]) return;
    char buf[80];
    if (!has_gps) {
        snprintf(buf, sizeof(buf), "No GPS");
    } else if (!has_anchor) {
        snprintf(buf, sizeof(buf), "Tap map or Drop Here to set anchor");
    } else {
        float brg_from = isnan(brg_to_anchor) ? 0.0f :
            fmodf(brg_to_anchor + 180.0f, 360.0f);  // bearing FROM anchor
        snprintf(buf, sizeof(buf), "%.0fm  %.0f\xc2\xb0  Max:%.0fm",
            dist_m, brg_from, g_state[n].max_drift_m);
    }
    lv_label_set_text(a_status_lbl[n], buf);
}

// ── Destroy ───────────────────────────────────────────────────────────────────
void anchor_display_destroy(int n) {
    if (!a_created[n]) return;
    if (a_bg[n]) { lv_obj_del(a_bg[n]); a_bg[n] = NULL; }
    if (a_cbuf[n]) { heap_caps_free(a_cbuf[n]); a_cbuf[n] = NULL; }
    a_canvas[n] = NULL;
    a_status_lbl[n] = NULL;
    a_alarm_lbl[n]  = NULL;
    a_radius_lbl[n] = NULL;
    // Track PSRAM buffers are intentionally kept across destroy/create
    // so the track survives a display type change.
    a_created[n] = false;
}
