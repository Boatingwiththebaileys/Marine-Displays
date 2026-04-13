/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_lcd_types.h"
#include "port/esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum SPI clock speed
#define PANEL_IO_SPI_CLK_MAX      (500 * 1000UL)

/**
 * @brief Panel IO type, use GPIO or IO expander
 */
typedef enum {
    IO_TYPE_GPIO = 0,
    IO_TYPE_EXPANDER,
} panel_io_type_t;

/**
 * @brief SPI line configuration structure
 */
typedef struct {
    panel_io_type_t cs_io_type;                     /*!< IO type of CS line */
    union {
        int cs_gpio_num;                            /*!< GPIO used for CS line */
        esp_io_expander_pin_num_t cs_expander_pin;  /*!< IO expander pin used for CS line */
    };
    panel_io_type_t scl_io_type;                    /*!< IO type of SCL line */
    union {
        int scl_gpio_num;                           /*!< GPIO used for SCL line */
        esp_io_expander_pin_num_t scl_expander_pin; /*!< IO expander pin used for SCL line */
    };
    panel_io_type_t sda_io_type;                    /*!< IO type of SDA line */
    union {
        int sda_gpio_num;                           /*!< GPIO used for SDA line */
        esp_io_expander_pin_num_t sda_expander_pin; /*!< IO expander pin used for SDA line */
    };
    esp_io_expander_handle_t io_expander;           /*!< IO expander handle, set to NULL if not used */
} spi_line_config_t;

/**
 * @brief Panel IO configuration structure
 */
typedef struct {
    spi_line_config_t line_config;  /*!< SPI line configuration */
    uint32_t expect_clk_speed;      /*!< Expected SPI clock speed, in Hz (1 ~ 500000
                                     *   If this value is 0, it will be set to `PANEL_IO_SPI_CLK_MAX` by default
                                     *   The actual frequency may be very different due to the limitation of the software delay */
    uint32_t spi_mode: 2;           /*!< Traditional SPI mode (0 ~ 3) */
    uint32_t lcd_cmd_bytes: 3;      /*!< Bytes of LCD command (1 ~ 4) */
    uint32_t lcd_param_bytes: 3;    /*!< Bytes of LCD parameter (1 ~ 4) */
    struct {
        uint32_t use_dc_bit: 1;             /*!< If this flag is enabled, transmit DC bit at the beginning of every command and data */
        uint32_t dc_zero_on_data: 1;        /*!< If this flag is enabled, DC bit = 0 means transfer data, DC bit = 1 means transfer command */
        uint32_t lsb_first: 1;              /*!< If this flag is enabled, transmit LSB bit first */
        uint32_t cs_high_active: 1;         /*!< If this flag is enabled, CS line is high active */
        uint32_t del_keep_cs_inactive: 1;   /*!< If this flag is enabled, keep CS line inactive even if panel_io is deleted */
    } flags;
} esp_lcd_panel_io_3wire_spi_config_t;

/**
 * @brief Create a new panel IO instance for 3-wire SPI interface (simulate by software)
 *
 * @note  This function uses GPIO or IO expander to simulate SPI interface by software and just supports to write data.
 *        It is only suitable for some applications with low speed SPI interface. (Such as initializing RGB panel)
 *
 * @param[in]  io_config Panel IO configuration
 * @param[out] ret_io    Pointer to return the created panel IO instance
 * @return
 *      - ESP_OK:              Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_NO_MEM:      Failed to allocate memory for panel IO instance
 *      - Others:              Fail
 */
esp_err_t esp_lcd_new_panel_io_3wire_spi(const esp_lcd_panel_io_3wire_spi_config_t *io_config, esp_lcd_panel_io_handle_t *ret_io);

/**
 * @brief Override 3-wire RX timing used by panel_io_rx_param for diagnostic sweeps
 *
 * @param panel_io     The panel_io instance (may be NULL for global override)
 * @param dummy_bytes  Number of dummy bytes to clock before sampling. Use 0xFF to clear override.
 * @param settle_us    Microseconds to delay before dummy clocking. Use 0xFFFFFFFF to clear override.
 */
esp_err_t esp_lcd_3wire_set_rx_timing(esp_lcd_panel_io_handle_t panel_io, uint8_t dummy_bytes, uint32_t settle_us);

/**
 * @brief Override 3-wire RX sampling phase/edge for diagnostics
 *
 * @param panel_io   The panel_io instance (may be NULL for global override)
 * @param invert     -1 to clear override, 0 to force normal phase, 1 to force inverted phase
 */
esp_err_t esp_lcd_3wire_set_rx_phase(esp_lcd_panel_io_handle_t panel_io, int invert);

/**
 * @brief Probe SDA by driving it low briefly then releasing to input and sampling
 *
 * Useful to detect whether the panel ever drives SDA low (versus SDA held high by pull-up or another master).
 *
 * @param panel_io The panel io instance (may be NULL for global / last-used instance)
 * @return ESP_OK on success (and diagnostic logged), error on failure
 */
esp_err_t esp_lcd_3wire_sda_probe(esp_lcd_panel_io_handle_t panel_io);

/**
 * @brief Hold SDA driven HIGH, then LOW, then released and log readbacks.
 *
 * @param panel_io The panel io instance
 * @param hold_ms  Milliseconds to hold each state (HIGH/LOW/RELEASE)
 */
esp_err_t esp_lcd_3wire_sda_hold_test(esp_lcd_panel_io_handle_t panel_io, uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
