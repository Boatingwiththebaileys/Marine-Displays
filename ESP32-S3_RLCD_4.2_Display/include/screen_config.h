/*
 * Screen configuration for ESP32-S3-RLCD-4.2 Marine Display.
 *
 * Simplified from the Square Display's ScreenConfig — no color fields,
 * no gauge calibration, no icons, no background images.  The 400×300
 * reflective LCD is monochrome so those features don't apply.
 *
 * Supported display types:
 *   Number   — full-screen single value
 *   Dual     — two values stacked (top / bottom)
 *   Quad     — four values in quadrants
 *   Graph    — line/bar chart of one or two data series
 *   Compass  — heading rose with two extra data fields
 *   Position — lat/lon + UTC time
 */

#ifndef SCREEN_CONFIG_H
#define SCREEN_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NUM_SCREENS
#define NUM_SCREENS 5
#endif

// ─── Display types ──────────────────────────────────────────────────────────
typedef enum {
    DISPLAY_TYPE_NUMBER   = 0,
    DISPLAY_TYPE_DUAL     = 1,
    DISPLAY_TYPE_QUAD     = 2,
    DISPLAY_TYPE_GRAPH    = 3,
    DISPLAY_TYPE_COMPASS  = 4,
    DISPLAY_TYPE_POSITION = 5,
    DISPLAY_TYPE_GAUGE    = 6
} RlcdDisplayType;

// ─── Font sizes (custom Inter fonts + Montserrat for small UI text) ────────
typedef enum {
    RLCD_FONT_48     = 0,   // Inter 48pt
    RLCD_FONT_72     = 1,   // Inter 72pt
    RLCD_FONT_96     = 2,   // Inter 96pt
    RLCD_FONT_120    = 3,   // Inter 120pt
    RLCD_FONT_144    = 4    // Inter 144pt
} RlcdFontSize;

// ─── Graph options ──────────────────────────────────────────────────────────
typedef enum {
    RLCD_GRAPH_LINE    = 0,
    RLCD_GRAPH_BAR     = 1
} RlcdGraphType;

typedef enum {
    RLCD_GRAPH_30S  = 0,
    RLCD_GRAPH_1M   = 1,
    RLCD_GRAPH_5M   = 2,
    RLCD_GRAPH_10M  = 3,
    RLCD_GRAPH_30M  = 4
} RlcdGraphTimeRange;

// ─── Position format ────────────────────────────────────────────────────────
typedef enum {
    RLCD_POS_DD  = 0,   // Decimal degrees
    RLCD_POS_DMS = 1,   // Degrees/minutes/seconds
    RLCD_POS_DDM = 2    // Degrees/decimal minutes
} RlcdPosFormat;

// ─── Gauge style ────────────────────────────────────────────────────────────
typedef enum {
    RLCD_GAUGE_RADIAL = 0,
    RLCD_GAUGE_BAR    = 1
} RlcdGaugeStyle;

// Sentinel for "alert disabled" — use INT16_MIN so 0 can be a valid threshold
#define ALERT_OFF  ((int16_t)-32768)

// ─── Per-screen configuration ───────────────────────────────────────────────
typedef struct {
    uint8_t  display_type;        // RlcdDisplayType

    // Number
    char     number_path[128];    // SignalK path
    uint8_t  number_font_size;    // RlcdFontSize
    char     number_label[32];    // custom label (e.g. "RPM", "SOG")
    int16_t  number_alert_low;    // alert low threshold (ALERT_OFF=disabled)
    int16_t  number_alert_high;   // alert high threshold (ALERT_OFF=disabled)
    uint8_t  number_alert_flash;  // 1 = flash display on alert
    uint8_t  number_alert_buzzer; // 1 = buzzer on alert

    // Dual
    char     dual_top_path[128];
    uint8_t  dual_top_font_size;
    char     dual_top_label[32];
    int16_t  dual_top_alert_low;
    int16_t  dual_top_alert_high;
    uint8_t  dual_top_alert_flash;
    uint8_t  dual_top_alert_buzzer;
    char     dual_bottom_path[128];
    uint8_t  dual_bottom_font_size;
    char     dual_bottom_label[32];
    int16_t  dual_bot_alert_low;
    int16_t  dual_bot_alert_high;
    uint8_t  dual_bot_alert_flash;
    uint8_t  dual_bot_alert_buzzer;

    // Quad
    char     quad_tl_path[128];
    uint8_t  quad_tl_font_size;
    char     quad_tl_label[32];
    int16_t  quad_tl_alert_low;
    int16_t  quad_tl_alert_high;
    uint8_t  quad_tl_alert_flash;
    uint8_t  quad_tl_alert_buzzer;
    char     quad_tr_path[128];
    uint8_t  quad_tr_font_size;
    char     quad_tr_label[32];
    int16_t  quad_tr_alert_low;
    int16_t  quad_tr_alert_high;
    uint8_t  quad_tr_alert_flash;
    uint8_t  quad_tr_alert_buzzer;
    char     quad_bl_path[128];
    uint8_t  quad_bl_font_size;
    char     quad_bl_label[32];
    int16_t  quad_bl_alert_low;
    int16_t  quad_bl_alert_high;
    uint8_t  quad_bl_alert_flash;
    uint8_t  quad_bl_alert_buzzer;
    char     quad_br_path[128];
    uint8_t  quad_br_font_size;
    char     quad_br_label[32];
    int16_t  quad_br_alert_low;
    int16_t  quad_br_alert_high;
    uint8_t  quad_br_alert_flash;
    uint8_t  quad_br_alert_buzzer;

    // Graph
    char     graph_path_1[128];
    char     graph_path_2[128];   // optional 2nd series
    uint8_t  graph_chart_type;    // RlcdGraphType
    uint8_t  graph_time_range;    // RlcdGraphTimeRange

    // Compass
    char     compass_path[128];   // heading source (magnetic or true)
    char     compass_bl_path[128];// bottom-left extra field
    char     compass_bl_label[32];// bottom-left label override
    char     compass_br_path[128];// bottom-right extra field
    char     compass_br_label[32];// bottom-right label override

    // Position
    uint8_t  pos_format;          // RlcdPosFormat

    // Gauge
    char     gauge_path[128];     // SignalK path
    char     gauge_label[32];     // custom label override
    uint8_t  gauge_style;         // RlcdGaugeStyle (0=radial, 1=bar)
    int16_t  gauge_min;           // minimum value
    int16_t  gauge_max;           // maximum value
    int16_t  gauge_alert_low;    // alert zone start (low end, e.g. 0)
    int16_t  gauge_alert_high;   // alert zone start (high end, e.g. 6000)
    uint8_t  gauge_alert_flash;  // 1 = flash display when value enters alert zone
    uint8_t  gauge_alert_buzzer; // 1 = buzzer when value enters alert zone

} __attribute__((packed)) RlcdScreenConfig;

extern RlcdScreenConfig screen_configs[NUM_SCREENS];

#ifdef __cplusplus
}
#endif

#endif // SCREEN_CONFIG_H
