/*
 * Screen rendering engine for RLCD Marine Display.
 *
 * Creates LVGL widgets for each configured display type
 * (Number, Dual, Quad, Graph, Compass, Position) on the
 * monochrome 400×300 reflective LCD.
 *
 * Value labels are stored so update_screen_values() can refresh
 * them periodically with live data from the SignalK WebSocket.
 */

#include "screen_render.h"
#include "screen_config.h"
#include "signalk_config.h"
#include "unit_convert.h"
#include "LVGL_Driver.h"
#include <lvgl.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <math.h>

#include "audio_alert.h"
#include <esp_heap_caps.h>

static const char *TAG = "screen_render";

static int current_screen = 0;

// ─── Stored label references for live value updates ─────────────────────────
// These get set during render_*() and read during update_screen_values().
// Only valid for the currently rendered screen.

// Each data slot has three labels: description (top), value (center), unit (bottom-right)
struct DataSlot {
    lv_obj_t* container; // bounding container (for zone flash; NULL for unconstrained)
    lv_obj_t* desc;
    lv_obj_t* value;
    lv_obj_t* unit;
};

static DataSlot slot_number = {};    // Number: single value

static DataSlot slot_dual_top = {};  // Dual: top value
static DataSlot slot_dual_bot = {};  // Dual: bottom value

static DataSlot slot_quad[4] = {};   // Quad: TL, TR, BL, BR

static lv_obj_t* val_compass_hdg = NULL; // Compass: heading
static lv_obj_t* val_compass_card = NULL; // Compass: cardinal direction
static DataSlot slot_compass_bl = {};    // Compass: bottom-left extra
static DataSlot slot_compass_br = {};    // Compass: bottom-right extra

// Linear tape compass geometry (400×300 monochrome)
// Horizontal tape strip across top; ticks and labels move left/right.
#define C_TAPE_Y         10    // top of tape band
#define C_TAPE_H         50    // tape band height
#define C_TAPE_BOT       (C_TAPE_Y + C_TAPE_H)  // 60
#define C_TICK_MAJ_H     30    // major tick (10°) height
#define C_TICK_MIN_H     15    // minor tick (5°) height
#define C_PPD             4    // pixels per degree
#define C_TICK_HALF      50    // ±50° visible
#define C_TICK_STEP       5
#define C_N_TICKS        21    // (50*2/5)+1
#define C_LBL_HALF       45
#define C_LBL_STEP       10
#define C_N_LABELS       10    // (45*2/10)+1 + safety

static lv_obj_t*   c_tick_lines[C_N_TICKS]  = {};
static lv_point_t  c_tick_pts[C_N_TICKS][2] = {};
static lv_obj_t*   c_deg_lbl[C_N_LABELS]    = {};
static lv_obj_t*   c_ptr_lines[2]           = {};
static lv_point_t  c_ptr_pts[2][2]          = {};

static lv_obj_t* val_pos_lat    = NULL;  // Position: latitude
static lv_obj_t* val_pos_lon    = NULL;  // Position: longitude
static lv_obj_t* val_pos_time   = NULL;  // Position: UTC time
static lv_obj_t* val_pos_date   = NULL;  // Position: date

static lv_obj_t* graph_chart    = NULL;  // Graph: chart object
static lv_chart_series_t* graph_ser1 = NULL;
static lv_chart_series_t* graph_ser2 = NULL;
static DataSlot slot_graph = {};         // Graph: current value + desc + unit
static lv_obj_t* graph_y_min_lbl = NULL; // Graph: Y-axis min label
static lv_obj_t* graph_y_max_lbl = NULL; // Graph: Y-axis max label

// ── PSRAM-backed persistent graph data ──────────────────────────────
#define GRAPH_POINTS 60

typedef struct {
    int32_t  series1[GRAPH_POINTS];
    uint16_t write_index;
    uint16_t count;
    unsigned long last_sample_time;
} GraphDataBuffer;

static GraphDataBuffer* graph_buffers[NUM_SCREENS] = {};

static const unsigned long graph_sample_intervals[] = {
    500,    // 30s
    1000,   // 1m
    5000,   // 5m
    10000,  // 10m
    30000   // 30m
};

static void graph_ensure_buffer(int screen) {
    if (screen < 0 || screen >= NUM_SCREENS) return;
    if (graph_buffers[screen]) return;
    graph_buffers[screen] = (GraphDataBuffer*)heap_caps_calloc(
        1, sizeof(GraphDataBuffer), MALLOC_CAP_SPIRAM);
    if (graph_buffers[screen])
        ESP_LOGI(TAG, "PSRAM graph buffer allocated for screen %d", screen);
}

static void graph_buffer_push(int screen, int32_t value) {
    GraphDataBuffer* buf = graph_buffers[screen];
    if (!buf) return;
    buf->series1[buf->write_index] = value;
    buf->write_index = (buf->write_index + 1) % GRAPH_POINTS;
    if (buf->count < GRAPH_POINTS) buf->count++;
}

// Gauge
static lv_obj_t* gauge_meter     = NULL;  // Gauge: lv_meter (radial)
static lv_meter_indicator_t* gauge_needle = NULL;
static lv_obj_t* gauge_bar       = NULL;  // Gauge: lv_bar (bar style)
static lv_obj_t* gauge_val_lbl   = NULL;  // Gauge: value label
static lv_obj_t* gauge_unit_lbl  = NULL;  // Gauge: unit label
static lv_obj_t* gauge_desc_lbl  = NULL;  // Gauge: description label
static lv_obj_t* gauge_min_lbl   = NULL;  // Gauge: min range label
static lv_obj_t* gauge_max_lbl   = NULL;  // Gauge: max range label

// Remote alert icon (flashing warning triangle, top-right)
static lv_obj_t* remote_alert_icon = NULL;
static void create_alert_icon(lv_obj_t* scr);

static void clear_label_refs() {
    slot_number = {};
    slot_dual_top = slot_dual_bot = {};
    for (int i = 0; i < 4; i++) slot_quad[i] = {};
    val_compass_hdg = NULL;
    val_compass_card = NULL;
    slot_compass_bl = slot_compass_br = {};
    for (int i = 0; i < C_N_TICKS; i++) c_tick_lines[i] = NULL;
    for (int i = 0; i < C_N_LABELS; i++) c_deg_lbl[i] = NULL;
    c_ptr_lines[0] = c_ptr_lines[1] = NULL;
    val_pos_lat = val_pos_lon = val_pos_time = val_pos_date = NULL;
    graph_chart = NULL;
    graph_ser1 = graph_ser2 = NULL;
    slot_graph = {};
    graph_y_min_lbl = graph_y_max_lbl = NULL;
    gauge_meter = NULL;
    gauge_needle = NULL;
    gauge_bar = NULL;
    gauge_val_lbl = gauge_unit_lbl = gauge_desc_lbl = NULL;
    gauge_min_lbl = gauge_max_lbl = NULL;
    remote_alert_icon = NULL;
}

// ─── Font lookup ────────────────────────────────────────────────────────────

LV_FONT_DECLARE(inter_16);
LV_FONT_DECLARE(inter_24);
LV_FONT_DECLARE(inter_48);
LV_FONT_DECLARE(inter_72);
LV_FONT_DECLARE(inter_96);
LV_FONT_DECLARE(inter_120);
LV_FONT_DECLARE(inter_144);

static const lv_font_t* get_font(uint8_t size) {
    switch (size) {
        case RLCD_FONT_48:  return &inter_48;
        case RLCD_FONT_72:  return &inter_72;
        case RLCD_FONT_96:  return &inter_96;
        case RLCD_FONT_120: return &inter_120;
        case RLCD_FONT_144: return &inter_144;
        default:            return &inter_48;
    }
}

// Unit font scales with value font (matching Square Display approach)
static const lv_font_t* get_unit_font(uint8_t size) {
    switch (size) {
        case RLCD_FONT_48:  return &inter_16;
        case RLCD_FONT_72:  return &inter_24;
        case RLCD_FONT_96:  return &inter_24;
        case RLCD_FONT_120: return &inter_48;
        case RLCD_FONT_144: return &inter_48;
        default:            return &inter_16;
    }
}

