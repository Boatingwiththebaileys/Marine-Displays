#include "screen_config_c_api.h"
#include "calibration_types.h"
#include "signalk_config.h"

// Define storage for screen configs and runtime calibration
ScreenConfig screen_configs[NUM_SCREENS];
GaugeCalibrationPoint gauge_cal[NUM_SCREENS][2][5];
