#include "signalk_config.h"
#include <Arduino.h>

extern "C" float ui_get_runtime_value(int screen_idx, int gauge_idx)
{
    int idx = -1;
    switch (screen_idx) {
        case 0: idx = (gauge_idx == 0 ? SCREEN1_RPM : SCREEN1_COOLANT_TEMP); break;
        case 1: idx = (gauge_idx == 0 ? SCREEN2_RPM : SCREEN2_FUEL); break;
        case 2: idx = (gauge_idx == 0 ? SCREEN3_COOLANT_TEMP : SCREEN3_EXHAUST_TEMP); break;
        case 3: idx = (gauge_idx == 0 ? SCREEN4_FUEL : SCREEN4_COOLANT_TEMP); break;
        case 4: idx = (gauge_idx == 0 ? SCREEN5_OIL_PRESSURE : SCREEN5_COOLANT_TEMP); break;
        default: idx = -1; break;
    }
    if (idx < 0) return NAN;
    float v = get_sensor_value(idx);
    return v;
}
