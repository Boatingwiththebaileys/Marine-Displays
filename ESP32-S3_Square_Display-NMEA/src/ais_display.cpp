#include "ais_display.h"
#include "ui.h"
#include "screen_config_c_api.h"
#include "unit_convert.h"
#include <Arduino.h>
#include <math.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>

// ─── PSRAM JSON allocator (matches signalk_config.cpp pattern) ───────────────
struct PsramAllocatorAis {
    void* allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
    void  deallocate(void* ptr) { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t new_size) { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};

// ─── Constants ────────────────────────────────────────────────────────────────
#define DISPLAY_CX       240
#define DISPLAY_CY       240
#define PLOT_RADIUS      210
#define OWN_SHIP_SIZE     14
#define TARGET_SIZE        8
#define NM_TO_METRES   1852.0f
#define DEG_TO_RAD     (M_PI / 180.0f)
#define RAD_TO_DEG     (180.0f / M_PI)

static const float range_nm[]  = { 0.1f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f };
static const int   ring_count[] = {  5,    5,    5,    4,    5,    5,     4    };
//  ring intervals:               0.02  0.1  0.2   0.5   1.0   2.0    5.0  NM

// ─── AIS target data ─────────────────────────────────────────────────────────
typedef struct {
    float lat, lon;     // degrees
    float cog;          // degrees true
    float sog;          // knots
    char  name[21];
    char  mmsi[12];
    bool  valid;
} AisTarget;

static AisTarget  g_ais_targets[AIS_MAX_TARGETS];
static int        g_ais_target_count = 0;
static SemaphoreHandle_t g_ais_mutex = NULL;
static unsigned long g_ais_last_fetch = 0;
static int           g_ais_fail_count = 0;
static bool          g_ais_self_key_failed = false;
#define AIS_FETCH_INTERVAL_MS 5000
#define AIS_FAIL_BACKOFF_MAX  60000   // back off to 1 min after repeated failures

// ─── LVGL objects per screen ─────────────────────────────────────────────────
static lv_obj_t*   a_bg[NUM_SCREENS]        = {};
static lv_obj_t*   a_canvas[NUM_SCREENS]    = {};
static lv_color_t* a_cbuf[NUM_SCREENS]      = {};
static lv_obj_t*   a_range_lbl[NUM_SCREENS] = {};
static lv_obj_t*   a_sog_lbl[NUM_SCREENS]   = {};
static lv_obj_t*   a_cog_lbl[NUM_SCREENS]   = {};
static lv_obj_t*   a_tgt_lbl[NUM_SCREENS]   = {};

// ─── Helpers ──────────────────────────────────────────────────────────────────
static lv_obj_t* screen_obj(int n) {
    switch (n) {
        case 0: return ui_Screen1;  case 1: return ui_Screen2;
        case 2: return ui_Screen3;  case 3: return ui_Screen4;
        case 4: return ui_Screen5;  default: return NULL;
    }
}

static lv_color_t parse_color(const char* hex) {
    if (!hex || hex[0] != '#') return lv_color_white();
    unsigned int r, g, b;
    if (sscanf(hex, "#%02x%02x%02x", &r, &g, &b) == 3)
        return lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
    return lv_color_white();
}

static float haversine_m(float lat1, float lon1, float lat2, float lon2) {
    float dlat = (lat2 - lat1) * DEG_TO_RAD;
    float dlon = (lon2 - lon1) * DEG_TO_RAD;
    float a = sinf(dlat / 2) * sinf(dlat / 2) +
              cosf(lat1 * DEG_TO_RAD) * cosf(lat2 * DEG_TO_RAD) *
              sinf(dlon / 2) * sinf(dlon / 2);
    float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
    return 6371000.0f * c;
}

static float bearing_deg(float lat1, float lon1, float lat2, float lon2) {
    float dlon = (lon2 - lon1) * DEG_TO_RAD;
    float y = sinf(dlon) * cosf(lat2 * DEG_TO_RAD);
    float x = cosf(lat1 * DEG_TO_RAD) * sinf(lat2 * DEG_TO_RAD) -
              sinf(lat1 * DEG_TO_RAD) * cosf(lat2 * DEG_TO_RAD) * cosf(dlon);
    float brg = atan2f(y, x) * RAD_TO_DEG;
    if (brg < 0) brg += 360.0f;
    return brg;
}

// Bresenham line on canvas
static void canvas_line(lv_obj_t* canvas, int x0, int y0, int x1, int y1, lv_color_t col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < 480 && y0 >= 0 && y0 < 480)
            lv_canvas_set_px_color(canvas, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draw a circle outline on canvas using pixel plotting
static void canvas_circle(lv_obj_t* canvas, int cx, int cy, int radius, lv_color_t col) {
    const int segments = 180;
    for (int s = 0; s < segments; s++) {
        float a = (360.0f * s / segments) * DEG_TO_RAD;
        int px = cx + (int)(radius * cosf(a));
        int py = cy + (int)(radius * sinf(a));
        if (px >= 0 && px < 480 && py >= 0 && py < 480)
            lv_canvas_set_px_color(canvas, px, py, col);
    }
}

// ─── Public: Create ──────────────────────────────────────────────────────────
void ais_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    ais_display_destroy(n);

    if (!g_ais_mutex) g_ais_mutex = xSemaphoreCreateMutex();

    lv_obj_t* scr = screen_obj(n);
    if (!scr) return;

    // Hide gauge needles and icons (same pattern as compass_display)
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

    // Background panel
    lv_color_t bg_col = parse_color(screen_configs[n].number_bg_color[0]
                                     ? screen_configs[n].number_bg_color : "#001020");
    a_bg[n] = lv_obj_create(scr);
    lv_obj_set_size(a_bg[n], 480, 480);
    lv_obj_set_pos(a_bg[n], 0, 0);
    lv_obj_set_style_bg_color(a_bg[n], bg_col, 0);
    lv_obj_set_style_bg_opa(a_bg[n], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a_bg[n], 0, 0);
    lv_obj_set_style_pad_all(a_bg[n], 0, 0);
    lv_obj_clear_flag(a_bg[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(a_bg[n], LV_OBJ_FLAG_FLOATING);  // Ignore parent layout

    // Canvas for radar plot (allocated in PSRAM)
    size_t buf_size = LV_CANVAS_BUF_SIZE_TRUE_COLOR(480, 480);
    a_cbuf[n] = (lv_color_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (a_cbuf[n]) {
        a_canvas[n] = lv_canvas_create(a_bg[n]);
        lv_canvas_set_buffer(a_canvas[n], a_cbuf[n], 480, 480, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_pos(a_canvas[n], 0, 0);
        lv_obj_clear_flag(a_canvas[n], LV_OBJ_FLAG_CLICKABLE);
        lv_canvas_fill_bg(a_canvas[n], bg_col, LV_OPA_COVER);
    } else {
        Serial.println("[AIS] PSRAM alloc failed for canvas buffer");
    }

    // Info labels — using inter_16 (project's custom font)
    auto make_label = [&](lv_obj_t*& lbl, const char* txt, lv_color_t col,
                          lv_align_t align, lv_coord_t xofs, lv_coord_t yofs) {
        lbl = lv_label_create(a_bg[n]);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &inter_16, 0);
        lv_obj_set_style_text_color(lbl, col, 0);
        lv_obj_align(lbl, align, xofs, yofs);
    };

    lv_color_t green = lv_color_make(100, 200, 100);
    lv_color_t grey  = lv_color_make(200, 200, 200);
    make_label(a_range_lbl[n], "",        green, LV_ALIGN_TOP_LEFT,     8,   8);
    make_label(a_tgt_lbl[n],   "TGT: 0", green, LV_ALIGN_TOP_RIGHT,   -8,  8);
    make_label(a_cog_lbl[n],   "COG ---", grey,  LV_ALIGN_BOTTOM_LEFT,  8, -28);
    make_label(a_sog_lbl[n],   "SOG ---", grey,  LV_ALIGN_BOTTOM_LEFT,  8,  -8);

    Serial.printf("[AIS] Display created on screen %d\n", n);
}

// ─── Public: Update (redraw radar plot) ──────────────────────────────────────
void ais_display_update(int n, float own_lat, float own_lon,
                        float own_cog, float own_sog, float own_hdg) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (!a_canvas[n] || !a_cbuf[n]) return;

    // Rate-limit canvas redraws — AIS data only updates every 5 s.
    // Also skip if nothing changed to avoid flicker.
    static unsigned long last_redraw[NUM_SCREENS] = {};
    static float prev_cog[NUM_SCREENS] = {NAN,NAN,NAN,NAN,NAN};
    static float prev_sog[NUM_SCREENS] = {NAN,NAN,NAN,NAN,NAN};
    static int   prev_tgt_count[NUM_SCREENS] = {};
    unsigned long now = millis();
    bool data_changed = (g_ais_target_count != prev_tgt_count[n])
                     || (isnan(own_cog) != isnan(prev_cog[n]))
                     || (!isnan(own_cog) && fabsf(own_cog - prev_cog[n]) > 0.5f)
                     || (isnan(own_sog) != isnan(prev_sog[n]))
                     || (!isnan(own_sog) && fabsf(own_sog - prev_sog[n]) > 0.05f);
    if (!data_changed && (now - last_redraw[n] < 2000)) return;
    last_redraw[n] = now;
    prev_cog[n] = own_cog;
    prev_sog[n] = own_sog;
    prev_tgt_count[n] = g_ais_target_count;

    // Range from config (repurpose graph_time_range field)
    uint8_t range_idx = screen_configs[n].graph_time_range;
    if (range_idx > 6) range_idx = 4;  // default 5 NM
    float range = range_nm[range_idx];
    float range_m = range * NM_TO_METRES;

    lv_color_t bg_col = parse_color(screen_configs[n].number_bg_color[0]
                                     ? screen_configs[n].number_bg_color : "#001020");
    lv_canvas_fill_bg(a_canvas[n], bg_col, LV_OPA_COVER);

    // ── Range rings (per-range count for clean round intervals) ──────────────
    int n_rings = ring_count[range_idx];
    lv_color_t ring_col = lv_color_make(40, 80, 40);
    for (int r = 1; r <= n_rings; r++) {
        int ring_r = (PLOT_RADIUS * r) / n_rings;
        canvas_circle(a_canvas[n], DISPLAY_CX, DISPLAY_CY, ring_r, ring_col);
    }

    // ── Cross hairs (N-S and E-W lines through centre) ───────────────────────
    lv_color_t xhair_col = lv_color_make(30, 60, 30);
    for (int i = DISPLAY_CY - PLOT_RADIUS; i <= DISPLAY_CY + PLOT_RADIUS; i++) {
        if (i >= 0 && i < 480)
            lv_canvas_set_px_color(a_canvas[n], DISPLAY_CX, i, xhair_col);
    }
    for (int i = DISPLAY_CX - PLOT_RADIUS; i <= DISPLAY_CX + PLOT_RADIUS; i++) {
        if (i >= 0 && i < 480)
            lv_canvas_set_px_color(a_canvas[n], i, DISPLAY_CY, xhair_col);
    }

    // ── North indicator ("N" drawn on canvas, rotated by heading for head-up) ──
    float ref_hdg = isnan(own_hdg) ? own_cog : own_hdg; // prefer heading, fall back to COG
    float north_angle = isnan(ref_hdg) ? 0.0f : -ref_hdg;
    float na_rad = north_angle * DEG_TO_RAD;
    int nx = DISPLAY_CX + (int)((PLOT_RADIUS + 12) * sinf(na_rad));
    int ny = DISPLAY_CY - (int)((PLOT_RADIUS + 12) * cosf(na_rad));
    {
        lv_draw_label_dsc_t n_dsc;
        lv_draw_label_dsc_init(&n_dsc);
        n_dsc.color = lv_color_make(255, 50, 50);
        n_dsc.font = &inter_16;
        lv_point_t n_sz;
        lv_txt_get_size(&n_sz, "N", n_dsc.font, 0, 0, 30, LV_TEXT_FLAG_NONE);
        lv_canvas_draw_text(a_canvas[n], nx - n_sz.x / 2, ny - n_sz.y / 2,
                            n_sz.x + 2, &n_dsc, "N");
    }

    // ── Own ship (centre, pointing up in head-up mode) ───────────────────────
    lv_color_t own_col = lv_color_make(255, 255, 255);
    for (int dy = -OWN_SHIP_SIZE; dy <= OWN_SHIP_SIZE; dy++) {
        int half_w = (OWN_SHIP_SIZE - abs(dy)) * 6 / 10;
        int py = DISPLAY_CY + dy;
        if (py < 0 || py >= 480) continue;
        for (int dx = -half_w; dx <= half_w; dx++) {
            int px = DISPLAY_CX + dx;
            if (px >= 0 && px < 480) {
                if (abs(dx) == half_w || dy == -OWN_SHIP_SIZE || dy == OWN_SHIP_SIZE)
                    lv_canvas_set_px_color(a_canvas[n], px, py, own_col);
            }
        }
    }

    // ── AIS targets ──────────────────────────────────────────────────────────
    int visible_count = 0;
    if (g_ais_mutex && xSemaphoreTake(g_ais_mutex, pdMS_TO_TICKS(20))) {
        for (int t = 0; t < g_ais_target_count && t < AIS_MAX_TARGETS; t++) {
            if (!g_ais_targets[t].valid) continue;
            if (isnan(own_lat) || isnan(own_lon)) continue;

            float dist_m = haversine_m(own_lat, own_lon,
                                       g_ais_targets[t].lat, g_ais_targets[t].lon);
            if (dist_m > range_m) continue;

            float brg = bearing_deg(own_lat, own_lon,
                                    g_ais_targets[t].lat, g_ais_targets[t].lon);
            // Head-up rotation uses heading (falls back to COG)
            float ref_rot = isnan(ref_hdg) ? 0.0f : ref_hdg;
            float rel_brg = brg - ref_rot;
            float rel_rad = rel_brg * DEG_TO_RAD;
            float r_px = (dist_m / range_m) * PLOT_RADIUS;
            int tx = DISPLAY_CX + (int)(r_px * sinf(rel_rad));
            int ty = DISPLAY_CY - (int)(r_px * cosf(rel_rad));

            // Target COG relative to screen (head-up)
            float tgt_cog = g_ais_targets[t].cog - ref_rot;
            float ta = tgt_cog * DEG_TO_RAD;

            lv_color_t tgt_col = lv_color_make(0, 180, 255);
            // Bow
            int bx = tx + (int)(TARGET_SIZE * sinf(ta));
            int by = ty - (int)(TARGET_SIZE * cosf(ta));
            // Port
            int plx = tx + (int)(TARGET_SIZE * 0.5f * sinf(ta + 2.4f));
            int ply = ty - (int)(TARGET_SIZE * 0.5f * cosf(ta + 2.4f));
            // Starboard
            int srx = tx + (int)(TARGET_SIZE * 0.5f * sinf(ta - 2.4f));
            int sry = ty - (int)(TARGET_SIZE * 0.5f * cosf(ta - 2.4f));

            canvas_line(a_canvas[n], bx, by, plx, ply, tgt_col);
            canvas_line(a_canvas[n], plx, ply, srx, sry, tgt_col);
            canvas_line(a_canvas[n], srx, sry, bx, by, tgt_col);

            // Name label for close targets
            if (dist_m < range_m * 0.5f) {
                const char* label = g_ais_targets[t].name[0] ? g_ais_targets[t].name
                                                              : g_ais_targets[t].mmsi;
                lv_draw_label_dsc_t lbl_dsc;
                lv_draw_label_dsc_init(&lbl_dsc);
                lbl_dsc.color = lv_color_make(0, 150, 220);
                lbl_dsc.font = &inter_16;
                lv_point_t lbl_size;
                lv_txt_get_size(&lbl_size, label, lbl_dsc.font, 0, 0, 200, LV_TEXT_FLAG_NONE);
                lv_canvas_draw_text(a_canvas[n],
                    tx - lbl_size.x / 2, ty + TARGET_SIZE + 2,
                    lbl_size.x + 4, &lbl_dsc, label);
            }
            visible_count++;
        }
        xSemaphoreGive(g_ais_mutex);
    }

    // ── Update info labels ───────────────────────────────────────────────────
    if (a_range_lbl[n]) {
        char buf[32];
        snprintf(buf, sizeof(buf), range >= 1.0f ? "%.0f NM" : "%.1f NM", range);
        lv_label_set_text(a_range_lbl[n], buf);
    }
    if (a_sog_lbl[n]) {
        char buf[24];
        snprintf(buf, sizeof(buf), isnan(own_sog) ? "SOG ---" : "SOG %.1f kn", own_sog);
        lv_label_set_text(a_sog_lbl[n], buf);
    }
    if (a_cog_lbl[n]) {
        char buf[24];
        if (isnan(own_cog))
            snprintf(buf, sizeof(buf), "COG ---");
        else
            snprintf(buf, sizeof(buf), "COG %03.0f\xc2\xb0", own_cog);
        lv_label_set_text(a_cog_lbl[n], buf);
    }
    if (a_tgt_lbl[n]) {
        char buf[16];
        snprintf(buf, sizeof(buf), "TGT: %d", visible_count);
        lv_label_set_text(a_tgt_lbl[n], buf);
    }

    lv_obj_invalidate(a_canvas[n]);
}

// ─── Public: Destroy ─────────────────────────────────────────────────────────
void ais_display_destroy(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;

    if (a_bg[n]) { lv_obj_del(a_bg[n]); a_bg[n] = NULL; }
    a_canvas[n] = NULL;
    a_range_lbl[n] = NULL;
    a_sog_lbl[n] = NULL;
    a_cog_lbl[n] = NULL;
    a_tgt_lbl[n] = NULL;

    if (a_cbuf[n]) {
        heap_caps_free(a_cbuf[n]);
        a_cbuf[n] = NULL;
    }
}

// ─── Public: Fetch AIS targets from Signal K REST API ────────────────────────
void ais_fetch_targets(const char* server_ip, uint16_t server_port) {
    if (!server_ip || server_ip[0] == '\0' || server_port == 0) return;
    // Don't attempt HTTP when WiFi isn't connected (AP mode) — the request
    // would block for the full timeout and freeze the display.
    if (WiFi.status() != WL_CONNECTED) return;

    unsigned long now = millis();
    // Back off on repeated failures: 5s → 10s → 20s → 40s → 60s cap
    unsigned long interval = AIS_FETCH_INTERVAL_MS;
    if (g_ais_fail_count > 0) {
        interval = AIS_FETCH_INTERVAL_MS * (1U << min(g_ais_fail_count, 4));
        if (interval > AIS_FAIL_BACKOFF_MAX) interval = AIS_FAIL_BACKOFF_MAX;
    }
    if (now - g_ais_last_fetch < interval) return;
    g_ais_last_fetch = now;

    if (!g_ais_mutex) g_ais_mutex = xSemaphoreCreateMutex();

    // Fetch own vessel identifier from Signal K (e.g. "vessels.urn:mrn:imo:mmsi:123456789")
    // Cache it — it never changes during a session.
    static String self_key;
    if (self_key.isEmpty() && !g_ais_self_key_failed) {
        HTTPClient hSelf;
        char selfUrl[128];
        snprintf(selfUrl, sizeof(selfUrl), "http://%s:%u/signalk/v1/api/self", server_ip, server_port);
        hSelf.setTimeout(1000);
        hSelf.begin(selfUrl);
        if (hSelf.GET() == 200) {
            self_key = hSelf.getString();
            self_key.replace("\"", "");      // strip JSON quotes
            // Strip leading "vessels." prefix so it matches the key in /vessels
            if (self_key.startsWith("vessels.")) self_key = self_key.substring(8);
            Serial.printf("[AIS] Own vessel key: %s\n", self_key.c_str());
        } else {
            g_ais_self_key_failed = true;  // don't retry every cycle
            Serial.println("[AIS] self-key fetch failed, will retry after backoff");
        }
        hSelf.end();
    }

    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/signalk/v1/api/vessels", server_ip, server_port);
    http.setTimeout(1500);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        g_ais_fail_count++;
        if (g_ais_fail_count == 1) Serial.printf("[AIS] vessels fetch failed (%d), backing off\n", httpCode);
        return;
    }

    // Success — reset failure tracking
    g_ais_fail_count = 0;
    g_ais_self_key_failed = false;

    String payload = http.getString();
    http.end();

    BasicJsonDocument<PsramAllocatorAis> doc(16384);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[AIS] JSON parse error: %s\n", err.c_str());
        return;
    }

    if (!xSemaphoreTake(g_ais_mutex, pdMS_TO_TICKS(100))) return;

    g_ais_target_count = 0;
    JsonObject vessels = doc.as<JsonObject>();
    for (JsonPair kv : vessels) {
        const char* key = kv.key().c_str();
        if (strcmp(key, "self") == 0) continue;
        // Skip own vessel (matched via /signalk/v1/api/self)
        if (self_key.length() > 0 && self_key.equals(key)) continue;

        JsonObject vessel = kv.value().as<JsonObject>();
        if (!vessel.containsKey("navigation")) continue;

        JsonObject nav = vessel["navigation"];
        if (!nav.containsKey("position")) continue;

        JsonObject pos = nav["position"];
        float lat = NAN, lon = NAN;
        if (pos.containsKey("value")) {
            JsonObject pv = pos["value"].as<JsonObject>();
            if (pv.containsKey("latitude"))  lat = pv["latitude"].as<float>();
            if (pv.containsKey("longitude")) lon = pv["longitude"].as<float>();
        } else {
            if (pos.containsKey("latitude"))  lat = pos["latitude"].as<float>();
            if (pos.containsKey("longitude")) lon = pos["longitude"].as<float>();
        }
        if (isnan(lat) || isnan(lon)) continue;

        AisTarget& tgt = g_ais_targets[g_ais_target_count];
        tgt.lat = lat;
        tgt.lon = lon;
        tgt.valid = true;

        // COG (Signal K stores radians)
        tgt.cog = 0;
        if (nav.containsKey("courseOverGroundTrue")) {
            JsonVariant cogv = nav["courseOverGroundTrue"];
            tgt.cog = convert_angle_rad(cogv.containsKey("value") ? cogv["value"].as<float>() : cogv.as<float>());
        }

        // SOG (Signal K stores m/s)
        tgt.sog = 0;
        if (nav.containsKey("speedOverGround")) {
            JsonVariant sogv = nav["speedOverGround"];
            float sog_ms = sogv.containsKey("value") ? sogv["value"].as<float>() : sogv.as<float>();
            tgt.sog = convert_speed(sog_ms);
        }

        // Vessel name
        tgt.name[0] = '\0';
        if (vessel.containsKey("name")) {
            const char* nm = vessel["name"].is<const char*>() ? vessel["name"].as<const char*>() : NULL;
            if (!nm && vessel["name"].containsKey("value"))
                nm = vessel["name"]["value"].as<const char*>();
            if (nm) { strncpy(tgt.name, nm, 20); tgt.name[20] = '\0'; }
        }

        // MMSI
        tgt.mmsi[0] = '\0';
        if (vessel.containsKey("mmsi")) {
            const char* m = vessel["mmsi"].as<const char*>();
            if (m) { strncpy(tgt.mmsi, m, 11); tgt.mmsi[11] = '\0'; }
        } else if (strlen(key) <= 11) {
            strncpy(tgt.mmsi, key, 11);
            tgt.mmsi[11] = '\0';
        }

        g_ais_target_count++;
        if (g_ais_target_count >= AIS_MAX_TARGETS) break;
    }

    xSemaphoreGive(g_ais_mutex);
}

// ─── N2K AIS target update (called from NMEA 2000 PGN handlers) ─────────────
// Find target by MMSI, or allocate a new slot.  Returns index, or -1 if full.
static int ais_find_or_alloc(uint32_t mmsi) {
    char mmsi_str[12];
    snprintf(mmsi_str, sizeof(mmsi_str), "%lu", (unsigned long)mmsi);
    // Search existing
    for (int i = 0; i < g_ais_target_count; i++) {
        if (g_ais_targets[i].valid && strcmp(g_ais_targets[i].mmsi, mmsi_str) == 0)
            return i;
    }
    // Allocate new
    if (g_ais_target_count < AIS_MAX_TARGETS) {
        int idx = g_ais_target_count++;
        memset(&g_ais_targets[idx], 0, sizeof(AisTarget));
        strncpy(g_ais_targets[idx].mmsi, mmsi_str, 11);
        g_ais_targets[idx].mmsi[11] = '\0';
        g_ais_targets[idx].valid = true;
        return idx;
    }
    return -1;
}

void ais_n2k_update_position(uint32_t mmsi, double lat, double lon,
                             double cog_deg, double sog_kn) {
    // Filter own vessel by MMSI
    extern String saved_own_mmsi;
    if (saved_own_mmsi.length() > 0 && (uint32_t)saved_own_mmsi.toInt() == mmsi) return;

    if (!g_ais_mutex) g_ais_mutex = xSemaphoreCreateMutex();
    if (!xSemaphoreTake(g_ais_mutex, pdMS_TO_TICKS(50))) return;
    int idx = ais_find_or_alloc(mmsi);
    if (idx >= 0) {
        g_ais_targets[idx].lat = (float)lat;
        g_ais_targets[idx].lon = (float)lon;
        g_ais_targets[idx].cog = (float)cog_deg;
        g_ais_targets[idx].sog = (float)sog_kn;
    }
    xSemaphoreGive(g_ais_mutex);
}

void ais_n2k_update_name(uint32_t mmsi, const char* name) {
    if (!name || !name[0]) return;
    // Filter own vessel by MMSI
    extern String saved_own_mmsi;
    if (saved_own_mmsi.length() > 0 && (uint32_t)saved_own_mmsi.toInt() == mmsi) return;

    if (!g_ais_mutex) g_ais_mutex = xSemaphoreCreateMutex();
    if (!xSemaphoreTake(g_ais_mutex, pdMS_TO_TICKS(50))) return;
    int idx = ais_find_or_alloc(mmsi);
    if (idx >= 0) {
        strncpy(g_ais_targets[idx].name, name, 20);
        g_ais_targets[idx].name[20] = '\0';
    }
    xSemaphoreGive(g_ais_mutex);
}
