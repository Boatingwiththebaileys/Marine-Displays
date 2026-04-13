// attitude_display.cpp — Artificial horizon / attitude indicator
// Uses onboard QMI8658 IMU when available (V4 boards),
// falls back to NMEA 2000 PGN 127257 attitude data (V3 boards).
// "Set Level" calibration stores current orientation as zero reference.

#include "attitude_display.h"
#include "screen_config_c_api.h"
#include "Gyro_QMI8658.h"
#include "signalk_config.h"
#include "nmea2000_config.h"
#include <math.h>
#include <Preferences.h>
#include <Arduino.h>

// UI screen objects
#include "ui.h"

// Fonts are declared in ui.h / font headers included above

// ── Constants ────────────────────────────────────────────────────────
#define SCR_W 480
#define SCR_H 480
#define CX    (SCR_W / 2)
#define CY    (SCR_H / 2)
#define DEG2RAD(d) ((d) * 3.14159265f / 180.0f)

// Pitch: pixels-per-degree — controls how fast the horizon moves
#define PX_PER_DEG  6.0f

// Roll arc radius and tick marks
#define ARC_R        200
#define ARC_TICK_LEN 18

// Pitch ladder half-width
#define LADDER_HW    100

// ── Calibration offsets (stored in NVS) ──────────────────────────────
static float g_cal_pitch = 0.0f;   // offset in degrees
static float g_cal_roll  = 0.0f;

// ── Low-pass filter state (accelerometer only — no gyro drift) ───────
static float g_filt_pitch = 0.0f;
static float g_filt_roll  = 0.0f;
static bool  g_filt_init  = false;
static uint32_t g_dbg_count = 0;
#define LPF_ALPHA 0.08f   // smoothing factor: lower = smoother, higher = faster response

// ── Per-screen LVGL objects ──────────────────────────────────────────
static lv_obj_t* a_bg[NUM_SCREENS];         // background panel
static lv_obj_t* a_sky[NUM_SCREENS];        // sky rectangle (rotated)
static lv_obj_t* a_gnd[NUM_SCREENS];        // ground/water rectangle (rotated)
static lv_obj_t* a_horizon_line[NUM_SCREENS];// horizon line
static lv_obj_t* a_pitch_lbl[NUM_SCREENS][9]; // pitch ladder: -20,-15,-10,-5,0,+5,+10,+15,+20
static lv_obj_t* a_roll_pointer[NUM_SCREENS]; // roll pointer triangle
static lv_obj_t* a_pitch_txt[NUM_SCREENS];   // digital pitch readout
static lv_obj_t* a_roll_txt[NUM_SCREENS];    // digital roll readout
static lv_obj_t* a_yaw_txt[NUM_SCREENS];     // rate of turn readout
static lv_obj_t* a_center_mark[NUM_SCREENS]; // fixed center reference wings
static lv_obj_t* a_info_band[NUM_SCREENS];   // dark band behind bottom readouts
// Roll arc tick lines (marine heel angles: -45,-30,-20,-10,0,+10,+20,+30,+45)
#define NUM_ARC_TICKS 9
static lv_obj_t* a_arc_tick[NUM_SCREENS][NUM_ARC_TICKS];
static lv_obj_t* a_arc_lbl[NUM_SCREENS][NUM_ARC_TICKS]; // labels

static bool a_created[NUM_SCREENS] = {};

// ── Helper: get parent screen object ─────────────────────────────────
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

// ── Helper: get background image object for each screen ──────────────
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

// ── IMU reading + calibration ────────────────────────────────────────

void attitude_load_calibration(void) {
    Preferences prefs;
    if (prefs.begin("imu_cal", true)) {
        g_cal_pitch = prefs.getFloat("pitch", 0.0f);
        g_cal_roll  = prefs.getFloat("roll",  0.0f);
        prefs.end();
        Serial.printf("[IMU] Calibration loaded: pitch=%.2f roll=%.2f\n", g_cal_pitch, g_cal_roll);
    }
}

