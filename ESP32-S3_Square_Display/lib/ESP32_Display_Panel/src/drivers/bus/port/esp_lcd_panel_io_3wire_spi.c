/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_interface.h"

#include "utils/esp_panel_utils_log.h"
#include "esp_utils_helpers.h"
#include "esp_lcd_panel_io_additions.h"

#define LCD_CMD_BYTES_MAX       (sizeof(uint32_t))  // Maximum number of bytes for LCD command
#define LCD_PARAM_BYTES_MAX     (sizeof(uint32_t))  // Maximum number of bytes for LCD parameter

#define DATA_DC_BIT_0           (0)     // DC bit = 0
#define DATA_DC_BIT_1           (1)     // DC bit = 1
#define DATA_NO_DC_BIT          (2)     // No DC bit
#define WRITE_ORDER_LSB_MASK    (0x01)  // Bit mask for LSB first write order
#define WRITE_ORDER_MSB_MASK    (0x80)  // Bit mask for MSB first write order

/**
 * @brief Enumeration of SPI lines
 */
typedef enum {
    CS = 0,
    SCL,
    SDA,
} spi_line_t;

/**
 * @brief Panel IO instance for 3-wire SPI interface
 *
 */
typedef struct {
    esp_lcd_panel_io_t base;                /*!< Base class of generic lcd panel io */
    panel_io_type_t cs_io_type;             /*!< IO type of CS line */
    int cs_io_num;                          /*!< IO used for CS line */
    panel_io_type_t scl_io_type;            /*!< IO type of SCL line */
    int scl_io_num;                         /*!< IO used for SCL line */
    panel_io_type_t sda_io_type;            /*!< IO type of SDA line */
    int sda_io_num;                         /*!< GPIO used for SDA line */
    esp_io_expander_handle_t io_expander;   /*!< IO expander handle, set to NULL if not used */
    uint32_t scl_half_period_us;            /*!< SCL half period in us */
    uint32_t lcd_cmd_bytes: 3;              /*!< Bytes of LCD command (1 ~ 4) */
    uint32_t cmd_dc_bit: 2;                 /*!< DC bit of command */
    uint32_t lcd_param_bytes: 3;            /*!< Bytes of LCD parameter (1 ~ 4) */
    uint32_t param_dc_bit: 2;               /*!< DC bit of parameter */
    uint32_t write_order_mask: 8;           /*!< Bit mask of write order */
    struct {
        uint32_t cs_high_active: 1;         /*!< If this flag is enabled, CS line is high active */
        uint32_t sda_scl_idle_high: 1;      /*!< If this flag is enabled, SDA and SCL line are high when idle */
        uint32_t scl_active_rising_edge: 1; /*!< If this flag is enabled, SCL line is active on rising edge */
        uint32_t del_keep_cs_inactive: 1;   /*!< If this flag is enabled, keep CS line inactive even if panel_io is deleted */
    } flags;
} esp_lcd_panel_io_3wire_spi_t;

static const char *TAG = "lcd_panel.io.3wire_spi";

/* Overrides for diagnostic sweeps. If set to UINT32_MAX the override is disabled. */
static uint32_t g_rx_dummy_bytes_override = UINT32_MAX;
static uint32_t g_rx_settle_us_override = UINT32_MAX;
/* Phase override: -1 = no override, 0 = force normal, 1 = force inverted */
static int g_rx_invert_edge_override = -1;

esp_err_t esp_lcd_3wire_set_rx_timing(esp_lcd_panel_io_handle_t panel_io, uint8_t dummy_bytes, uint32_t settle_us);
esp_err_t esp_lcd_3wire_set_rx_phase(esp_lcd_panel_io_handle_t panel_io, int invert);

static esp_err_t panel_io_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size);
static esp_err_t panel_io_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size);
static esp_err_t panel_io_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size);
static esp_err_t panel_io_del(esp_lcd_panel_io_t *io);
static esp_err_t panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs, void *user_ctx);

static esp_err_t set_line_level(esp_lcd_panel_io_3wire_spi_t *panel_io, spi_line_t line, uint32_t level);
static esp_err_t reset_line_io(esp_lcd_panel_io_3wire_spi_t *panel_io, spi_line_t line);
static void delay_us(uint32_t delay_us);
static esp_err_t spi_write_package(esp_lcd_panel_io_3wire_spi_t *panel_io, bool is_cmd, uint32_t data);

