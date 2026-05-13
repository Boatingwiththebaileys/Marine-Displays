#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Create the attitude indicator overlay on the given screen (0-4).
void attitude_display_create(int screen_num);

// Update with new pitch/roll (degrees, already calibrated) and yaw rate (°/s).
// Positive pitch = bow up, positive roll = starboard heel.
void attitude_display_update(int screen_num, float pitch_deg, float roll_deg, float yaw_rate_dps);

// Remove all LVGL objects for this screen.
void attitude_display_destroy(int screen_num);

// Read IMU, apply calibration offsets, compute pitch/roll/yaw.
// Call this once per loop iteration (50 Hz is fine).
void attitude_imu_read(float *pitch_deg, float *roll_deg, float *yaw_rate_dps);

// Store the current N2K pitch/roll as the "level" reference offset.  Persisted to NVS.
void attitude_calibrate_level(void);

// Clear calibration offsets back to zero (returns to raw N2K data).  Persisted to NVS.
void attitude_clear_calibration(void);

// Load calibration offsets from NVS (called once at startup).
void attitude_load_calibration(void);

#ifdef __cplusplus
}
#endif