void attitude_calibrate_level(void) {
    // Read accelerometer right now
    getAccelerometer();
    float ax = Accel.x, ay = Accel.y, az = Accel.z;

    // Raw pitch/roll from gravity
    float raw_pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / M_PI;
    float raw_roll  = atan2f(ay, az) * 180.0f / M_PI;

    g_cal_pitch = raw_pitch;
    g_cal_roll  = raw_roll;

    // Reset filter after calibration
    g_filt_pitch = 0.0f;
    g_filt_roll  = 0.0f;
    g_filt_init  = false;

    // Persist to NVS
    Preferences prefs;
    if (prefs.begin("imu_cal", false)) {
        prefs.putFloat("pitch", g_cal_pitch);
        prefs.putFloat("roll",  g_cal_roll);
        prefs.end();
    }
    Serial.printf("[IMU] Level set: pitch_offset=%.2f roll_offset=%.2f\n", g_cal_pitch, g_cal_roll);
}

void attitude_imu_read(float *pitch_deg, float *roll_deg, float *yaw_rate_dps) {
    // If onboard IMU not available, use NMEA 2000 PGN 127257 data
    if (!imu_is_available()) {
        float n2k_pitch = get_sensor_value_by_path(String((int)N2K_PITCH));  // radians
        float n2k_roll  = get_sensor_value_by_path(String((int)N2K_ROLL));   // radians
        float n2k_yaw   = get_sensor_value_by_path(String((int)N2K_YAW));    // radians
        *pitch_deg    = isnan(n2k_pitch) ? 0.0f : n2k_pitch * 180.0f / M_PI;
        *roll_deg     = isnan(n2k_roll)  ? 0.0f : n2k_roll  * 180.0f / M_PI;
        *yaw_rate_dps = isnan(n2k_yaw)   ? 0.0f : n2k_yaw   * 180.0f / M_PI;
        return;
    }

    // Rate-limit I2C reads to avoid bus contention with touch/RTC/IO expander
    static uint32_t last_read_ms = 0;
    uint32_t now_ms = millis();
    if (now_ms - last_read_ms < 200) {
        // Return cached filtered values
        *pitch_deg = g_filt_pitch;
        *roll_deg  = g_filt_roll;
        *yaw_rate_dps = 0.0f;
        return;
    }
    last_read_ms = now_ms;

    // Read accelerometer — skip this cycle if I2C fails
    if (!getAccelerometer()) {
        *pitch_deg = g_filt_pitch;
        *roll_deg  = g_filt_roll;
        *yaw_rate_dps = 0.0f;
        return;
    }
    getGyroscope();  // gyro failure is non-critical (only used for ROT display)

    float ax = Accel.x, ay = Accel.y, az = Accel.z;

    // Sanity check: accelerometer magnitude should be ~1g
    // Reject readings that are way off (I2C glitch / bus contention)
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    if (mag < 0.5f || mag > 2.0f || isnan(mag)) {
        // Bad reading — keep previous filtered values
        if (g_dbg_count++ % 10 == 0)
            Serial.printf("[IMU] BAD accel mag=%.3f (ax=%.3f ay=%.3f az=%.3f)\n", mag, ax, ay, az);
        *pitch_deg = g_filt_pitch;
        *roll_deg  = g_filt_roll;
        *yaw_rate_dps = 0.0f;
        return;
    }

    // Pitch/roll from accelerometer (gravity) with calibration offset
    float acc_pitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 180.0f / M_PI - g_cal_pitch;
    float acc_roll  = atan2f(ay, az) * 180.0f / M_PI - g_cal_roll;

    // Debug: print every 10th successful reading (~1x/sec at 5Hz)
    if (g_dbg_count++ % 10 == 0) {
        Serial.printf("[IMU] ax=%.3f ay=%.3f az=%.3f mag=%.3f -> raw P=%.1f R=%.1f -> filt P=%.1f R=%.1f\n",
                      ax, ay, az, mag, acc_pitch, acc_roll, g_filt_pitch, g_filt_roll);
    }

    // Simple low-pass filter on accelerometer — no gyro integration, no drift
    if (!g_filt_init) {
        g_filt_pitch = acc_pitch;
        g_filt_roll  = acc_roll;
        g_filt_init  = true;
    } else {
        g_filt_pitch += LPF_ALPHA * (acc_pitch - g_filt_pitch);
        g_filt_roll  += LPF_ALPHA * (acc_roll  - g_filt_roll);
    }

    *pitch_deg = g_filt_pitch;
    *roll_deg  = g_filt_roll;
    *yaw_rate_dps = Gyro.z;  // gyro only used for rate-of-turn display
}