esp_err_t esp_lcd_new_panel_io_3wire_spi(const esp_lcd_panel_io_3wire_spi_config_t *io_config, esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(io_config && ret_io, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");
    ESP_RETURN_ON_FALSE(io_config->expect_clk_speed <= PANEL_IO_SPI_CLK_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid Clock frequency");
    ESP_RETURN_ON_FALSE(io_config->lcd_cmd_bytes > 0 && io_config->lcd_cmd_bytes <= LCD_CMD_BYTES_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "Invalid LCD command bytes");
    ESP_RETURN_ON_FALSE(io_config->lcd_param_bytes > 0 && io_config->lcd_param_bytes <= LCD_PARAM_BYTES_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "Invalid LCD parameter bytes");

    const spi_line_config_t *line_config = &io_config->line_config;
    ESP_RETURN_ON_FALSE(line_config->io_expander || (line_config->cs_io_type == IO_TYPE_GPIO &&
                        line_config->scl_io_type == IO_TYPE_GPIO && line_config->sda_io_type == IO_TYPE_GPIO),
                        ESP_ERR_INVALID_ARG, TAG, "IO Expander handle is required if any IO is not gpio");

    esp_lcd_panel_io_3wire_spi_t *panel_io = calloc(1, sizeof(esp_lcd_panel_io_3wire_spi_t));
    ESP_RETURN_ON_FALSE(panel_io, ESP_ERR_NO_MEM, TAG, "No memory");

    panel_io->cs_io_type = line_config->cs_io_type;
    panel_io->cs_io_num = line_config->cs_gpio_num;
    panel_io->scl_io_type = line_config->scl_io_type;
    panel_io->scl_io_num = line_config->scl_gpio_num;
    panel_io->sda_io_type = line_config->sda_io_type;
    panel_io->sda_io_num = line_config->sda_gpio_num;
    panel_io->io_expander = line_config->io_expander;
    uint32_t expect_clk_speed = io_config->expect_clk_speed ? io_config->expect_clk_speed : PANEL_IO_SPI_CLK_MAX;
    panel_io->scl_half_period_us = 1000000 / (expect_clk_speed * 2);
    panel_io->lcd_cmd_bytes = io_config->lcd_cmd_bytes;
    panel_io->lcd_param_bytes = io_config->lcd_param_bytes;
    if (io_config->flags.use_dc_bit) {
        panel_io->param_dc_bit = io_config->flags.dc_zero_on_data ? DATA_DC_BIT_0 : DATA_DC_BIT_1;
        panel_io->cmd_dc_bit = io_config->flags.dc_zero_on_data ? DATA_DC_BIT_1 : DATA_DC_BIT_0;
    } else {
        panel_io->param_dc_bit = DATA_NO_DC_BIT;
        panel_io->cmd_dc_bit = DATA_NO_DC_BIT;
    }
    panel_io->write_order_mask = io_config->flags.lsb_first ? WRITE_ORDER_LSB_MASK : WRITE_ORDER_MSB_MASK;
    panel_io->flags.cs_high_active = io_config->flags.cs_high_active;
    panel_io->flags.del_keep_cs_inactive = io_config->flags.del_keep_cs_inactive;
    panel_io->flags.sda_scl_idle_high = io_config->spi_mode & 0x1;
    if (panel_io->flags.sda_scl_idle_high) {
        panel_io->flags.scl_active_rising_edge = (io_config->spi_mode & 0x2) ? 1 : 0;
    } else {
        panel_io->flags.scl_active_rising_edge = (io_config->spi_mode & 0x2) ? 0 : 1;
    }

    panel_io->base.rx_param = panel_io_rx_param;
    panel_io->base.tx_param = panel_io_tx_param;
    panel_io->base.tx_color = panel_io_tx_color;
    panel_io->base.del = panel_io_del;
    panel_io->base.register_event_callbacks = panel_io_register_event_callbacks;

    // Get GPIO mask and IO expander pin mask
    esp_err_t ret = ESP_OK;
    int64_t gpio_mask = 0;
    uint32_t expander_pin_mask = 0;
    if (panel_io->cs_io_type == IO_TYPE_GPIO) {
        gpio_mask |= BIT64(panel_io->cs_io_num);
    } else {
        expander_pin_mask |= panel_io->cs_io_num;
    }
    if (panel_io->scl_io_type == IO_TYPE_GPIO) {
        gpio_mask |= BIT64(panel_io->scl_io_num);
    } else {
        expander_pin_mask |= panel_io->scl_io_num;
    }
    if (panel_io->sda_io_type == IO_TYPE_GPIO) {
        gpio_mask |= BIT64(panel_io->sda_io_num);
    } else {
        expander_pin_mask |= panel_io->sda_io_num;
    }
    // Configure GPIOs
    if (gpio_mask) {
        ESP_GOTO_ON_ERROR(gpio_config(&((gpio_config_t) {
            .pin_bit_mask = gpio_mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        })), err, TAG, "GPIO config failed");
    }
    // Configure pins of IO expander
    if (expander_pin_mask) {
        ESP_GOTO_ON_ERROR(esp_io_expander_set_dir(panel_io->io_expander, expander_pin_mask, IO_EXPANDER_OUTPUT), err,
                          TAG, "Expander set dir failed");
    }

    // Set CS, SCL and SDA to idle level
    uint32_t cs_idle_level = panel_io->flags.cs_high_active ? 0 : 1;
    uint32_t sda_scl_idle_level = panel_io->flags.sda_scl_idle_high ? 1 : 0;
    ESP_GOTO_ON_ERROR(set_line_level(panel_io, CS, cs_idle_level), err, TAG, "Set CS level failed");
    ESP_GOTO_ON_ERROR(set_line_level(panel_io, SCL, sda_scl_idle_level), err, TAG, "Set SCL level failed");
    ESP_GOTO_ON_ERROR(set_line_level(panel_io, SDA, sda_scl_idle_level), err, TAG, "Set SDA level failed");

    *ret_io = (esp_lcd_panel_io_handle_t)panel_io;
    return ESP_OK;

err:
    if (gpio_mask) {
        for (int i = 0; i < 64; i++) {
            if (gpio_mask & BIT64(i)) {
                gpio_reset_pin(i);
            }
        }
    }
    if (expander_pin_mask) {
        esp_io_expander_set_dir(panel_io->io_expander, expander_pin_mask, IO_EXPANDER_INPUT);
    }
    free(panel_io);
    return ret;
}

static esp_err_t panel_io_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    esp_lcd_panel_io_3wire_spi_t *panel_io = __containerof(io, esp_lcd_panel_io_3wire_spi_t, base);

    // Send command
    if (lcd_cmd >= 0) {
        ESP_RETURN_ON_ERROR(spi_write_package(panel_io, true, lcd_cmd), TAG, "SPI write package failed");
    }

    // Send parameter
    if (param != NULL && param_size > 0) {
        uint32_t param_data = 0;
        uint32_t param_bytes = panel_io->lcd_param_bytes;
        size_t param_count = param_size / param_bytes;

        // Iteratively get parameter packages and send them one by one
        for (int i = 0; i < param_count; i++) {
            param_data = 0;
            for (int j = 0; j < param_bytes; j++) {
                param_data |= ((uint8_t *)param)[i * param_bytes + j] << (j * 8);
            }
            ESP_RETURN_ON_ERROR(spi_write_package(panel_io, false, param_data), TAG, "SPI write package failed");
        }
    }

    return ESP_OK;
}

static esp_err_t panel_io_del(esp_lcd_panel_io_t *io)
{
    esp_lcd_panel_io_3wire_spi_t *panel_io = __containerof(io, esp_lcd_panel_io_3wire_spi_t, base);

    if (!panel_io->flags.del_keep_cs_inactive) {
        ESP_RETURN_ON_ERROR(reset_line_io(panel_io, CS), TAG, "Reset CS line failed");
    } else {
        ESP_LOGW(TAG, "Delete but keep CS line inactive");
    }
    ESP_RETURN_ON_ERROR(reset_line_io(panel_io, SCL), TAG, "Reset SCL line failed");
    ESP_RETURN_ON_ERROR(reset_line_io(panel_io, SDA), TAG, "Reset SDA line failed");
    free(panel_io);

    return ESP_OK;
}

/**
 * @brief Read parameter bytes from the panel using bit-banged 3-wire SPI
 *
 * This implementation: sends an optional command, configures SDA as input,
 * toggles SCL and samples SDA for each data bit, and returns the bytes in
 * the provided buffer. Works with both GPIO and IO expander SDA implementations.
 */
static esp_err_t panel_io_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    esp_lcd_panel_io_3wire_spi_t *panel_io = __containerof(io, esp_lcd_panel_io_3wire_spi_t, base);
    ESP_RETURN_ON_FALSE(param && param_size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid param buffer");

    // If a command is provided, send it first
    if (lcd_cmd >= 0) {
        ESP_RETURN_ON_ERROR(spi_write_package(panel_io, true, lcd_cmd), TAG, "SPI write package failed");
    }

    uint32_t cs_idle_level = panel_io->flags.cs_high_active ? 0 : 1;
    uint32_t sda_scl_idle_level = panel_io->flags.sda_scl_idle_high ? 1 : 0;
    bool scl_active_rising = panel_io->flags.scl_active_rising_edge ? true : false;
    if (g_rx_invert_edge_override == 1) {
        scl_active_rising = !scl_active_rising; /* Forced invert */
    } else if (g_rx_invert_edge_override == 0) {
        scl_active_rising = panel_io->flags.scl_active_rising_edge; /* Forced normal */
    }
    uint32_t scl_active_befor_level = scl_active_rising ? 0 : 1;
    uint32_t scl_active_after_level = !scl_active_befor_level;
    uint32_t time_us = panel_io->scl_half_period_us;

    // Configure SDA to input so we can sample data
    if (panel_io->sda_io_type == IO_TYPE_GPIO) {
        ESP_RETURN_ON_ERROR(gpio_set_direction(panel_io->sda_io_num, GPIO_MODE_INPUT), TAG, "Set SDA input failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(panel_io->io_expander, BIT64(panel_io->sda_io_num), IO_EXPANDER_INPUT),
                            TAG, "Expander set SDA input failed");
    }

    // Activate CS and prepare SCL
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, CS, !cs_idle_level), TAG, "Set CS level failed");
    delay_us(time_us);
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_befor_level), TAG, "Set SCL level failed");

    // Small settle delay so controller can prepare read data (some panels require a few us)
    uint32_t settle = (g_rx_settle_us_override != UINT32_MAX) ? g_rx_settle_us_override : ((time_us < 20) ? 20 : 0);
    if (settle) delay_us(settle);

    // Perform dummy byte(s) of clocking to allow device to prepare read data (many ST devices expect dummy cycles)
    uint32_t dummy_count = (g_rx_dummy_bytes_override != UINT32_MAX) ? g_rx_dummy_bytes_override : panel_io->lcd_param_bytes;
    for (uint32_t d = 0; d < dummy_count; ++d) {
        for (int bit = 0; bit < 8; ++bit) {
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_after_level), TAG, "Set SCL level failed");
            if (time_us >= 2) delay_us(time_us / 2);
            // no sampling during dummy
            if (time_us >= 2) delay_us(time_us / 2);
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_befor_level), TAG, "Set SCL level failed");
        }
    }

    uint8_t *out = (uint8_t *)param;
    for (size_t i = 0; i < param_size; ++i) {
        uint8_t val = 0;
        for (int bit = 0; bit < 8; ++bit) {
            // Drive SCL to active edge
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_after_level), TAG, "Set SCL level failed");
            // Wait half the clock period and sample the SDA line
            if (time_us >= 2) {
                delay_us(time_us / 2);
            } else {
                esp_rom_delay_us(1);
            }

            int level = 0;
            if (panel_io->sda_io_type == IO_TYPE_GPIO) {
                level = gpio_get_level(panel_io->sda_io_num);
            } else {
                uint32_t level_mask = 0;
                ESP_RETURN_ON_ERROR(esp_io_expander_get_level(panel_io->io_expander, BIT64(panel_io->sda_io_num), &level_mask),
                                    TAG, "Expander get level failed");
                level = (level_mask & BIT64(panel_io->sda_io_num)) ? 1 : 0;
            }

            if (panel_io->write_order_mask == WRITE_ORDER_LSB_MASK) {
                val |= (level & 0x1) << bit;
            } else {
                val = (val << 1) | (level & 0x1);
            }

            // Finish bit period by restoring SCL to before level
            if (time_us >= 2) {
                delay_us(time_us / 2);
            }
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_befor_level), TAG, "Set SCL level failed");
            // Small settle before next bit
            if (time_us >= 2) delay_us(1);
        }
        out[i] = val;
    }

    ESP_LOGI(TAG, "Read bytes: %02x %02x", param_size > 0 ? out[0] : 0, param_size > 1 ? out[1] : 0);

    // Finish read: restore line idle levels and deactivate CS
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, sda_scl_idle_level), TAG, "Set SCL level failed");
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SDA, sda_scl_idle_level), TAG, "Set SDA level failed");
    delay_us(time_us);
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, CS, cs_idle_level), TAG, "Set CS level failed");

    // Restore SDA to output so the IO behaves as before
    if (panel_io->sda_io_type == IO_TYPE_GPIO) {
        ESP_RETURN_ON_ERROR(gpio_set_direction(panel_io->sda_io_num, GPIO_MODE_OUTPUT), TAG, "Restore SDA output failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(panel_io->io_expander, BIT64(panel_io->sda_io_num), IO_EXPANDER_OUTPUT),
                            TAG, "Expander restore SDA output failed");
    }

    return ESP_OK;
}