// Description font scales down from value font
static const lv_font_t* get_desc_font(uint8_t size) {
    switch (size) {
        case RLCD_FONT_48:  return &inter_24;
        case RLCD_FONT_72:  return &inter_24;
        case RLCD_FONT_96:  return &inter_24;
        case RLCD_FONT_120: return &inter_48;
        case RLCD_FONT_144: return &inter_48;
        default:            return &inter_16;
    }
}

// Helper: create a DataSlot within a bounded container (for dual/quad halves)
// Container occupies a fixed region; labels are pinned to fixed positions within it.
static DataSlot make_bounded_slot(lv_obj_t* parent, const char* label_text,
                                   uint8_t font_size,
                                   int cx, int cy, int cw, int ch) {
    DataSlot slot = {};
    const lv_font_t* vfont = get_font(font_size);
    const lv_font_t* dfont = get_desc_font(font_size);
    const lv_font_t* ufont = get_unit_font(font_size);

    // Invisible container
    lv_obj_t* cont = lv_obj_create(parent);
    slot.container = cont;
    lv_obj_set_size(cont, cw, ch);
    lv_obj_set_pos(cont, cx, cy);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // Description label — pinned to top-center
    slot.desc = lv_label_create(cont);
    lv_label_set_text(slot.desc, (label_text && label_text[0]) ? label_text : "");
    lv_obj_set_style_text_color(slot.desc, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.desc, dfont, 0);
    lv_obj_set_style_text_align(slot.desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(slot.desc, LV_ALIGN_TOP_MID, 0, 2);

    // Value label — centered in container
    slot.value = lv_label_create(cont);
    lv_label_set_text(slot.value, "---");
    lv_obj_set_style_text_color(slot.value, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.value, vfont, 0);
    lv_obj_set_style_text_align(slot.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(slot.value, LV_ALIGN_CENTER, 0, 0);

    // Unit label — pinned to bottom-right
    slot.unit = lv_label_create(cont);
    lv_label_set_text(slot.unit, "");
    lv_obj_set_style_text_color(slot.unit, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.unit, ufont, 0);
    lv_obj_set_style_text_align(slot.unit, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(slot.unit, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    return slot;
}

// ─── Helper: create description + value + unit labels ───────────────────────

static DataSlot make_data_slot(lv_obj_t* parent, const char* label_text,
                                uint8_t font_size, lv_align_t align,
                                int x_ofs, int y_ofs) {
    DataSlot slot = {};
    const lv_font_t* vfont = get_font(font_size);
    const lv_font_t* dfont = get_desc_font(font_size);
    const lv_font_t* ufont = get_unit_font(font_size);

    // Description label (above value — static label text or auto from metadata)
    slot.desc = lv_label_create(parent);
    lv_label_set_text(slot.desc, (label_text && label_text[0]) ? label_text : "");
    lv_obj_set_style_text_color(slot.desc, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.desc, dfont, 0);
    lv_obj_set_style_text_align(slot.desc, LV_TEXT_ALIGN_CENTER, 0);
    int desc_offset = (int)(vfont->line_height / 2) + (dfont->line_height / 2) + 2;
    lv_obj_align(slot.desc, align, x_ofs, y_ofs - desc_offset);

    // Value label (centered)
    slot.value = lv_label_create(parent);
    lv_label_set_text(slot.value, "---");
    lv_obj_set_style_text_color(slot.value, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.value, vfont, 0);
    lv_obj_set_style_text_align(slot.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(slot.value, align, x_ofs, y_ofs);

    // Unit label (below value)
    slot.unit = lv_label_create(parent);
    lv_label_set_text(slot.unit, "");
    lv_obj_set_style_text_color(slot.unit, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot.unit, ufont, 0);
    lv_obj_set_style_text_align(slot.unit, LV_TEXT_ALIGN_CENTER, 0);
    int unit_offset = (int)(vfont->line_height / 2) + 4;
    lv_obj_align(slot.unit, align, x_ofs, y_ofs + unit_offset);

    return slot;
}

// ─── Helper: format a float for display ─────────────────────────────────────

static void format_value(char* buf, size_t len, float val) {
    if (isnan(val)) {
        snprintf(buf, len, "---");
    } else if (fabsf(val) >= 1000.0f) {
        snprintf(buf, len, "%.0f", val);
    } else if (fabsf(val) >= 100.0f) {
        snprintf(buf, len, "%.1f", val);
    } else {
        snprintf(buf, len, "%.2f", val);
    }
}

// Convert SI value and format as number only (no unit appended)
static void format_converted_value(char* buf, size_t len, float si_value,
                                    const char* path) {
    if (isnan(si_value)) {
        snprintf(buf, len, "---");
        return;
    }
    String si_unit = get_sensor_unit_by_path(path);
    String display_unit;
    float converted = convert_unit(si_value, si_unit, String(path), display_unit);
    format_value(buf, len, converted);
}

// Get the display unit string for a path (e.g. "kn", "°C")
static String get_display_unit(const char* path) {
    String si_unit = get_sensor_unit_by_path(path);
    String display_unit;
    convert_unit(0.0f, si_unit, String(path), display_unit);
    return display_unit;
}

// Get description: use label override if set, else SignalK metadata
static String get_slot_description(const char* label_override, const char* path) {
    if (label_override && label_override[0]) return String(label_override);
    return get_sensor_description_by_path(path);
}

// Update a DataSlot with live values
static void update_data_slot(DataSlot& slot, const char* path,
                              const char* label_override) {
    char buf[64];
    if (slot.value && path && path[0]) {
        float v = get_sensor_value_by_path(path);
        format_converted_value(buf, sizeof(buf), v, path);
        lv_label_set_text(slot.value, buf);

        if (slot.unit) {
            String unit = get_display_unit(path);
            lv_label_set_text(slot.unit, unit.c_str());
        }
        if (slot.desc) {
            String desc = get_slot_description(label_override, path);
            lv_label_set_text(slot.desc, desc.c_str());
        }
    }
}

// ─── Render: Number ─────────────────────────────────────────────────────────

static void render_number(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    slot_number = make_data_slot(scr, cfg.number_label, cfg.number_font_size,
                                 LV_ALIGN_CENTER, 0, 0);
}

// ─── Render: Dual ───────────────────────────────────────────────────────────

static void render_dual(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    // Divider line
    static lv_point_t line_pts[] = {{0, 150}, {400, 150}};
    lv_obj_t* line = lv_line_create(scr);
    lv_line_set_points(line, line_pts, 2);
    lv_obj_set_style_line_color(line, lv_color_black(), 0);
    lv_obj_set_style_line_width(line, 2, 0);

    // Top half: 400×148, starts at y=0
    slot_dual_top = make_bounded_slot(scr, cfg.dual_top_label,
                     cfg.dual_top_font_size, 0, 0, 400, 148);
    // Bottom half: 400×148, starts at y=152
    slot_dual_bot = make_bounded_slot(scr, cfg.dual_bottom_label,
                     cfg.dual_bottom_font_size, 0, 152, 400, 148);
}

// ─── Render: Quad ───────────────────────────────────────────────────────────

static void render_quad(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    // Cross dividers
    static lv_point_t h_pts[] = {{0, 150}, {400, 150}};
    static lv_point_t v_pts[] = {{200, 0}, {200, 300}};
    lv_obj_t* hl = lv_line_create(scr);
    lv_line_set_points(hl, h_pts, 2);
    lv_obj_set_style_line_color(hl, lv_color_black(), 0);
    lv_obj_set_style_line_width(hl, 2, 0);
    lv_obj_t* vl = lv_line_create(scr);
    lv_line_set_points(vl, v_pts, 2);
    lv_obj_set_style_line_color(vl, lv_color_black(), 0);
    lv_obj_set_style_line_width(vl, 2, 0);

    // TL, TR, BL, BR — use bounded slots for per-zone flash support
    const struct { const char* lbl; uint8_t fs; int cx; int cy; int cw; int ch; } quads[] = {
        {cfg.quad_tl_label, cfg.quad_tl_font_size,   0,   0, 198, 148},
        {cfg.quad_tr_label, cfg.quad_tr_font_size, 202,   0, 198, 148},
        {cfg.quad_bl_label, cfg.quad_bl_font_size,   0, 152, 198, 148},
        {cfg.quad_br_label, cfg.quad_br_font_size, 202, 152, 198, 148},
    };
    for (int i = 0; i < 4; i++) {
        slot_quad[i] = make_bounded_slot(scr, quads[i].lbl, quads[i].fs,
                         quads[i].cx, quads[i].cy, quads[i].cw, quads[i].ch);
    }
}

// ─── Render: Graph ──────────────────────────────────────────────────────────

static void render_graph(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    // Title from description: config label override or metadata
    lv_obj_t* title = lv_label_create(scr);
    String desc = get_slot_description(NULL, cfg.graph_path_1);
    if (desc.length() == 0) {
        const char* last_dot = strrchr(cfg.graph_path_1, '.');
        desc = last_dot ? last_dot + 1 : cfg.graph_path_1;
    }
    lv_label_set_text(title, desc.c_str());
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 5, 4);

    // Current value + unit in top-right
    slot_graph.value = lv_label_create(scr);
    lv_label_set_text(slot_graph.value, "---");
    lv_obj_set_style_text_color(slot_graph.value, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot_graph.value, &inter_48, 0);
    lv_obj_align(slot_graph.value, LV_ALIGN_TOP_RIGHT, -40, -6);

    slot_graph.unit = lv_label_create(scr);
    lv_label_set_text(slot_graph.unit, "");
    lv_obj_set_style_text_color(slot_graph.unit, lv_color_black(), 0);
    lv_obj_set_style_text_font(slot_graph.unit, &lv_font_montserrat_14, 0);
    lv_obj_align(slot_graph.unit, LV_ALIGN_TOP_RIGHT, -5, 4);

    // Chart (shifted right to leave room for Y-axis labels)
    graph_chart = lv_chart_create(scr);
    lv_obj_set_size(graph_chart, 355, 240);
    lv_obj_align(graph_chart, LV_ALIGN_BOTTOM_RIGHT, -4, -8);
    lv_chart_set_type(graph_chart, cfg.graph_chart_type == RLCD_GRAPH_BAR ?
                      LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(graph_chart, GRAPH_POINTS);
    lv_obj_set_style_bg_color(graph_chart, lv_color_white(), 0);
    lv_obj_set_style_border_color(graph_chart, lv_color_black(), 0);
    lv_obj_set_style_line_color(graph_chart, lv_color_make(0x80, 0x80, 0x80), LV_PART_MAIN);

    // Start with default range (will auto-adjust)
    lv_chart_set_range(graph_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);

    graph_ser1 = lv_chart_add_series(graph_chart, lv_color_black(),
                                      LV_CHART_AXIS_PRIMARY_Y);

    // Restore data from PSRAM buffer if available
    graph_ensure_buffer(current_screen);
    GraphDataBuffer* gbuf = graph_buffers[current_screen];
    int32_t y_lo = 0, y_hi = 100;
    if (gbuf && gbuf->count > 0) {
        int np = (gbuf->count < GRAPH_POINTS) ? gbuf->count : GRAPH_POINTS;
        int start = (gbuf->count < GRAPH_POINTS) ? 0 : gbuf->write_index;
        for (int i = 0; i < GRAPH_POINTS - np; i++)
            lv_chart_set_next_value(graph_chart, graph_ser1, LV_CHART_POINT_NONE);
        y_lo = gbuf->series1[start % GRAPH_POINTS];
        y_hi = y_lo;
        for (int i = 0; i < np; i++) {
            int idx = (start + i) % GRAPH_POINTS;
            int32_t v = gbuf->series1[idx];
            lv_chart_set_next_value(graph_chart, graph_ser1, (lv_coord_t)v);
            if (v < y_lo) y_lo = v;
            if (v > y_hi) y_hi = v;
        }
        float rng = (float)(y_hi - y_lo);
        float margin = rng * 0.1f;
        if (margin < 1.0f) margin = 1.0f;
        y_lo = (int32_t)((float)y_lo - margin);
        y_hi = (int32_t)((float)y_hi + margin);
        lv_chart_set_range(graph_chart, LV_CHART_AXIS_PRIMARY_Y, y_lo, y_hi);
    } else {
        for (int i = 0; i < GRAPH_POINTS; i++)
            lv_chart_set_next_value(graph_chart, graph_ser1, LV_CHART_POINT_NONE);
    }

    // Y-axis range labels (positioned at chart top/bottom edges)
    char lb[16];
    graph_y_min_lbl = lv_label_create(scr);
    snprintf(lb, sizeof(lb), "%d", (int)y_lo);
    lv_label_set_text(graph_y_min_lbl, lb);
    lv_obj_set_style_text_color(graph_y_min_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(graph_y_min_lbl, &inter_16, 0);
    lv_obj_align(graph_y_min_lbl, LV_ALIGN_BOTTOM_LEFT, 4, -10);

    graph_y_max_lbl = lv_label_create(scr);
    snprintf(lb, sizeof(lb), "%d", (int)y_hi);
    lv_label_set_text(graph_y_max_lbl, lb);
    lv_obj_set_style_text_color(graph_y_max_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(graph_y_max_lbl, &inter_16, 0);
    lv_obj_align(graph_y_max_lbl, LV_ALIGN_TOP_LEFT, 4, 50);
}

// ─── Compass helpers ────────────────────────────────────────────────────────

static float norm360(float a) {
    while (a <    0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return a;
}

static const char* cardinal_for(float deg) {
    static const char* cards[] = {"N","NE","E","SE","S","SW","W","NW"};
    int i = (int)((norm360(deg) + 22.5f) / 45.0f) % 8;
    return cards[i];
}

static void update_compass_tape(float heading_deg) {
    heading_deg = norm360(heading_deg);

    // Reposition tick marks
    float tick_start = floorf((heading_deg - C_TICK_HALF) / C_TICK_STEP) * C_TICK_STEP;
    for (int i = 0; i < C_N_TICKS; i++) {
        float bearing = norm360(tick_start + i * C_TICK_STEP);
        float offset  = bearing - heading_deg;
        if (offset >  180.0f) offset -= 360.0f;
        if (offset < -180.0f) offset += 360.0f;

        int cx = 200 + (int)(offset * C_PPD);
        bool major = (fmodf(fabsf(bearing) + 0.5f, 10.0f) < 1.0f);
        int tick_h = major ? C_TICK_MAJ_H : C_TICK_MIN_H;

        c_tick_pts[i][0] = {(lv_coord_t)cx, (lv_coord_t)C_TAPE_BOT};
        c_tick_pts[i][1] = {(lv_coord_t)cx, (lv_coord_t)(C_TAPE_BOT - tick_h)};
        if (c_tick_lines[i]) {
            lv_line_set_points(c_tick_lines[i], c_tick_pts[i], 2);
            lv_obj_invalidate(c_tick_lines[i]);
        }
    }

    // Reposition degree/cardinal labels
    float lbl_start = floorf((heading_deg - C_LBL_HALF) / C_LBL_STEP) * C_LBL_STEP;
    for (int i = 0; i < C_N_LABELS; i++) {
        if (!c_deg_lbl[i]) continue;
        float bearing = norm360(lbl_start + i * C_LBL_STEP);
        float offset  = bearing - heading_deg;
        if (offset >  180.0f) offset -= 360.0f;
        if (offset < -180.0f) offset += 360.0f;

        if (fabsf(offset) > (float)C_TICK_HALF) {
            lv_label_set_text(c_deg_lbl[i], "");
            continue;
        }

        int cx = 200 + (int)(offset * C_PPD);
        bool is_card = (fmodf(bearing + 0.5f, 45.0f) < 1.0f);
        char lbuf[8];
        if (is_card) {
            lv_label_set_text(c_deg_lbl[i], cardinal_for(bearing));
            lv_obj_set_style_text_font(c_deg_lbl[i], &inter_24, 0);
        } else {
            snprintf(lbuf, sizeof(lbuf), "%.0f", bearing);
            lv_label_set_text(c_deg_lbl[i], lbuf);
            lv_obj_set_style_text_font(c_deg_lbl[i], &inter_16, 0);
        }
        lv_obj_update_layout(c_deg_lbl[i]);
        int w = lv_obj_get_width(c_deg_lbl[i]);
        int h = lv_obj_get_height(c_deg_lbl[i]);
        lv_obj_set_pos(c_deg_lbl[i], cx - w / 2, C_TAPE_BOT - C_TICK_MAJ_H - h - 2);
    }
}

// ─── Render: Compass ────────────────────────────────────────────────────────

static void render_compass(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    // Source label (top-left)
    bool isMag = (String(cfg.compass_path).length() == 0 ||
                  String(cfg.compass_path) == "navigation.headingMagnetic");
    lv_obj_t* src = lv_label_create(scr);
    lv_label_set_text(src, isMag ? "HDG \xC2\xB0M" : "HDG \xC2\xB0T");
    lv_obj_set_style_text_color(src, lv_color_black(), 0);
    lv_obj_set_style_text_font(src, &inter_16, 0);
    lv_obj_align(src, LV_ALIGN_TOP_LEFT, 4, C_TAPE_BOT + 2);

    // Tape baseline (horizontal line)
    static lv_point_t tape_line_pts[] = {{0, C_TAPE_BOT}, {400, C_TAPE_BOT}};
    lv_obj_t* tape_line = lv_line_create(scr);
    lv_line_set_points(tape_line, tape_line_pts, 2);
    lv_obj_set_style_line_color(tape_line, lv_color_black(), 0);
    lv_obj_set_style_line_width(tape_line, 2, 0);

    // Tick lines (positioned by update_compass_tape)
    for (int i = 0; i < C_N_TICKS; i++) {
        c_tick_pts[i][0] = {200, C_TAPE_BOT};
        c_tick_pts[i][1] = {200, C_TAPE_BOT};
        c_tick_lines[i] = lv_line_create(scr);
        lv_line_set_points(c_tick_lines[i], c_tick_pts[i], 2);
        lv_obj_set_style_line_width(c_tick_lines[i], 2, 0);
        lv_obj_set_style_line_color(c_tick_lines[i], lv_color_black(), 0);
    }

    // Degree/cardinal labels on tape (positioned by update_compass_tape)
    for (int i = 0; i < C_N_LABELS; i++) {
        c_deg_lbl[i] = lv_label_create(scr);
        lv_label_set_text(c_deg_lbl[i], "");
        lv_obj_set_style_text_font(c_deg_lbl[i], &inter_16, 0);
        lv_obj_set_style_text_color(c_deg_lbl[i], lv_color_black(), 0);
    }

    // Fixed pointer triangle at top-center (pointing down to tape)
    c_ptr_pts[0][0] = {200, (lv_coord_t)(C_TAPE_BOT + 2)};
    c_ptr_pts[0][1] = {192, (lv_coord_t)(C_TAPE_BOT + 14)};
    c_ptr_pts[1][0] = {200, (lv_coord_t)(C_TAPE_BOT + 2)};
    c_ptr_pts[1][1] = {208, (lv_coord_t)(C_TAPE_BOT + 14)};
    c_ptr_lines[0] = lv_line_create(scr);
    lv_line_set_points(c_ptr_lines[0], c_ptr_pts[0], 2);
    lv_obj_set_style_line_width(c_ptr_lines[0], 2, 0);
    lv_obj_set_style_line_color(c_ptr_lines[0], lv_color_black(), 0);
    c_ptr_lines[1] = lv_line_create(scr);
    lv_line_set_points(c_ptr_lines[1], c_ptr_pts[1], 2);
    lv_obj_set_style_line_width(c_ptr_lines[1], 2, 0);
    lv_obj_set_style_line_color(c_ptr_lines[1], lv_color_black(), 0);

    // Large heading number (below tape)
    val_compass_hdg = lv_label_create(scr);
    lv_label_set_text(val_compass_hdg, "---\xC2\xB0");
    lv_obj_set_style_text_color(val_compass_hdg, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_compass_hdg, &inter_72, 0);
    lv_obj_set_style_text_align(val_compass_hdg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val_compass_hdg, LV_ALIGN_CENTER, 0, -20);

    // Cardinal direction (top-right)
    val_compass_card = lv_label_create(scr);
    lv_label_set_text(val_compass_card, "---");
    lv_obj_set_style_text_color(val_compass_card, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_compass_card, &inter_16, 0);
    lv_obj_align(val_compass_card, LV_ALIGN_TOP_RIGHT, -4, C_TAPE_BOT + 2);

    // Bottom-left extra field
    if (cfg.compass_bl_path[0]) {
        slot_compass_bl.desc = lv_label_create(scr);
        String desc = get_slot_description(cfg.compass_bl_label, cfg.compass_bl_path);
        lv_label_set_text(slot_compass_bl.desc, desc.c_str());
        lv_obj_set_style_text_color(slot_compass_bl.desc, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_bl.desc, &inter_16, 0);
        lv_obj_set_width(slot_compass_bl.desc, 180);
        lv_label_set_long_mode(slot_compass_bl.desc, LV_LABEL_LONG_WRAP);
        lv_obj_align(slot_compass_bl.desc, LV_ALIGN_BOTTOM_LEFT, 8, -78);

        slot_compass_bl.value = lv_label_create(scr);
        lv_label_set_text(slot_compass_bl.value, "---");
        lv_obj_set_style_text_color(slot_compass_bl.value, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_bl.value, &inter_48, 0);
        lv_obj_align(slot_compass_bl.value, LV_ALIGN_BOTTOM_LEFT, 8, -5);

        slot_compass_bl.unit = lv_label_create(scr);
        String blu = get_display_unit(cfg.compass_bl_path);
        lv_label_set_text(slot_compass_bl.unit, blu.c_str());
        lv_obj_set_style_text_color(slot_compass_bl.unit, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_bl.unit, &inter_16, 0);
        lv_obj_align(slot_compass_bl.unit, LV_ALIGN_BOTTOM_LEFT, 8, -62);
    }
    // Bottom-right extra field
    if (cfg.compass_br_path[0]) {
        slot_compass_br.desc = lv_label_create(scr);
        String desc = get_slot_description(cfg.compass_br_label, cfg.compass_br_path);
        lv_label_set_text(slot_compass_br.desc, desc.c_str());
        lv_obj_set_style_text_color(slot_compass_br.desc, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_br.desc, &inter_16, 0);
        lv_obj_set_width(slot_compass_br.desc, 180);
        lv_label_set_long_mode(slot_compass_br.desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(slot_compass_br.desc, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(slot_compass_br.desc, LV_ALIGN_BOTTOM_RIGHT, -8, -78);

        slot_compass_br.value = lv_label_create(scr);
        lv_label_set_text(slot_compass_br.value, "---");
        lv_obj_set_style_text_color(slot_compass_br.value, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_br.value, &inter_48, 0);
        lv_obj_align(slot_compass_br.value, LV_ALIGN_BOTTOM_RIGHT, -8, -5);

        slot_compass_br.unit = lv_label_create(scr);
        String bru = get_display_unit(cfg.compass_br_path);
        lv_label_set_text(slot_compass_br.unit, bru.c_str());
        lv_obj_set_style_text_color(slot_compass_br.unit, lv_color_black(), 0);
        lv_obj_set_style_text_font(slot_compass_br.unit, &inter_16, 0);
        lv_obj_align(slot_compass_br.unit, LV_ALIGN_BOTTOM_RIGHT, -8, -62);
    }

    // Draw initial position (heading = 0)
    update_compass_tape(0.0f);
}

// ─── Render: Position ───────────────────────────────────────────────────────

static void render_position(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    // Title
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "POSITION & TIME");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title, &inter_16, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    // Divider 1
    static lv_point_t div1_pts[] = {{10, 22}, {390, 22}};
    lv_obj_t* d1 = lv_line_create(scr);
    lv_line_set_points(d1, div1_pts, 2);
    lv_obj_set_style_line_color(d1, lv_color_black(), 0);

    // LAT label
    lv_obj_t* lat_lbl = lv_label_create(scr);
    lv_label_set_text(lat_lbl, "LAT");
    lv_obj_set_style_text_color(lat_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(lat_lbl, &inter_16, 0);
    lv_obj_align(lat_lbl, LV_ALIGN_TOP_LEFT, 10, 26);

    // LAT value (inter_48)
    val_pos_lat = lv_label_create(scr);
    lv_label_set_text(val_pos_lat, "---");
    lv_obj_set_style_text_color(val_pos_lat, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_pos_lat, &inter_48, 0);
    lv_obj_set_style_text_align(val_pos_lat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val_pos_lat, LV_ALIGN_TOP_MID, 0, 44);

    // LON label
    lv_obj_t* lon_lbl = lv_label_create(scr);
    lv_label_set_text(lon_lbl, "LON");
    lv_obj_set_style_text_color(lon_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_font(lon_lbl, &inter_16, 0);
    lv_obj_align(lon_lbl, LV_ALIGN_TOP_LEFT, 10, 100);

    // LON value (inter_48)
    val_pos_lon = lv_label_create(scr);
    lv_label_set_text(val_pos_lon, "---");
    lv_obj_set_style_text_color(val_pos_lon, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_pos_lon, &inter_48, 0);
    lv_obj_set_style_text_align(val_pos_lon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val_pos_lon, LV_ALIGN_TOP_MID, 0, 118);

    // Divider 2
    static lv_point_t div2_pts[] = {{10, 178}, {390, 178}};
    lv_obj_t* d2 = lv_line_create(scr);
    lv_line_set_points(d2, div2_pts, 2);
    lv_obj_set_style_line_color(d2, lv_color_black(), 0);

    // Time value (inter_48)
    val_pos_time = lv_label_create(scr);
    lv_label_set_text(val_pos_time, "--:--:-- UTC");
    lv_obj_set_style_text_color(val_pos_time, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_pos_time, &inter_48, 0);
    lv_obj_set_style_text_align(val_pos_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val_pos_time, LV_ALIGN_TOP_MID, 0, 190);

    // Date (inter_16)
    val_pos_date = lv_label_create(scr);
    lv_label_set_text(val_pos_date, "----/--/--");
    lv_obj_set_style_text_color(val_pos_date, lv_color_black(), 0);
    lv_obj_set_style_text_font(val_pos_date, &inter_16, 0);
    lv_obj_set_style_text_align(val_pos_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val_pos_date, LV_ALIGN_TOP_MID, 0, 250);
}

// ─── Render: Unconfigured (splash) ──────────────────────────────────────────

static void render_splash(lv_obj_t* scr, int screen_num) {
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Marine Display");
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -50);

    lv_obj_t* sub = lv_label_create(scr);
    lv_label_set_text(sub, "Boating With The Baileys");
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_24, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 0);

    char buf[80];
    String ip = WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    snprintf(buf, sizeof(buf), "Screen %d - configure via %s", screen_num + 1, ip.c_str());
    lv_obj_t* hint = lv_label_create(scr);
    lv_label_set_text(hint, buf);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);
}

// ─── Position formatting helpers ────────────────────────────────────────────

static void format_lat(char* buf, size_t len, float lat, uint8_t fmt) {
    if (isnan(lat)) { snprintf(buf, len, "---"); return; }
    char ns = lat >= 0 ? 'N' : 'S';
    float a = fabsf(lat);
    if (fmt == RLCD_POS_DMS) {
        int d = (int)a;
        float mf = (a - d) * 60.0f;
        int m = (int)mf;
        float s = (mf - m) * 60.0f;
        snprintf(buf, len, "%d\xC2\xB0%02d'%05.2f\"%c", d, m, s, ns);
    } else if (fmt == RLCD_POS_DDM) {
        int d = (int)a;
        float m = (a - d) * 60.0f;
        snprintf(buf, len, "%d\xC2\xB0%06.3f'%c", d, m, ns);
    } else {
        snprintf(buf, len, "%.6f\xC2\xB0%c", a, ns);
    }
}

static void format_lon(char* buf, size_t len, float lon, uint8_t fmt) {
    if (isnan(lon)) { snprintf(buf, len, "---"); return; }
    char ew = lon >= 0 ? 'E' : 'W';
    float a = fabsf(lon);
    if (fmt == RLCD_POS_DMS) {
        int d = (int)a;
        float mf = (a - d) * 60.0f;
        int m = (int)mf;
        float s = (mf - m) * 60.0f;
        snprintf(buf, len, "%d\xC2\xB0%02d'%05.2f\"%c", d, m, s, ew);
    } else if (fmt == RLCD_POS_DDM) {
        int d = (int)a;
        float m = (a - d) * 60.0f;
        snprintf(buf, len, "%d\xC2\xB0%06.3f'%c", d, m, ew);
    } else {
        snprintf(buf, len, "%.6f\xC2\xB0%c", a, ew);
    }
}

// ─── Render: Gauge ──────────────────────────────────────────────────────────

static void render_gauge(lv_obj_t* scr, const RlcdScreenConfig& cfg) {
    int16_t g_min = cfg.gauge_min;
    int16_t g_max = cfg.gauge_max;
    if (g_max <= g_min) g_max = g_min + 100;

    // Description string
    String desc = get_slot_description(cfg.gauge_label, cfg.gauge_path);
    if (desc.length() == 0) {
        const char* dot = strrchr(cfg.gauge_path, '.');
        desc = dot ? dot + 1 : cfg.gauge_path;
    }

    if (cfg.gauge_style == RLCD_GAUGE_RADIAL) {
        // ── Radial gauge using lv_meter ──
        gauge_meter = lv_meter_create(scr);
        lv_obj_set_size(gauge_meter, 290, 290);
        lv_obj_align(gauge_meter, LV_ALIGN_CENTER, 0, 5);
        lv_obj_set_style_bg_color(gauge_meter, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(gauge_meter, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(gauge_meter, 2, 0);
        lv_obj_set_style_border_color(gauge_meter, lv_color_black(), 0);
        lv_obj_set_style_pad_all(gauge_meter, 10, 0);

        // Scale: 270° sweep, ticks
        lv_meter_scale_t* scale = lv_meter_add_scale(gauge_meter);
        int range = g_max - g_min;
        int major_nth = 5;
        int tick_count = 41;
        if (range <= 20) { tick_count = range + 1; major_nth = 5; }
        else if (range <= 100) { tick_count = range / 2 + 1; major_nth = 5; }
        else if (range <= 500) { tick_count = 51; major_nth = 5; }
        else { tick_count = 41; major_nth = 4; }

        lv_meter_set_scale_ticks(gauge_meter, scale, tick_count, 2, 10, lv_color_black());
        lv_meter_set_scale_major_ticks(gauge_meter, scale, major_nth, 3, 18, lv_color_black(), 12);
        lv_meter_set_scale_range(gauge_meter, scale, g_min, g_max, 270, 135);

        // Alert range arcs (hatched zones like RPM redline)
        if (cfg.gauge_alert_low != ALERT_OFF && cfg.gauge_alert_low > g_min && cfg.gauge_alert_low < g_max) {
            // Low alert zone: from g_min up to alert_low
            lv_meter_indicator_t* arc_lo = lv_meter_add_arc(gauge_meter, scale, 8, lv_color_black(), 0);
            lv_meter_set_indicator_start_value(gauge_meter, arc_lo, g_min);
            lv_meter_set_indicator_end_value(gauge_meter, arc_lo, cfg.gauge_alert_low);
        }
        if (cfg.gauge_alert_high != ALERT_OFF && cfg.gauge_alert_high > g_min && cfg.gauge_alert_high < g_max) {
            // High alert zone: from alert_high up to g_max
            lv_meter_indicator_t* arc_hi = lv_meter_add_arc(gauge_meter, scale, 8, lv_color_black(), 0);
            lv_meter_set_indicator_start_value(gauge_meter, arc_hi, cfg.gauge_alert_high);
            lv_meter_set_indicator_end_value(gauge_meter, arc_hi, g_max);
        }

        // Needle indicator
        gauge_needle = lv_meter_add_needle_line(gauge_meter, scale, 3, lv_color_black(), -10);
        lv_meter_set_indicator_value(gauge_meter, gauge_needle, g_min);

        // Description label — inside meter at top
        gauge_desc_lbl = lv_label_create(gauge_meter);
        lv_label_set_text(gauge_desc_lbl, desc.c_str());
        lv_obj_set_style_text_color(gauge_desc_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_desc_lbl, &inter_24, 0);
        lv_obj_align(gauge_desc_lbl, LV_ALIGN_TOP_MID, 0, 80);

        // Value + unit label — inside meter, below centre
        gauge_val_lbl = lv_label_create(gauge_meter);
        lv_label_set_text(gauge_val_lbl, "---");
        lv_obj_set_style_text_color(gauge_val_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_val_lbl, &inter_48, 0);
        lv_obj_align(gauge_val_lbl, LV_ALIGN_CENTER, 0, 100);

        // Unit label — centered below value
        gauge_unit_lbl = lv_label_create(gauge_meter);
        String unit = get_display_unit(cfg.gauge_path);
        lv_label_set_text(gauge_unit_lbl, unit.c_str());
        lv_obj_set_style_text_color(gauge_unit_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_unit_lbl, &inter_24, 0);
        lv_obj_align(gauge_unit_lbl, LV_ALIGN_CENTER, 0, 40);

    } else {
        // ── Bar gauge using lv_bar ──

        // Description label (top, larger font)
        gauge_desc_lbl = lv_label_create(scr);
        lv_label_set_text(gauge_desc_lbl, desc.c_str());
        lv_obj_set_style_text_color(gauge_desc_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_desc_lbl, &inter_24, 0);
        lv_obj_align(gauge_desc_lbl, LV_ALIGN_TOP_MID, 0, 4);

        gauge_bar = lv_bar_create(scr);
        lv_obj_set_size(gauge_bar, 360, 50);
        lv_obj_align(gauge_bar, LV_ALIGN_CENTER, 0, 30);
        lv_bar_set_range(gauge_bar, g_min, g_max);
        lv_bar_set_value(gauge_bar, g_min, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(gauge_bar, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(gauge_bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(gauge_bar, lv_color_black(), 0);
        lv_obj_set_style_border_width(gauge_bar, 2, 0);
        lv_obj_set_style_bg_color(gauge_bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(gauge_bar, LV_OPA_COVER, LV_PART_INDICATOR);

        // Min / Max labels under the bar
        gauge_min_lbl = lv_label_create(scr);
        char lb[16];
        snprintf(lb, sizeof(lb), "%d", g_min);
        lv_label_set_text(gauge_min_lbl, lb);
        lv_obj_set_style_text_color(gauge_min_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_min_lbl, &inter_16, 0);
        lv_obj_align(gauge_min_lbl, LV_ALIGN_CENTER, -175, 70);

        gauge_max_lbl = lv_label_create(scr);
        snprintf(lb, sizeof(lb), "%d", g_max);
        lv_label_set_text(gauge_max_lbl, lb);
        lv_obj_set_style_text_color(gauge_max_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_max_lbl, &inter_16, 0);
        lv_obj_align(gauge_max_lbl, LV_ALIGN_CENTER, 175, 70);

        // Alert range markers on the bar
        if (cfg.gauge_alert_low != ALERT_OFF && cfg.gauge_alert_low > g_min && cfg.gauge_alert_low < g_max) {
            // Draw a tick mark at the alert_low position
            lv_obj_t* mk = lv_label_create(scr);
            lv_label_set_text(mk, "|");
            lv_obj_set_style_text_color(mk, lv_color_black(), 0);
            lv_obj_set_style_text_font(mk, &inter_24, 0);
            int16_t xoff = -180 + (int32_t)(cfg.gauge_alert_low - g_min) * 360 / (g_max - g_min);
            lv_obj_align(mk, LV_ALIGN_CENTER, xoff, 55);
        }
        if (cfg.gauge_alert_high != ALERT_OFF && cfg.gauge_alert_high > g_min && cfg.gauge_alert_high < g_max) {
            lv_obj_t* mk = lv_label_create(scr);
            lv_label_set_text(mk, "|");
            lv_obj_set_style_text_color(mk, lv_color_black(), 0);
            lv_obj_set_style_text_font(mk, &inter_24, 0);
            int16_t xoff = -180 + (int32_t)(cfg.gauge_alert_high - g_min) * 360 / (g_max - g_min);
            lv_obj_align(mk, LV_ALIGN_CENTER, xoff, 55);
        }

        // Value label (large, above bar)
        gauge_val_lbl = lv_label_create(scr);
        lv_label_set_text(gauge_val_lbl, "---");
        lv_obj_set_style_text_color(gauge_val_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_val_lbl, &inter_96, 0);
        lv_obj_set_style_text_align(gauge_val_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(gauge_val_lbl, LV_ALIGN_CENTER, 0, -60);

        // Unit label — right of value
        gauge_unit_lbl = lv_label_create(scr);
        String unit = get_display_unit(cfg.gauge_path);
        lv_label_set_text(gauge_unit_lbl, unit.c_str());
        lv_obj_set_style_text_color(gauge_unit_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_font(gauge_unit_lbl, &inter_24, 0);
        lv_obj_align_to(gauge_unit_lbl, gauge_val_lbl, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, 0);
    }
}

// ─── Public API ─────────────────────────────────────────────────────────────

int get_current_screen() {
    return current_screen;
}

void set_current_screen(int screen) {
    if (screen < 0) screen = 0;
    if (screen >= NUM_SCREENS) screen = NUM_SCREENS - 1;
    current_screen = screen;
    render_screen(screen);
}

void render_screen(int screen) {
    if (!LVGL_Lock()) return;

    clear_label_refs();

    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const RlcdScreenConfig& cfg = screen_configs[screen];

    bool configured = false;
    switch (cfg.display_type) {
        case DISPLAY_TYPE_NUMBER:   configured = cfg.number_path[0] != 0; break;
        case DISPLAY_TYPE_DUAL:     configured = cfg.dual_top_path[0] != 0 || cfg.dual_bottom_path[0] != 0; break;
        case DISPLAY_TYPE_QUAD:     configured = cfg.quad_tl_path[0] != 0; break;
        case DISPLAY_TYPE_GRAPH:    configured = cfg.graph_path_1[0] != 0; break;
        case DISPLAY_TYPE_COMPASS:  configured = true; break;
        case DISPLAY_TYPE_POSITION: configured = true; break;
        case DISPLAY_TYPE_GAUGE:    configured = cfg.gauge_path[0] != 0; break;
        default: break;
    }

    if (!configured) {
        render_splash(scr, screen);
    } else {
        switch (cfg.display_type) {
            case DISPLAY_TYPE_NUMBER:   render_number(scr, cfg); break;
            case DISPLAY_TYPE_DUAL:     render_dual(scr, cfg); break;
            case DISPLAY_TYPE_QUAD:     render_quad(scr, cfg); break;
            case DISPLAY_TYPE_GRAPH:    render_graph(scr, cfg); break;
            case DISPLAY_TYPE_COMPASS:  render_compass(scr, cfg); break;
            case DISPLAY_TYPE_POSITION: render_position(scr, cfg); break;
            case DISPLAY_TYPE_GAUGE:    render_gauge(scr, cfg); break;
            default:                    render_splash(scr, screen); break;
        }
    }

    // Always create the remote alert icon overlay (hidden by default)
    create_alert_icon(scr);

    LVGL_Unlock();
    ESP_LOGI(TAG, "Rendered screen %d (type=%d)", screen, cfg.display_type);
}

// Create the remote alert icon overlay (called after screen content is rendered)
static void create_alert_icon(lv_obj_t* scr) {
    remote_alert_icon = lv_label_create(scr);
    lv_label_set_text(remote_alert_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(remote_alert_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(remote_alert_icon, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(remote_alert_icon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(remote_alert_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(remote_alert_icon, 3, 0);
    lv_obj_align(remote_alert_icon, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_add_flag(remote_alert_icon, LV_OBJ_FLAG_HIDDEN);
}

// ─── Alert check helper ─────────────────────────────────────────────────────
// Extract converted int32_t value from a SignalK path, return true if valid.
static bool get_converted_int(const char* path, int32_t &out) {
    if (!path || !path[0]) return false;
    float v = get_sensor_value_by_path(path);
    if (isnan(v)) return false;
    String si_unit = get_sensor_unit_by_path(path);
    String disp_unit;
    float cv = convert_unit(v, si_unit, String(path), disp_unit);
    out = (int32_t)cv;
    return true;
}

// Apply per-zone flash visual effect (invert zone bg/text when flashing)
static void apply_zone_flash() {
    const int zone_ids[] = {FLASH_ZONE_DUAL_TOP, FLASH_ZONE_DUAL_BOT,
                            FLASH_ZONE_QUAD_TL, FLASH_ZONE_QUAD_TR,
                            FLASH_ZONE_QUAD_BL, FLASH_ZONE_QUAD_BR};
    DataSlot* slots[] = {&slot_dual_top, &slot_dual_bot,
                         &slot_quad[0], &slot_quad[1],
                         &slot_quad[2], &slot_quad[3]};
    for (int i = 0; i < 6; i++) {
        if (!slots[i]->container) continue;
        bool inv = is_zone_flash_on(zone_ids[i]);
        lv_color_t fg = inv ? lv_color_white() : lv_color_black();
        lv_obj_set_style_bg_color(slots[i]->container, inv ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(slots[i]->container, inv ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (slots[i]->desc)  lv_obj_set_style_text_color(slots[i]->desc,  fg, 0);
        if (slots[i]->value) lv_obj_set_style_text_color(slots[i]->value, fg, 0);
        if (slots[i]->unit)  lv_obj_set_style_text_color(slots[i]->unit,  fg, 0);
    }
}

// Check alerts for one screen config (any display type that supports alerts).
// screen_idx identifies which screen config this is.
static void check_screen_alerts(int screen_idx) {
    const RlcdScreenConfig& cfg = screen_configs[screen_idx];
    int32_t iv;

    switch (cfg.display_type) {
        case DISPLAY_TYPE_NUMBER:
            if ((cfg.number_alert_low != ALERT_OFF || cfg.number_alert_high != ALERT_OFF) && get_converted_int(cfg.number_path, iv))
                check_alert(iv, cfg.number_alert_low, cfg.number_alert_high, cfg.number_alert_flash, cfg.number_alert_buzzer, screen_idx, FLASH_ZONE_FULL);
            break;

        case DISPLAY_TYPE_DUAL: {
            if ((cfg.dual_top_alert_low != ALERT_OFF || cfg.dual_top_alert_high != ALERT_OFF) && get_converted_int(cfg.dual_top_path, iv))
                check_alert(iv, cfg.dual_top_alert_low, cfg.dual_top_alert_high, cfg.dual_top_alert_flash, cfg.dual_top_alert_buzzer, screen_idx, FLASH_ZONE_DUAL_TOP);
            if ((cfg.dual_bot_alert_low != ALERT_OFF || cfg.dual_bot_alert_high != ALERT_OFF) && get_converted_int(cfg.dual_bottom_path, iv))
                check_alert(iv, cfg.dual_bot_alert_low, cfg.dual_bot_alert_high, cfg.dual_bot_alert_flash, cfg.dual_bot_alert_buzzer, screen_idx, FLASH_ZONE_DUAL_BOT);
            break;
        }

        case DISPLAY_TYPE_QUAD: {
            const char* qPaths[] = {cfg.quad_tl_path, cfg.quad_tr_path, cfg.quad_bl_path, cfg.quad_br_path};
            const int16_t qLow[] = {cfg.quad_tl_alert_low, cfg.quad_tr_alert_low, cfg.quad_bl_alert_low, cfg.quad_br_alert_low};
            const int16_t qHigh[] = {cfg.quad_tl_alert_high, cfg.quad_tr_alert_high, cfg.quad_bl_alert_high, cfg.quad_br_alert_high};
            const uint8_t qFlash[] = {cfg.quad_tl_alert_flash, cfg.quad_tr_alert_flash, cfg.quad_bl_alert_flash, cfg.quad_br_alert_flash};
            const uint8_t qBuzzer[] = {cfg.quad_tl_alert_buzzer, cfg.quad_tr_alert_buzzer, cfg.quad_bl_alert_buzzer, cfg.quad_br_alert_buzzer};
            const int qZones[] = {FLASH_ZONE_QUAD_TL, FLASH_ZONE_QUAD_TR, FLASH_ZONE_QUAD_BL, FLASH_ZONE_QUAD_BR};
            for (int q = 0; q < 4; q++) {
                if ((qLow[q] != ALERT_OFF || qHigh[q] != ALERT_OFF) && get_converted_int(qPaths[q], iv))
                    check_alert(iv, qLow[q], qHigh[q], qFlash[q], qBuzzer[q], screen_idx, qZones[q]);
            }
            break;
        }

        case DISPLAY_TYPE_GAUGE:
            if ((cfg.gauge_alert_low != ALERT_OFF || cfg.gauge_alert_high != ALERT_OFF) && get_converted_int(cfg.gauge_path, iv))
                check_alert(iv, cfg.gauge_alert_low, cfg.gauge_alert_high, cfg.gauge_alert_flash, cfg.gauge_alert_buzzer, screen_idx, FLASH_ZONE_FULL);
            break;

        default:
            break;
    }
}

// ─── Live value update ──────────────────────────────────────────────────────

void update_screen_values() {
    if (!LVGL_Lock()) return;

    const RlcdScreenConfig& cfg = screen_configs[current_screen];
    char buf[64];

    switch (cfg.display_type) {
        case DISPLAY_TYPE_NUMBER: {
            update_data_slot(slot_number, cfg.number_path, cfg.number_label);
            break;
        }
        case DISPLAY_TYPE_DUAL: {
            update_data_slot(slot_dual_top, cfg.dual_top_path, cfg.dual_top_label);
            update_data_slot(slot_dual_bot, cfg.dual_bottom_path, cfg.dual_bottom_label);
            break;
        }
        case DISPLAY_TYPE_QUAD: {
            const char* paths[] = {cfg.quad_tl_path, cfg.quad_tr_path,
                                   cfg.quad_bl_path, cfg.quad_br_path};
            const char* labels[] = {cfg.quad_tl_label, cfg.quad_tr_label,
                                    cfg.quad_bl_label, cfg.quad_br_label};
            for (int i = 0; i < 4; i++) {
                update_data_slot(slot_quad[i], paths[i], labels[i]);
            }
            break;
        }
        case DISPLAY_TYPE_GRAPH: {
            if (cfg.graph_path_1[0]) {
                graph_ensure_buffer(current_screen);
                GraphDataBuffer* gbuf = graph_buffers[current_screen];
                float v = get_sensor_value_by_path(cfg.graph_path_1);
                if (!isnan(v)) {
                    String si_unit = get_sensor_unit_by_path(cfg.graph_path_1);
                    String disp_unit;
                    float cv = convert_unit(v, si_unit, String(cfg.graph_path_1), disp_unit);

                    // Time-gated sampling into PSRAM buffer + LVGL chart
                    uint8_t tr = cfg.graph_time_range;
                    if (tr > 4) tr = 0;
                    unsigned long now = millis();
                    bool should_sample = !gbuf || gbuf->last_sample_time == 0 ||
                        (now - gbuf->last_sample_time) >= graph_sample_intervals[tr];

                    if (should_sample) {
                        if (gbuf) { gbuf->last_sample_time = now; }
                        graph_buffer_push(current_screen, (int32_t)cv);

                        if (graph_chart && graph_ser1) {
                            lv_chart_set_next_value(graph_chart, graph_ser1, (lv_coord_t)cv);

                            uint16_t pc = lv_chart_get_point_count(graph_chart);
                            lv_coord_t* ya = lv_chart_get_y_array(graph_chart, graph_ser1);
                            lv_coord_t ymin = (lv_coord_t)cv, ymax = (lv_coord_t)cv;
                            for (uint16_t i = 0; i < pc; i++) {
                                if (ya[i] == LV_CHART_POINT_NONE) continue;
                                if (ya[i] < ymin) ymin = ya[i];
                                if (ya[i] > ymax) ymax = ya[i];
                            }
                            float rng = (float)(ymax - ymin);
                            float margin = rng * 0.1f;
                            if (margin < 1.0f) margin = 1.0f;
                            int32_t adj_min = (int32_t)((float)ymin - margin);
                            int32_t adj_max = (int32_t)((float)ymax + margin);
                            lv_chart_set_range(graph_chart, LV_CHART_AXIS_PRIMARY_Y, adj_min, adj_max);

                            if (graph_y_min_lbl) {
                                char lb[16]; snprintf(lb, sizeof(lb), "%d", (int)adj_min);
                                lv_label_set_text(graph_y_min_lbl, lb);
                            }
                            if (graph_y_max_lbl) {
                                char lb[16]; snprintf(lb, sizeof(lb), "%d", (int)adj_max);
                                lv_label_set_text(graph_y_max_lbl, lb);
                            }
                            lv_chart_refresh(graph_chart);
                        }
                    }
                }
                // Always update current value + unit labels
                if (slot_graph.value) {
                    format_converted_value(buf, sizeof(buf), v, cfg.graph_path_1);
                    lv_label_set_text(slot_graph.value, buf);
                }
                if (slot_graph.unit) {
                    String unit = get_display_unit(cfg.graph_path_1);
                    lv_label_set_text(slot_graph.unit, unit.c_str());
                }
            }
            break;
        }
        case DISPLAY_TYPE_COMPASS: {
            float hdg_rad = get_sensor_value_by_path(
                cfg.compass_path[0] ? cfg.compass_path : "navigation.headingMagnetic");
            float deg = 0.0f;
            bool valid = !isnan(hdg_rad);
            if (valid) {
                deg = convert_angle_rad(hdg_rad);
                if (deg < 0) deg += 360.0f;
            }
            if (val_compass_hdg) {
                if (valid)
                    snprintf(buf, sizeof(buf), "%03.0f\xC2\xB0", deg);
                else
                    snprintf(buf, sizeof(buf), "---\xC2\xB0");
                lv_label_set_text(val_compass_hdg, buf);
            }
            if (val_compass_card && valid) {
                lv_label_set_text(val_compass_card, cardinal_for(deg));
            }
            if (valid) {
                update_compass_tape(deg);
            }
            if (slot_compass_bl.value && cfg.compass_bl_path[0]) {
                float v = get_sensor_value_by_path(cfg.compass_bl_path);
                format_converted_value(buf, sizeof(buf), v, cfg.compass_bl_path);
                lv_label_set_text(slot_compass_bl.value, buf);
            }
            if (slot_compass_br.value && cfg.compass_br_path[0]) {
                float v = get_sensor_value_by_path(cfg.compass_br_path);
                format_converted_value(buf, sizeof(buf), v, cfg.compass_br_path);
                lv_label_set_text(slot_compass_br.value, buf);
            }
            break;
        }
        case DISPLAY_TYPE_POSITION: {
            if (val_pos_lat) {
                format_lat(buf, sizeof(buf), g_nav_latitude, cfg.pos_format);
                lv_label_set_text(val_pos_lat, buf);
            }
            if (val_pos_lon) {
                format_lon(buf, sizeof(buf), g_nav_longitude, cfg.pos_format);
                lv_label_set_text(val_pos_lon, buf);
            }
            if (g_sk_datetime[0] && strlen(g_sk_datetime) >= 19) {
                if (val_pos_time) {
                    snprintf(buf, sizeof(buf), "%.8s UTC", g_sk_datetime + 11);
                    lv_label_set_text(val_pos_time, buf);
                }
                if (val_pos_date) {
                    snprintf(buf, sizeof(buf), "%.10s", g_sk_datetime);
                    lv_label_set_text(val_pos_date, buf);
                }
            }
            break;
        }
        case DISPLAY_TYPE_GAUGE: {
            if (cfg.gauge_path[0]) {
                float v = get_sensor_value_by_path(cfg.gauge_path);
                if (!isnan(v)) {
                    String si_unit = get_sensor_unit_by_path(cfg.gauge_path);
                    String disp_unit;
                    float cv = convert_unit(v, si_unit, String(cfg.gauge_path), disp_unit);
                    int32_t iv = (int32_t)cv;

                    if (cfg.gauge_style == RLCD_GAUGE_RADIAL && gauge_meter && gauge_needle) {
                        lv_meter_set_indicator_value(gauge_meter, gauge_needle, iv);
                    } else if (gauge_bar) {
                        lv_bar_set_value(gauge_bar, iv, LV_ANIM_OFF);
                    }
                }
                if (gauge_val_lbl) {
                    format_converted_value(buf, sizeof(buf), v, cfg.gauge_path);
                    lv_label_set_text(gauge_val_lbl, buf);
                }
                if (gauge_unit_lbl) {
                    String unit = get_display_unit(cfg.gauge_path);
                    lv_label_set_text(gauge_unit_lbl, unit.c_str());
                    // Re-align: bar uses right-of-value, radial uses centered below
                    if (gauge_val_lbl && cfg.gauge_style != RLCD_GAUGE_RADIAL) {
                        lv_obj_align_to(gauge_unit_lbl, gauge_val_lbl, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, 0);
                    }
                }
            }
            break;
        }
        default:
            break;
    }

    // ── Background graph data collection for non-visible graph screens ──
    for (int s = 0; s < NUM_SCREENS; s++) {
        if (s == current_screen) continue;
        const RlcdScreenConfig& gcfg = screen_configs[s];
        if (gcfg.display_type != DISPLAY_TYPE_GRAPH || !gcfg.graph_path_1[0]) continue;
        graph_ensure_buffer(s);
        GraphDataBuffer* gbuf = graph_buffers[s];
        if (!gbuf) continue;
        uint8_t tr = gcfg.graph_time_range;
        if (tr > 4) tr = 0;
        unsigned long now = millis();
        if (gbuf->last_sample_time != 0 &&
            (now - gbuf->last_sample_time) < graph_sample_intervals[tr]) continue;
        float gv = get_sensor_value_by_path(gcfg.graph_path_1);
        if (isnan(gv)) continue;
        String si = get_sensor_unit_by_path(gcfg.graph_path_1);
        String du;
        float gcv = convert_unit(gv, si, String(gcfg.graph_path_1), du);
        gbuf->last_sample_time = now;
        graph_buffer_push(s, (int32_t)gcv);
    }

    // ── Alert checks (buzzer + flash) ────────────────────────────────
    alert_cycle_begin();
    if (buzzer_mode == 1) {
        // Global: check all screens
        for (int s = 0; s < NUM_SCREENS; s++)
            check_screen_alerts(s);
    } else if (buzzer_mode == 2) {
        // Per-screen: only active screen
        check_screen_alerts(current_screen);
    } else {
        // Off: still check for flash on active screen (flash is per-screen)
        check_screen_alerts(current_screen);
    }
    alert_cycle_end();
    apply_zone_flash();

    // Remote alert icon: flash when a non-active screen has an alert
    if (remote_alert_icon) {
        if (is_remote_alert_on()) {
            lv_obj_clear_flag(remote_alert_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(remote_alert_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    LVGL_Unlock();
}
