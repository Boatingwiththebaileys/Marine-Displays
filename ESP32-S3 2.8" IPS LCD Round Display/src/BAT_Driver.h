#pragma once
#include <Arduino.h> 

#define BAT_ADC_PIN   4
#define Measurement_offset 0.980952     

extern float BAT_analogVolts;

void BAT_Init(void);
float BAT_Get_Volts(void);