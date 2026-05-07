#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include <driver/gpio.h>

// ─── ST7305 Reflective LCD (SPI) ───
#define RLCD_MOSI_PIN   GPIO_NUM_12
#define RLCD_SCK_PIN    GPIO_NUM_11
#define RLCD_DC_PIN     GPIO_NUM_5
#define RLCD_CS_PIN     GPIO_NUM_40
#define RLCD_RST_PIN    GPIO_NUM_41
#define RLCD_TE_PIN     GPIO_NUM_6    // Tearing-effect (optional sync)

// Display resolution (landscape)
#define RLCD_WIDTH      400
#define RLCD_HEIGHT     300

// ─── I2C Bus ───
#define I2C_SDA_PIN     GPIO_NUM_13
#define I2C_SCL_PIN     GPIO_NUM_14

// ─── I2C Device Addresses ───
#define PCF85063_ADDR   0x51   // RTC
#define SHTC3_ADDR      0x70   // Temperature / Humidity sensor

// ─── SD Card (SDMMC 1-bit) ───
#define SD_CLK_PIN      GPIO_NUM_38
#define SD_CMD_PIN      GPIO_NUM_21
#define SD_D0_PIN       GPIO_NUM_39

// ─── I2S Audio (ES8311 codec → speaker) ───
#define I2S_MCLK_PIN    GPIO_NUM_16
#define I2S_BCLK_PIN    GPIO_NUM_9
#define I2S_WS_PIN      GPIO_NUM_45
#define I2S_DOUT_PIN    GPIO_NUM_8    // ESP32 → ES8311 (playback)
#define I2S_DIN_PIN     GPIO_NUM_10   // ES8311 → ESP32 (record, unused)
#define PA_EN_PIN       GPIO_NUM_46   // Power amplifier enable

// ─── Audio codec I2C addresses ───
#define ES8311_ADDR     0x18
#define ES7210_ADDR     0x40

// ─── Buttons ───
#define BOOT_BTN_PIN    GPIO_NUM_0    // BOOT button (active low)
#define KEY_BTN_PIN     GPIO_NUM_18   // KEY button (active low)
// PWR button is hardware-only (power latch), not readable via GPIO

#endif // BOARD_PINS_H
