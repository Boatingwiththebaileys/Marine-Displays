#pragma once
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include <esp_err.h>

#include "TCA9554PWR.h"
#include "LVGL_Driver.h"
#include "Touch_GT911.h"

// Disable runtime panel tests/diagnostics when set to 1
#define REMOVE_PANEL_TESTS 1

#define LCD_CLK_PIN   2
#define LCD_MOSI_PIN  1 
#define LCD_Backlight_PIN   -1 // Use expander BL_EN (EXIO_PIN2) only; MCU PWM disabled
// If your board uses a GPIO for LCD CS set it here, otherwise -1 to use expander
#define LCD_CS_PIN     42
// Backlight   ledChannel：PWM Channe 
#define PWM_Channel     1       // PWM Channel   
#define Frequency       20000   // PWM frequencyconst        
#define Resolution      10       // PWM resolution ratio     MAX:13
#define Dutyfactor      500     // PWM Dutyfactor      
#define Backlight_MAX   100      


#define ESP_PANEL_LCD_WIDTH                       (480)
#define ESP_PANEL_LCD_HEIGHT                      (480)
#define ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ          (10 * 1000 * 1000) 
#define ESP_PANEL_LCD_RGB_TIMING_HPW              (8)
#define ESP_PANEL_LCD_RGB_TIMING_HBP              (50)   // back porch (match Arduino demo)
#define ESP_PANEL_LCD_RGB_TIMING_HFP              (10)   // front porch (match Arduino demo)
#define ESP_PANEL_LCD_RGB_TIMING_VPW              (8)    // vsync pulse width
#define ESP_PANEL_LCD_RGB_TIMING_VBP              (40)   // vsync back porch (increased for stability)
#define ESP_PANEL_LCD_RGB_TIMING_VFP              (10)   // vsync front porch
#define ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG         (1)     // 1: falling edge (match demo)
#define ESP_PANEL_LCD_RGB_DATA_WIDTH              (16)
#define ESP_PANEL_LCD_RGB_PIXEL_BITS              (16)    // 24 | 18 | 16 (using RGB666 (0x60) to match Arduino demo)
#define ESP_PANEL_LCD_RGB_FRAME_BUF_NUM           (2)     // Double buffering - display one while writing to other
#define ESP_PANEL_LCD_BOUNCE_BUF_SIZE             (0)   // No bounce buffer - use framebuffer directly (zero-copy mode)


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your board spec ////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC           (38)
#define ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC           (39)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DE              (40)
#define ESP_PANEL_LCD_PIN_NUM_RGB_PCLK            (41)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DISP            (-1)
/* Mapping variant macro removed — runtime mapping variants are unused when using the Arduino demo fixed mapping. */
    /* Using Waveshare demo mapping for RGB666/RGB565 (variant 0 = Arduino demo mapping) */
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA0           (5)   // D0 / B0
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA1           (45)  // D1 / B1
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA2           (48)  // D2 / B2
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA3           (47)  // D3 / B3
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA4           (21)  // D4 / B4
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA5           (14)  // D5 / G0
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA6           (13)  // D6 / G1
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA7           (12)  // D7 / G2
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA8           (11)  // G3
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA9           (10)  // G4
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA10          (9)   // G5
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA11          (46)  // R0
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA12          (3)   // R1
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA13          (8)   // R2
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA14          (18)  // R3
    #define ESP_PANEL_LCD_PIN_NUM_RGB_DATA15          (17)  // R4

extern uint8_t LCD_Backlight;
extern esp_lcd_panel_handle_t panel_handle;
bool example_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data);
uint32_t get_vsync_count();
uint32_t get_vsync_max_gap_us();
void reset_vsync_stats();
#define USE_SWSPI_VENDOR_INIT 0

void ST7701_Init();

void LCD_Init();
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint8_t* color);
// Diagnostic helpers - enable with build define: ENABLE_RGB_DIAG
void Display_RunDiagnostics();
void Display_RunDiagnosticsLoop(int cycles, uint32_t ms_each);
void Display_RunDiagnosticsAsync(int cycles, uint32_t ms_each);
#ifdef ENABLE_RGB_DIAG
// Handle a single-character diagnostic command forwarded from `main` or serial input.
void Display_Diag_HandleChar(char c);
#endif

// SDA diagnostic wrappers (call into 3-wire driver tests)
esp_err_t Display_SDA_Probe();
esp_err_t Display_SDA_Hold_Test(uint32_t hold_ms);

// backlight
void Backlight_Init();
void Set_Backlight(uint8_t Light);    