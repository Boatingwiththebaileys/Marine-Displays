#pragma once

#include <stdio.h>
#include "I2C_Driver.h"

#define TCA9554_EXIO1 0x01
#define TCA9554_EXIO2 0x02
#define TCA9554_EXIO3 0x03
#define TCA9554_EXIO4 0x04
#define TCA9554_EXIO5 0x05
#define TCA9554_EXIO6 0x06
#define TCA9554_EXIO7 0x07

/****************************************************** IO Expander register map ******************************************************/
// V3 boards: TCA9554 at 0x20       V4 boards: CH32V003 at 0x24
// Reg 0x00: INPUT (read pins)       Reg 0x02: DIRECTION (1=out,0=in)
// Reg 0x01: OUTPUT (R/W)            Reg 0x03: OUTPUT (R/W)
// Reg 0x02: POLARITY                Reg 0x04: INPUT (read pins)
// Reg 0x03: CONFIG (0=out,1=in)     Reg 0x05: PWM (backlight, 0=bright, 255=off)
//                                   Reg 0x06: ADC (battery, 2 bytes LE)
//                                   Reg 0x07: RTC_INT
// Register map verified from Waveshare official custom_io_expander_ch32v003 driver.

#define TCA9554_ADDR_V3         0x20                      // TCA9554 I2C address (v3 boards)
#define TCA9554_ADDR_V4         0x24                      // CH32V003 I2C address (v4 boards)
extern uint8_t g_tca9554_address;                         // Detected at runtime
#define TCA9554_ADDRESS         g_tca9554_address          // Use detected address

// V3 (TCA9554) register addresses
#define TCA9554_INPUT_REG       0x00                      // TCA9554 input register
#define TCA9554_OUTPUT_REG      0x01                      // TCA9554 output register
#define TCA9554_Polarity_REG    0x02                      // TCA9554 polarity inversion
#define TCA9554_CONFIG_REG      0x03                      // TCA9554 direction (0=out, 1=in)

// V4 (CH32V003) register addresses — from Waveshare official driver
#define CH32V003_DIR_REG        0x02                      // Direction (1=output, 0=input)
#define CH32V003_OUTPUT_REG     0x03                      // Output pin levels
#define CH32V003_INPUT_REG      0x04                      // Input pin states
#define CH32V003_PWM_REG        0x05                      // Backlight PWM (0=bright, 255=off)
#define CH32V003_ADC_REG        0x06                      // Battery ADC (2 bytes LE, 10-bit)

// V4 CH32V003 direction mask: 1=output, 0=input
// EXIO0=in(charger), EXIO1=out(TP_RST), EXIO2=in(TP_INT), EXIO3=out(LCD_RST),
// EXIO4=out(SDCS), EXIO5=out(SYS_EN), EXIO6=in(BEE_EN safe), EXIO7=in(RTC_INT)
// Waveshare official V4 CH32V003 values (from ESP32-S3-Touch-LCD-4 example):
//   Direction 0xFF (all outputs), Output 0x3A (TP_RST=HIGH, LCD_RST=HIGH,
//   SYS_EN=HIGH, SD_CS=HIGH, BEE_EN=LOW, EXIO0/2/7=LOW).
#define CH32V003_DIR_ALL_OUT    0xFF                      // All pins output (Waveshare official)
#define CH32V003_OUT_NORMAL     0x3A                      // Normal operation — buzzer OFF
#define CH32V003_OUT_BUZZER     0x7A                      // Same + BEE_EN HIGH (buzzer ON)

// Legacy constants — kept for reference
#define CH32V003_DIR_SAFE       0x3A                      // BEE_EN as input (buzzer can't activate)
#define CH32V003_DIR_FULL       0x7A                      // BEE_EN as output (buzzer software control)

// Equivalent in TCA9554 convention (0=out, 1=in) for use with Mode_EXIOS/TCA9554PWR_Init
#define V4_DIR_TCA_CONVENTION   0xC5                      // ~0x3A — driver inverts for CH32V003