esp_err_t esp_lcd_3wire_set_rx_timing(esp_lcd_panel_io_handle_t panel_io, uint8_t dummy_bytes, uint32_t settle_us)
{
    (void)panel_io;
    if (dummy_bytes == 0xFF) {
        g_rx_dummy_bytes_override = UINT32_MAX;
    } else {
        g_rx_dummy_bytes_override = dummy_bytes;
    }
    if (settle_us == 0xFFFFFFFF) {
        g_rx_settle_us_override = UINT32_MAX;
    } else {
        g_rx_settle_us_override = settle_us;
    }
    ESP_LOGI(TAG, "Set 3wire RX timing override: dummy=%u settle_us=%u", (unsigned)(g_rx_dummy_bytes_override==UINT32_MAX?0xFFFF:g_rx_dummy_bytes_override), (unsigned)(g_rx_settle_us_override==UINT32_MAX?0xFFFFFFFF:g_rx_settle_us_override));
    return ESP_OK;
}

esp_err_t esp_lcd_3wire_set_rx_phase(esp_lcd_panel_io_handle_t panel_io, int invert)
{
    (void)panel_io;
    if (invert < -1 || invert > 1) return ESP_ERR_INVALID_ARG;
    g_rx_invert_edge_override = invert;
    ESP_LOGI(TAG, "Set 3wire RX phase override: invert=%d", g_rx_invert_edge_override);
    return ESP_OK;
}

