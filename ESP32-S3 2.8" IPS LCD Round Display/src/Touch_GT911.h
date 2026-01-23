#pragma once

#include "Arduino.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"

#define GT911_ADDR          0x5D
#define GT911_INT_PIN       16


#define GT911_LCD_TOUCH_MAX_POINTS             (5)      
/* GT911 registers */
#define ESP_LCD_TOUCH_GT911_READ_XY_REG       (0x814E)
#define ESP_LCD_TOUCH_GT911_CONFIG_REG        (0x8047)
#define ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG    (0x8140)

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

struct GT911_Touch{
  uint8_t points;    // Number of touch points
  struct {
    uint16_t x; /*!< X coordinate */
    uint16_t y; /*!< Y coordinate */
    uint16_t strength; /*!< Strength */
  }coords[GT911_LCD_TOUCH_MAX_POINTS];
};


uint8_t Touch_Init();
void Touch_Loop(void);
uint8_t GT911_Touch_Reset(void);
void GT911_Read_cfg(void);
uint8_t Touch_Read_Data(void);
uint8_t Touch_Get_XY(uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
void example_touchpad_read(void);
void IRAM_ATTR Touch_GT911_ISR(void);