// ── LVGL display creation ────────────────────────────────────────────

void attitude_display_create(int n) {
    if (n < 0 || n >= NUM_SCREENS) return;
    if (a_created[n]) attitude_display_destroy(n);

    lv_obj_t* parent = get_screen_obj(n);
    if (!parent) return;

    // Hide the screen's background image so it doesn't show behind us
    lv_obj_t* bg_img = get_bg_img_obj(n);
    if (bg_img) lv_obj_add_flag(bg_img, LV_OBJ_FLAG_HIDDEN);

    // ── Full-screen background (black) ───────────────────────────────
    a_bg[n] = lv_obj_create(parent);
    lv_obj_remove_style_all(a_bg[n]);
    lv_obj_set_size(a_bg[n], SCR_W, SCR_H);
    lv_obj_set_pos(a_bg[n], 0, 0);
    lv_obj_set_style_bg_color(a_bg[n], lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a_bg[n], LV_OPA_COVER, 0);
    lv_obj_clear_flag(a_bg[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ── Water half (ocean teal-green) ─────────────────────────────
    a_gnd[n] = lv_obj_create(a_bg[n]);
    lv_obj_remove_style_all(a_gnd[n]);
    lv_obj_set_size(a_gnd[n], SCR_W, SCR_H / 2);
    lv_obj_set_style_bg_color(a_gnd[n], lv_color_make(0, 50, 70), 0); // deep ocean teal
    lv_obj_set_style_bg_opa(a_gnd[n], LV_OPA_COVER, 0);
    lv_obj_clear_flag(a_gnd[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ── Sky half (warm marine sky) ───────────────────────────────────
    a_sky[n] = lv_obj_create(a_bg[n]);
    lv_obj_remove_style_all(a_sky[n]);
    lv_obj_set_size(a_sky[n], SCR_W, SCR_H / 2);
    lv_obj_set_style_bg_color(a_sky[n], lv_color_make(100, 160, 210), 0); // warm horizon sky
    lv_obj_set_style_bg_opa(a_sky[n], LV_OPA_COVER, 0);
    lv_obj_clear_flag(a_sky[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ── Horizon line ─────────────────────────────────────────────────
    a_horizon_line[n] = lv_line_create(a_bg[n]);
    lv_obj_set_style_line_color(a_horizon_line[n], lv_color_white(), 0);
    lv_obj_set_style_line_width(a_horizon_line[n], 3, 0);

    // ── Pitch ladder (marine: ±5, ±10, ±15, ±20 — 0 is the horizon)
    static const int pitch_angles[] = {-20, -15, -10, -5, 0, 5, 10, 15, 20};
    for (int i = 0; i < 9; i++) {
        a_pitch_lbl[n][i] = lv_label_create(a_bg[n]);
        lv_obj_set_style_text_font(a_pitch_lbl[n][i], &inter_16, 0);
        if (pitch_angles[i] == 0) {
            lv_obj_set_style_text_color(a_pitch_lbl[n][i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_text_color(a_pitch_lbl[n][i], lv_color_make(200, 200, 200), 0);
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%+d", pitch_angles[i]);
        lv_label_set_text(a_pitch_lbl[n][i], pitch_angles[i] == 0 ? "" : buf);
    }

    // ── Roll arc ticks (marine heel angles) ──────────────────────────
    static const int arc_angles[] = {-45, -30, -20, -10, 0, 10, 20, 30, 45};
    for (int i = 0; i < NUM_ARC_TICKS; i++) {
        a_arc_tick[n][i] = lv_line_create(a_bg[n]);
        lv_obj_set_style_line_color(a_arc_tick[n][i], lv_color_white(), 0);
        lv_obj_set_style_line_width(a_arc_tick[n][i], (arc_angles[i] == 0) ? 3 : 2, 0);

        a_arc_lbl[n][i] = lv_label_create(a_bg[n]);
        lv_obj_set_style_text_font(a_arc_lbl[n][i], &inter_16, 0);
        lv_obj_set_style_text_color(a_arc_lbl[n][i], lv_color_make(200, 200, 200), 0);
        if (arc_angles[i] == 0) {
            lv_label_set_text(a_arc_lbl[n][i], "");
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", abs(arc_angles[i]));
            lv_label_set_text(a_arc_lbl[n][i], buf);
        }
    }

    // ── Heel pointer (triangle at top center) ────────────────────────
    a_roll_pointer[n] = lv_line_create(a_bg[n]);
    lv_obj_set_style_line_color(a_roll_pointer[n], lv_color_white(), 0);
    lv_obj_set_style_line_width(a_roll_pointer[n], 3, 0);

    // ── Fixed center reference (boat bow / keel) ─────────────────────
    a_center_mark[n] = lv_line_create(a_bg[n]);
    static lv_point_t bow_pts[7] = {
        {CX - 80, CY + 4}, {CX - 25, CY + 4},  // port beam
        {CX - 10, CY},     {CX, CY - 12},       // bow point
        {CX + 10, CY},
        {CX + 25, CY + 4}, {CX + 80, CY + 4}   // starboard beam
    };
    lv_line_set_points(a_center_mark[n], bow_pts, 7);
    lv_obj_set_style_line_color(a_center_mark[n], lv_color_white(), 0);
    lv_obj_set_style_line_width(a_center_mark[n], 3, 0);

    // ── Dark band behind bottom readouts ──────────────────────────────
    a_info_band[n] = lv_obj_create(a_bg[n]);
    lv_obj_remove_style_all(a_info_band[n]);
    lv_obj_set_size(a_info_band[n], SCR_W, 70);
    lv_obj_set_pos(a_info_band[n], 0, SCR_H - 85);
    lv_obj_set_style_bg_color(a_info_band[n], lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a_info_band[n], LV_OPA_70, 0);
    lv_obj_clear_flag(a_info_band[n], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ── Digital readouts ─────────────────────────────────────────────
    // Pitch (left side)
    a_pitch_txt[n] = lv_label_create(a_bg[n]);
    lv_obj_set_style_text_font(a_pitch_txt[n], &inter_24, 0);
    lv_obj_set_style_text_color(a_pitch_txt[n], lv_color_white(), 0);
    lv_label_set_text(a_pitch_txt[n], "Trim: ---");
    lv_obj_align(a_pitch_txt[n], LV_ALIGN_BOTTOM_LEFT, 20, -50);

    // Roll (right side)
    a_roll_txt[n] = lv_label_create(a_bg[n]);
    lv_obj_set_style_text_font(a_roll_txt[n], &inter_24, 0);
    lv_obj_set_style_text_color(a_roll_txt[n], lv_color_white(), 0);
    lv_label_set_text(a_roll_txt[n], "Heel: ---");
    lv_obj_align(a_roll_txt[n], LV_ALIGN_BOTTOM_RIGHT, -20, -50);

    // Rate of turn (bottom center)
    a_yaw_txt[n] = lv_label_create(a_bg[n]);
    lv_obj_set_style_text_font(a_yaw_txt[n], &inter_24, 0);
    lv_obj_set_style_text_color(a_yaw_txt[n], lv_color_white(), 0);
    lv_label_set_text(a_yaw_txt[n], "ROT: ---");
    lv_obj_align(a_yaw_txt[n], LV_ALIGN_BOTTOM_MID, 0, -20);

    a_created[n] = true;
    Serial.printf("[ATTITUDE] Created on screen %d\n", n);
}

// ── Update ───────────────────────────────────────────────────────────

void attitude_display_update(int n, float pitch_deg, float roll_deg, float yaw_rate_dps) {
    if (n < 0 || n >= NUM_SCREENS || !a_created[n]) return;

    // Clamp for display sanity
    if (pitch_deg > 60.0f)  pitch_deg = 60.0f;
    if (pitch_deg < -60.0f) pitch_deg = -60.0f;
    if (roll_deg > 60.0f)   roll_deg = 60.0f;
    if (roll_deg < -60.0f)  roll_deg = -60.0f;

    float roll_rad = DEG2RAD(roll_deg);
    float sin_r = sinf(roll_rad);
    float cos_r = cosf(roll_rad);

    // ── Position sky and ground panels ───────────────────────────────
    // Simple approach: sky covers top half, ground covers bottom half,
    // split at the pitch-adjusted horizon line. The horizon line itself
    // is drawn with roll rotation, giving the attitude indicator look.
    float horizon_y = CY + pitch_deg * PX_PER_DEG;

    // Sky: from top to horizon
    int sky_h = (int)horizon_y;
    if (sky_h < 0) sky_h = 0;
    if (sky_h > SCR_H) sky_h = SCR_H;
    lv_obj_set_pos(a_sky[n], 0, 0);
    lv_obj_set_size(a_sky[n], SCR_W, sky_h);

    // Ground: from horizon to bottom
    lv_obj_set_pos(a_gnd[n], 0, sky_h);
    lv_obj_set_size(a_gnd[n], SCR_W, SCR_H - sky_h);

    // ── Horizon line (rotated across center) ─────────────────────────
    static lv_point_t h_pts[NUM_SCREENS][2];
    int hw = 400; // half-width of line
    h_pts[n][0].x = CX + (int)(-hw * cos_r);
    h_pts[n][0].y = (int)(horizon_y + hw * sin_r);
    h_pts[n][1].x = CX + (int)(hw * cos_r);
    h_pts[n][1].y = (int)(horizon_y - hw * sin_r);
    lv_line_set_points(a_horizon_line[n], h_pts[n], 2);

    // ── Pitch (trim) ladder ───────────────────────────────────────────
    static const int pitch_angles[] = {-20, -15, -10, -5, 0, 5, 10, 15, 20};
    for (int i = 0; i < 9; i++) {
        float py = horizon_y - pitch_angles[i] * PX_PER_DEG;
        // Rotate the label position around the horizon center by roll
        float dx = 0.0f;
        float dy = py - horizon_y;
        float rx = dx * cos_r - dy * sin_r;
        float ry = dx * sin_r + dy * cos_r;
        int lx = CX + (int)rx + LADDER_HW + 70; // offset right of ladder, clear of roll arc
        int ly = (int)(horizon_y + ry) - 18;  // shift up away from lines
        lv_obj_set_pos(a_pitch_lbl[n][i], lx, ly);

        // Visibility: only show if within visible area
        bool vis = (ly > 30 && ly < SCR_H - 30);
        if (vis) lv_obj_clear_flag(a_pitch_lbl[n][i], LV_OBJ_FLAG_HIDDEN);
        else     lv_obj_add_flag(a_pitch_lbl[n][i], LV_OBJ_FLAG_HIDDEN);
    }

    // ── Roll (heel) arc ticks (fixed at top, labels rotate) ──────────
    static const int arc_angles_deg[] = {-45, -30, -20, -10, 0, 10, 20, 30, 45};
    for (int i = 0; i < NUM_ARC_TICKS; i++) {
        // Tick angle on the arc (0° = straight up, negative = left)
        float a = DEG2RAD(arc_angles_deg[i] - 90.0f); // -90 so 0° is top
        float x_inner = CX + (ARC_R - ARC_TICK_LEN) * cosf(a);
        float y_inner = CY + (ARC_R - ARC_TICK_LEN) * sinf(a);
        float x_outer = CX + ARC_R * cosf(a);
        float y_outer = CY + ARC_R * sinf(a);

        static lv_point_t tick_pts[NUM_SCREENS][NUM_ARC_TICKS][2];
        tick_pts[n][i][0].x = (int)x_inner;
        tick_pts[n][i][0].y = (int)y_inner;
        tick_pts[n][i][1].x = (int)x_outer;
        tick_pts[n][i][1].y = (int)y_outer;
        lv_line_set_points(a_arc_tick[n][i], tick_pts[n][i], 2);

        // Label just outside the arc
        float x_lbl = CX + (ARC_R + 18) * cosf(a);
        float y_lbl = CY + (ARC_R + 18) * sinf(a);
        lv_obj_set_pos(a_arc_lbl[n][i], (int)x_lbl - 8, (int)y_lbl - 8);
    }

    // ── Roll pointer (gold triangle pointing inward at the roll angle)
    {
        float a = DEG2RAD(roll_deg - 90.0f);
        float x_tip  = CX + (ARC_R - ARC_TICK_LEN - 4) * cosf(a);
        float y_tip  = CY + (ARC_R - ARC_TICK_LEN - 4) * sinf(a);
        float x_base_l = CX + ARC_R * cosf(a - 0.06f);
        float y_base_l = CY + ARC_R * sinf(a - 0.06f);
        float x_base_r = CX + ARC_R * cosf(a + 0.06f);
        float y_base_r = CY + ARC_R * sinf(a + 0.06f);

        static lv_point_t ptr_pts[NUM_SCREENS][4];
        ptr_pts[n][0].x = (int)x_base_l; ptr_pts[n][0].y = (int)y_base_l;
        ptr_pts[n][1].x = (int)x_tip;    ptr_pts[n][1].y = (int)y_tip;
        ptr_pts[n][2].x = (int)x_base_r; ptr_pts[n][2].y = (int)y_base_r;
        ptr_pts[n][3].x = (int)x_base_l; ptr_pts[n][3].y = (int)y_base_l;
        lv_line_set_points(a_roll_pointer[n], ptr_pts[n], 4);
    }

    // ── Digital readouts ─────────────────────────────────────────────
    {
        char buf[32];
        const char* p_dir = (pitch_deg >= 0) ? "Bow Up" : "Bow Dn";
        snprintf(buf, sizeof(buf), "Trim:%+.1f\xC2\xB0 %s", pitch_deg, p_dir);
        lv_label_set_text(a_pitch_txt[n], buf);
        lv_obj_align(a_pitch_txt[n], LV_ALIGN_BOTTOM_LEFT, 20, -50);

        const char* r_dir = (roll_deg >= 0) ? "Stbd" : "Port";
        snprintf(buf, sizeof(buf), "Heel:%+.1f\xC2\xB0 %s", roll_deg, r_dir);
        lv_label_set_text(a_roll_txt[n], buf);
        lv_obj_align(a_roll_txt[n], LV_ALIGN_BOTTOM_RIGHT, -20, -50);

        // Rate of turn in °/min for marine convention
        float rot_deg_min = yaw_rate_dps * 60.0f;
        const char* rot_dir = (rot_deg_min >= 0) ? "S" : "P"; // Stbd / Port
        snprintf(buf, sizeof(buf), "ROT: %.0f\xC2\xB0/m %s", fabsf(rot_deg_min), rot_dir);
        lv_label_set_text(a_yaw_txt[n], buf);
        lv_obj_align(a_yaw_txt[n], LV_ALIGN_BOTTOM_MID, 0, -20);
    }
}

// ── Destroy ──────────────────────────────────────────────────────────

void attitude_display_destroy(int n) {
    if (n < 0 || n >= NUM_SCREENS || !a_created[n]) return;

    // Restore the screen's background image
    lv_obj_t* bg_img = get_bg_img_obj(n);
    if (bg_img) lv_obj_clear_flag(bg_img, LV_OBJ_FLAG_HIDDEN);

    // Deleting the background panel removes all children too
    if (a_bg[n]) { lv_obj_del(a_bg[n]); a_bg[n] = NULL; }

    a_sky[n] = NULL;
    a_gnd[n] = NULL;
    a_horizon_line[n] = NULL;
    a_roll_pointer[n] = NULL;
    a_center_mark[n] = NULL;
    a_info_band[n] = NULL;
    a_pitch_txt[n] = NULL;
    a_roll_txt[n] = NULL;
    a_yaw_txt[n] = NULL;
    for (int i = 0; i < 9; i++) a_pitch_lbl[n][i] = NULL;
    for (int i = 0; i < NUM_ARC_TICKS; i++) {
        a_arc_tick[n][i] = NULL;
        a_arc_lbl[n][i] = NULL;
    }

    a_created[n] = false;
    Serial.printf("[ATTITUDE] Destroyed on screen %d\n", n);
}