#define Low   0
#define High  1
#define EXIO_PIN1   1
#define EXIO_PIN2   2
#define EXIO_PIN3   3
#define EXIO_PIN4   4
#define EXIO_PIN5   5
#define EXIO_PIN6   6
#define EXIO_PIN7   7
#define EXIO_PIN8   8

/*****************************************************  Board detection  ****************************************************/
bool detect_expander_address();                             // Auto-detect board version (probe once, cache in NVS)
bool is_board_v4();                                         // Returns true if v4 board detected

// V4 shadow registers — CH32V003 register reads are unreliable, so
// we track direction and output state in software.
uint8_t ch32v003_get_dir_shadow();
uint8_t ch32v003_get_out_shadow();

// Returns correct register address for detected board
uint8_t exio_output_reg();                                  // V3: 0x01, V4: 0x03
uint8_t exio_dir_reg();                                     // V3: 0x03, V4: 0x02
uint8_t exio_input_reg();                                   // V3: 0x00, V4: 0x04

// Functional pin numbers — V3/V4 have different EXIO-to-function mappings.
// Returns 1-indexed Pin number for use with Set_EXIO / Mode_EXIO.
// V4 pins are shifted +1 because CH32V003 added EXIO0 (charger) at bit0.
uint8_t pin_tp_rst();                                       // V3=pin1(IO0), V4=pin2(EXIO1)
uint8_t pin_lcd_rst();                                      // V3=pin3(IO2), V4=pin4(EXIO3)
uint8_t pin_sdcs();                                         // V3=pin4(IO3), V4=pin5(EXIO4)

// V4-only pin numbers (1-indexed) — V3 has no buzzer or SYS_EN
#define PIN_BEE_EN   7                                      // EXIO6/bit6 (buzzer, V4 only)
#define PIN_SYS_EN   6                                      // EXIO5/bit5 (LCD power, V4 only)

/*****************************************************  Operation register REG   ****************************************************/   
bool    I2C_Read_EXIO_safe(uint8_t REG, uint8_t *out);           // Read register; returns false on I2C failure (out unchanged)
uint8_t I2C_Read_EXIO(uint8_t REG);                              // Read register (returns 0xFF on failure — prefer _safe variant)
uint8_t I2C_Write_EXIO(uint8_t REG,uint8_t Data);               // Write Data to the REG register
/********************************************************** Set EXIO mode **********************************************************/       
void Mode_EXIO(uint8_t Pin,uint8_t State);                  // Set pin direction. State: 0=Output 1=Input (auto-inverts for V4)
void Mode_EXIOS(uint8_t PinState);                          // Set all pin directions (TCA convention: 0=out,1=in; auto-inverts for V4)
/********************************************************** Read EXIO status **********************************************************/       
uint8_t Read_EXIO(uint8_t Pin);                             // Read the level of Pin
uint8_t Read_EXIOS(uint8_t REG = 0xFF);                     // Read register (default: auto-detect input reg). Use exio_output_reg() to read outputs.
/********************************************************** Set the EXIO output status **********************************************************/  
void Set_EXIO(uint8_t Pin,uint8_t State);                   // Sets the level of Pin without affecting others
void Set_EXIOS(uint8_t PinState);                           // Set all output pins (writes to correct register for V3/V4)
/********************************************************** Flip EXIO state **********************************************************/  
void Set_Toggle(uint8_t Pin);                               // Flip the level of Pin
/********************************************************* Init ***********************************************************/  
void TCA9554PWR_Init(uint8_t PinState = 0x00);              // Init directions (TCA convention: 0=out,1=in). V3 default=0x00 (all out)

// V4 backlight control via CH32V003 PWM register (inverted: 0=brightest, 255=dimmest)
void backlight_set_pwm(uint8_t duty);