// Probe SDA behaviour: drive SDA low for a short time, release to input and sample
// Return ESP_OK and log samples so caller can inspect whether the panel ever drives SDA low
esp_err_t esp_lcd_3wire_sda_probe(esp_lcd_panel_io_handle_t panel_io)
{
    esp_lcd_panel_io_3wire_spi_t *panel = __containerof(panel_io, esp_lcd_panel_io_3wire_spi_t, base);
    if (!panel) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "SDA probe: driving SDA low briefly then sampling");

    // Configure SDA as output and drive low
    if (panel->sda_io_type == IO_TYPE_GPIO) {
        gpio_set_direction(panel->sda_io_num, GPIO_MODE_OUTPUT);
        set_line_level(panel, SDA, 0);
    } else {
        esp_io_expander_set_dir(panel->io_expander, BIT64(panel->sda_io_num), IO_EXPANDER_OUTPUT);
        set_line_level(panel, SDA, 0);
    }

    // Hold it low briefly so any external pull-up/master sees it
    vTaskDelay(pdMS_TO_TICKS(5));

    // Release SDA to input so panel can drive it
    if (panel->sda_io_type == IO_TYPE_GPIO) {
        gpio_set_direction(panel->sda_io_num, GPIO_MODE_INPUT);
    } else {
        esp_io_expander_set_dir(panel->io_expander, BIT64(panel->sda_io_num), IO_EXPANDER_INPUT);
    }

    // Small settle then sample multiple times
    const int samples = 20;
    const int sample_interval_us = 5; // 5us between samples
    int any_low = 0;
    char buf[128];
    int pos = 0;
    for (int i = 0; i < samples; ++i) {
        uint32_t level = 0;
        if (panel->sda_io_type == IO_TYPE_GPIO) {
            level = gpio_get_level(panel->sda_io_num);
        } else {
            uint32_t mask = 0;
            esp_io_expander_get_level(panel->io_expander, BIT64(panel->sda_io_num), &mask);
            level = (mask & BIT64(panel->sda_io_num)) ? 1 : 0;
        }
        if (level == 0) any_low = 1;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%u", (unsigned)level);
        if (i + 1 < samples) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        esp_rom_delay_us(sample_interval_us);
    }

    ESP_LOGI(TAG, "SDA probe samples: [%s] any_low=%d", buf, any_low);

    // Additional diagnostic: if SDA is a GPIO we can enable the internal pull‑down and sample
    if (panel->sda_io_type == IO_TYPE_GPIO) {
        // Enable internal pull-down briefly and sample to see whether SDA is actively driven high
        gpio_set_pull_mode(panel->sda_io_num, GPIO_PULLDOWN_ONLY);
        vTaskDelay(pdMS_TO_TICKS(1)); // settle

        const int p_samples = 20;
        const int p_interval_us = 5;
        int any_low_pd = 0;
        char pbuf[128];
        int ppos = 0;
        for (int i = 0; i < p_samples; ++i) {
            int level = gpio_get_level(panel->sda_io_num);
            if (level == 0) any_low_pd = 1;
            ppos += snprintf(pbuf + ppos, sizeof(pbuf) - ppos, "%u", (unsigned)level);
            if (i + 1 < p_samples) ppos += snprintf(pbuf + ppos, sizeof(pbuf) - ppos, ",");
            esp_rom_delay_us(p_interval_us);
        }
        ESP_LOGI(TAG, "SDA pull-down samples: [%s] any_low=%d", pbuf, any_low_pd);

        // Restore pull to floating (no pull) to match original behavior
        gpio_set_pull_mode(panel->sda_io_num, GPIO_FLOATING);

        // Stronger diagnostic: drive SDA as output LOW and read back to detect external drive
        gpio_set_direction(panel->sda_io_num, GPIO_MODE_OUTPUT);
        set_line_level(panel, SDA, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        int read_back_low = gpio_get_level(panel->sda_io_num);
        ESP_LOGI(TAG, "SDA drive-test: MCU output LOW -> readback=%d", read_back_low);

        // Now drive SDA HIGH and read back
        set_line_level(panel, SDA, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        int read_back_high = gpio_get_level(panel->sda_io_num);
        ESP_LOGI(TAG, "SDA drive-test: MCU output HIGH -> readback=%d", read_back_high);

        // Finally, release (set to input) so panel may drive it if needed
        gpio_set_direction(panel->sda_io_num, GPIO_MODE_INPUT);

    } else {
        ESP_LOGI(TAG, "SDA pull-down sampling: not supported for expander IO; skipping");
    }

    // Restore SDA to output low as driver expects
    if (panel->sda_io_type == IO_TYPE_GPIO) {
        gpio_set_direction(panel->sda_io_num, GPIO_MODE_OUTPUT);
        set_line_level(panel, SDA, panel->flags.sda_scl_idle_high ? 1 : 0);
    } else {
        set_line_level(panel, SDA, panel->flags.sda_scl_idle_high ? 1 : 0);
        esp_io_expander_set_dir(panel->io_expander, BIT64(panel->sda_io_num), IO_EXPANDER_OUTPUT);
    }

    return ESP_OK;
}

// SDA hold test: Drive SDA HIGH for hold_ms, sample readbacks; drive LOW for hold_ms, sample; release to input and sample.
esp_err_t esp_lcd_3wire_sda_hold_test(esp_lcd_panel_io_handle_t panel_io, uint32_t hold_ms)
{
    esp_lcd_panel_io_3wire_spi_t *panel = __containerof(panel_io, esp_lcd_panel_io_3wire_spi_t, base);
    if (!panel) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "SDA hold test: hold_ms=%u ms (HIGH then LOW then RELEASE)", (unsigned)hold_ms);

    if (panel->sda_io_type != IO_TYPE_GPIO) {
        ESP_LOGI(TAG, "SDA hold test: expander SDA not supported; skipping");
        return ESP_FAIL;
    }

    const int samples = 10;
    uint32_t interval_ms = (hold_ms > 0) ? (hold_ms / samples) : 50;

    // Drive HIGH
    gpio_set_direction(panel->sda_io_num, GPIO_MODE_OUTPUT);
    set_line_level(panel, SDA, 1);
    for (int i = 0; i < samples; ++i) {
        int level = gpio_get_level(panel->sda_io_num);
        ESP_LOGI(TAG, "SDA hold HIGH sample %d/%d -> %d", i+1, samples, level);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    // Drive LOW
    set_line_level(panel, SDA, 0);
    for (int i = 0; i < samples; ++i) {
        int level = gpio_get_level(panel->sda_io_num);
        ESP_LOGI(TAG, "SDA hold LOW sample %d/%d -> %d", i+1, samples, level);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    // Release to input and sample
    gpio_set_direction(panel->sda_io_num, GPIO_MODE_INPUT);
    for (int i = 0; i < samples; ++i) {
        int level = gpio_get_level(panel->sda_io_num);
        ESP_LOGI(TAG, "SDA release sample %d/%d -> %d", i+1, samples, level);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    // Restore SDA to output idle
    gpio_set_direction(panel->sda_io_num, GPIO_MODE_OUTPUT);
    set_line_level(panel, SDA, panel->flags.sda_scl_idle_high ? 1 : 0);

    return ESP_OK;
} 

/**
 * @brief This function is not ready and only for compatibility
 */
static esp_err_t panel_io_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)
{
    ESP_LOGE(TAG, "Tx color is not supported");

    return ESP_FAIL;
}

/**
 * @brief This function is not ready and only for compatibility
 */
static esp_err_t panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_io_callbacks_t *cbs, void *user_ctx)
{
    ESP_LOGE(TAG, "Register event callbacks is not supported");

    return ESP_FAIL;
}

/**
 * @brief Set the level of specified line.
 *
 * This function can use GPIO or IO expander according to the type of line
 *
 * @param[in]  panel_io Pointer to panel IO instance
 * @param[in]  line     Target line
 * @param[out] level    Target level, 0 - Low, 1 - High
 *
 * @return
 *      - ESP_OK:              Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - Others:              Fail
 */
static esp_err_t set_line_level(esp_lcd_panel_io_3wire_spi_t *panel_io, spi_line_t line, uint32_t level)
{
    panel_io_type_t line_type = IO_TYPE_GPIO;
    int line_io = 0;
    switch (line) {
    case CS:
        line_type = panel_io->cs_io_type;
        line_io = panel_io->cs_io_num;
        break;
    case SCL:
        line_type = panel_io->scl_io_type;
        line_io = panel_io->scl_io_num;
        break;
    case SDA:
        line_type = panel_io->sda_io_type;
        line_io = panel_io->sda_io_num;
        break;
    default:
        break;
    }

    if (line_type == IO_TYPE_GPIO) {
        return gpio_set_level(line_io, level);
    } else {
        return esp_io_expander_set_level(panel_io->io_expander, (esp_io_expander_pin_num_t)line_io, level != 0);
    }
}

/**
 * @brief Reset the IO of specified line
 *
 * This function can use GPIO or IO expander according to the type of line
 *
 * @param[in]  panel_io Pointer to panel IO instance
 * @param[in]  line     Target line
 *
 * @return
 *      - ESP_OK:              Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - Others:              Fail
 */
static esp_err_t reset_line_io(esp_lcd_panel_io_3wire_spi_t *panel_io, spi_line_t line)
{
    panel_io_type_t line_type = IO_TYPE_GPIO;
    int line_io = 0;
    switch (line) {
    case CS:
        line_type = panel_io->cs_io_type;
        line_io = panel_io->cs_io_num;
        break;
    case SCL:
        line_type = panel_io->scl_io_type;
        line_io = panel_io->scl_io_num;
        break;
    case SDA:
        line_type = panel_io->sda_io_type;
        line_io = panel_io->sda_io_num;
        break;
    default:
        break;
    }

    if (line_type == IO_TYPE_GPIO) {
        return gpio_reset_pin(line_io);
    } else {
        return esp_io_expander_set_dir(panel_io->io_expander, (esp_io_expander_pin_num_t)line_io, IO_EXPANDER_INPUT);
    }
}

/**
 * @brief Delay for given microseconds
 *
 * @note  This function uses `esp_rom_delay_us()` for delays < 1000us and `vTaskDelay()` for longer delays.
 *
 * @param[in] delay_us Delay time in microseconds
 *
 */
static void delay_us(uint32_t delay_us)
{
    if (delay_us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS(delay_us / 1000));
    } else {
        esp_rom_delay_us(delay_us);
    }
}

