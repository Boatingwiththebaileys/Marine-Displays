#include <esp_attr.h>
#include "psram_alloc.h"
#include "screen_config_c_api.h"
#include "calibration_types.h"
#include "signalk_config.h"

// Screen configs and runtime calibration live in PSRAM (see psram_alloc.h),
// freeing ~10 KB of internal RAM for TCP/WiFi buffers.
ScreenConfig* screen_configs = psram_array<ScreenConfig>(NUM_SCREENS);
GaugeCalibrationPoint (*gauge_cal)[2][5] = psram_array<GaugeCalibrationPoint[2][5]>(NUM_SCREENS);
