#pragma once

#include "Arduino.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"

#define CST820_ADDR           0x15
#define CST820_INT_PIN        16


#define CST820_LCD_TOUCH_MAX_POINTS             (1)   
/* CST820 GESTURE */
enum GESTURE {
  NONE = 0x00,
  SWIPE_UP = 0x01,
  SWIPE_DOWN = 0x02,
  SWIPE_LEFT = 0x03,
  SWIPE_RIGHT = 0x04,
  SINGLE_CLICK = 0x05,
  DOUBLE_CLICK = 0x0B,
  LONG_PRESS = 0x0C

};
//debug info
/****************HYN_REG_MUT_DEBUG_INFO_MODE address start***********/
#define CST820_REG_Mode           0x00
#define CST820_REG_GestureID      0x01
#define CST820_REG_Version        0x15
#define CST820_REG_ChipID         0xA7
#define CST820_REG_ProjID         0xA8
#define CST820_REG_FwVersion      0xA9
#define CST820_REG_AutoSleepTime  0xF9
#define CST820_REG_DisAutoSleep   0xFE

//Interrupt Modes
#define RISING    0x01
#define FALLING   0x02
#define CHANGE    0x03
#define ONLOW     0x04
#define ONHIGH    0x05
#define ONLOW_WE  0x0C
#define ONHIGH_WE 0x0D

#define interrupt RISING

extern uint8_t Touch_interrupts;
extern struct CST820_Touch touch_data;

struct CST820_Touch{
  uint8_t points;     // Number of touch points
  GESTURE gesture;    /*!< uint8_t */
  uint16_t x;         /*!< X coordinate */
  uint16_t y;         /*!< Y coordinate */
};


uint8_t Touch_Init();
// False on boards without a touch panel (non-touch versions of the display).
// While false the driver only checks for the panel every few seconds.
bool Touch_Present(void);
uint8_t CST820_Touch_Reset(void);
void CST820_AutoSleep(bool Sleep_State);
uint16_t CST820_Read_cfg(void);
String Touch_GestureName(void);
uint8_t Touch_Read_Data(void);
void example_touchpad_read(void);
void IRAM_ATTR Touch_CST820_ISR(void);