/**
 * @brief Write one byte to LCD panel
 *
 * @param[in] panel_io Pointer to panel IO instance
 * @param[in] dc_bit   DC bit
 * @param[in] data     8-bit data to write
 *
 * @return
 *      - ESP_OK:              Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - Others:              Fail
 */
static esp_err_t spi_write_byte(esp_lcd_panel_io_3wire_spi_t *panel_io, int dc_bit, uint8_t data)
{
    uint16_t data_temp = data;
    uint8_t data_bits = (dc_bit != DATA_NO_DC_BIT) ? 9 : 8;
    uint16_t write_order_mask = panel_io->write_order_mask;
    uint32_t scl_active_befor_level = panel_io->flags.scl_active_rising_edge ? 0 : 1;
    uint32_t scl_active_after_level = !scl_active_befor_level;
    uint32_t scl_half_period_us = panel_io->scl_half_period_us;

    for (uint8_t i = 0; i < data_bits; i++) {
        // Send DC bit first
        if (data_bits == 9 && i == 0) {
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SDA, dc_bit), TAG, "Set SDA level failed");
        } else { // Then send data bit
            // SDA set to data bit
            ESP_RETURN_ON_ERROR(set_line_level(panel_io, SDA, data_temp & write_order_mask), TAG, "Set SDA level failed");
            // Get next bit
            data_temp = (write_order_mask == WRITE_ORDER_LSB_MASK) ? data_temp >> 1 : data_temp << 1;
        }
        // Generate SCL active edge
        ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_befor_level), TAG, "Set SCL level failed");
        delay_us(scl_half_period_us);
        ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_after_level), TAG, "Set SCL level failed");
        delay_us(scl_half_period_us);
    }

    return ESP_OK;
}

