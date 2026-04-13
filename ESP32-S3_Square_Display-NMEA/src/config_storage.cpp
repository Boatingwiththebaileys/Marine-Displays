#include <esp_attr.h>
#include "screen_config_c_api.h"
#include "calibration_types.h"
#include "signalk_config.h"

// Define storage for screen configs and runtime calibration.
// EXT_RAM_ATTR places these in PSRAM instead of internal BSS,
// freeing ~10 KB of internal RAM for TCP/WiFi buffers.
EXT_RAM_ATTR ScreenConfig screen_configs[NUM_SCREENS];
EXT_RAM_ATTR GaugeCalibrationPoint gauge_cal[NUM_SCREENS][2][5];
