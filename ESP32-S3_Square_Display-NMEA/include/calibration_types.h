#ifndef CALIBRATION_TYPES_H
#define CALIBRATION_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int angle;
    float value;
} __attribute__((packed)) GaugeCalibrationPoint;

#ifdef __cplusplus
}
#endif

#endif // CALIBRATION_TYPES_H