/**
 * @brief Write a package of data to LCD panel in big-endian order
 *
 * @param[in] panel_io Pointer to panel IO instance
 * @param[in] is_cmd   True for command, false for data
 * @param[in] data     Data to write, with
 *
 * @return
 *      - ESP_OK:              Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - Others:              Fail
 */
static esp_err_t spi_write_package(esp_lcd_panel_io_3wire_spi_t *panel_io, bool is_cmd, uint32_t data)
{
    uint32_t data_bytes = is_cmd ? panel_io->lcd_cmd_bytes : panel_io->lcd_param_bytes;
    uint32_t cs_idle_level = panel_io->flags.cs_high_active ? 0 : 1;
    uint32_t sda_scl_idle_level = panel_io->flags.sda_scl_idle_high ? 1 : 0;
    uint32_t scl_active_befor_level = panel_io->flags.scl_active_rising_edge ? 0 : 1;
    uint32_t time_us = panel_io->scl_half_period_us;
    // Swap command bytes order due to different endianness
    uint32_t swap_data = SPI_SWAP_DATA_TX(data, data_bytes * 8);
    int data_dc_bit = is_cmd ? panel_io->cmd_dc_bit : panel_io->param_dc_bit;

    // CS active
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, CS, !cs_idle_level), TAG, "Set CS level failed");
    delay_us(time_us);
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, scl_active_befor_level), TAG, "Set SCL level failed");
    // Send data byte by byte
    for (int i = 0; i < data_bytes; i++) {
        // Only set DC bit for the first byte
        if (i == 0) {
            ESP_RETURN_ON_ERROR(spi_write_byte(panel_io, data_dc_bit, swap_data & 0xff), TAG, "SPI write byte failed");
        } else {
            ESP_RETURN_ON_ERROR(spi_write_byte(panel_io, DATA_NO_DC_BIT, swap_data & 0xff), TAG, "SPI write byte failed");
        }
        swap_data >>= 8;
    }
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SCL, sda_scl_idle_level), TAG, "Set SCL level failed");
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, SDA, sda_scl_idle_level), TAG, "Set SDA level failed");
    delay_us(time_us);
    // CS inactive
    ESP_RETURN_ON_ERROR(set_line_level(panel_io, CS, cs_idle_level), TAG, "Set CS level failed");
    delay_us(time_us);

    return ESP_OK;
}
