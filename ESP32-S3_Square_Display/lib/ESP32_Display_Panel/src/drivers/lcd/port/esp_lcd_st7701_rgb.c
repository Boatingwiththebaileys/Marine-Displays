/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../esp_panel_lcd_conf_internal.h"
#if ESP_PANEL_DRIVERS_LCD_ENABLE_ST7701

#include "soc/soc_caps.h"

#if SOC_LCD_RGB_SUPPORTED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_st7701_interface.h"

#include "utils/esp_panel_utils_log.h"
#include "esp_utils_helpers.h"
#include "esp_panel_lcd_vendor_types.h"
#include "esp_lcd_st7789.h"

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    uint8_t madctl_val; // Save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // Save current value of LCD_CMD_COLMOD register
    const esp_panel_lcd_vendor_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int mirror_by_cmd: 1;
        unsigned int enable_io_multiplex: 1;
        unsigned int display_on_off_use_cmd: 1;
        unsigned int reset_level: 1;
    } flags;
    // To save the original functions of RGB panel
    esp_err_t (*init)(esp_lcd_panel_t *panel);
    esp_err_t (*del)(esp_lcd_panel_t *panel);
    esp_err_t (*reset)(esp_lcd_panel_t *panel);
    esp_err_t (*mirror)(esp_lcd_panel_t *panel, bool x_axis, bool y_axis);
    esp_err_t (*disp_on_off)(esp_lcd_panel_t *panel, bool on_off);
} st7701_panel_t;

static const char *TAG = "st7701_rgb";

static esp_err_t panel_st7701_send_init_cmds(st7701_panel_t *st7701);

static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st7701_disp_on_off(esp_lcd_panel_t *panel, bool off);

esp_err_t esp_lcd_new_panel_st7701_rgb(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                       esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    esp_panel_lcd_vendor_config_t *vendor_config = (esp_panel_lcd_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor_config && vendor_config->rgb_config, ESP_ERR_INVALID_ARG, TAG, "`verndor_config` and `rgb_config` are necessary");
    ESP_RETURN_ON_FALSE(!vendor_config->flags.enable_io_multiplex || !vendor_config->flags.mirror_by_cmd,
                        ESP_ERR_INVALID_ARG, TAG, "`mirror_by_cmd` and `enable_io_multiplex` cannot work together");

    esp_err_t ret = ESP_OK;
    st7701_panel_t *st7701 = (st7701_panel_t *)calloc(1, sizeof(st7701_panel_t));
    ESP_RETURN_ON_FALSE(st7701, ESP_ERR_NO_MEM, TAG, "no mem for st7701 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        st7701->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        st7701->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color element order");
        break;
    }

    st7701->colmod_val = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        st7701->colmod_val = 0x50;
        break;
    case 18: // RGB666
        st7701->colmod_val = 0x60;
        break;
    case 24: // RGB888
        st7701->colmod_val = 0x70;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    st7701->io = io;
    st7701->init_cmds = vendor_config->init_cmds;
    st7701->init_cmds_size = vendor_config->init_cmds_size;
    st7701->reset_gpio_num = panel_dev_config->reset_gpio_num;
    st7701->flags.mirror_by_cmd = vendor_config->flags.mirror_by_cmd;
    st7701->flags.display_on_off_use_cmd = (vendor_config->rgb_config->disp_gpio_num >= 0) ? 0 : 1;
    st7701->flags.enable_io_multiplex = vendor_config->flags.enable_io_multiplex;
    st7701->flags.reset_level = panel_dev_config->flags.reset_active_high;

    ESP_LOGI(TAG, "st7701 config: io=%p init_cmds=%p size=%u flags: enable_io_multiplex=%d mirror_by_cmd=%d display_on_off_use_cmd=%d reset_level=%d",
             st7701->io, st7701->init_cmds, st7701->init_cmds_size, st7701->flags.enable_io_multiplex, st7701->flags.mirror_by_cmd, st7701->flags.display_on_off_use_cmd, st7701->flags.reset_level);
    ESP_LOGI(TAG, "[ST7701-DRV] st7701 config: io=%p init_cmds=%p size=%u flags: enable_io_multiplex=%d mirror_by_cmd=%d display_on_off_use_cmd=%d reset_level=%d",
             st7701->io, st7701->init_cmds, st7701->init_cmds_size, st7701->flags.enable_io_multiplex, st7701->flags.mirror_by_cmd, st7701->flags.display_on_off_use_cmd, st7701->flags.reset_level);

    if (st7701->flags.enable_io_multiplex) {
        if (st7701->reset_gpio_num >= 0) {  // Perform hardware reset
            gpio_set_level(st7701->reset_gpio_num, st7701->flags.reset_level);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(st7701->reset_gpio_num, !st7701->flags.reset_level);
        } else { // Perform software reset
            ESP_GOTO_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), err, TAG, "send command failed");
        }
        vTaskDelay(pdMS_TO_TICKS(120));

        /**
         * In order to enable the 3-wire SPI interface pins (such as SDA and SCK) to share other pins of the RGB interface
         * (such as HSYNC) and save GPIOs, we need to send LCD initialization commands via the 3-wire SPI interface before
         * `esp_lcd_new_rgb_panel()` is called.
         */
        ESP_LOGI(TAG, "enable_io_multiplex is set: sending vendor init sequence via 3-wire SPI");
        ESP_GOTO_ON_ERROR(panel_st7701_send_init_cmds(st7701), err, TAG, "send init commands failed");
        // After sending the initialization commands, the 3-wire SPI interface can be deleted
        ESP_GOTO_ON_ERROR(esp_lcd_panel_io_del(io), err, TAG, "delete panel IO failed");
        st7701->io = NULL;
        ESP_LOGD(TAG, "delete panel IO");
        ESP_LOGI(TAG, "panel IO deleted: 3-wire SPI pins released (SD must be initialized before panel takeover to avoid shared SPI conflicts)");
    }

    // Create RGB panel
    ESP_GOTO_ON_ERROR(esp_lcd_new_rgb_panel(vendor_config->rgb_config, ret_panel), err, TAG, "create RGB panel failed");
    ESP_LOGD(TAG, "new RGB panel @%p", ret_panel);

    // Save the original functions of RGB panel
    st7701->init = (*ret_panel)->init;
    st7701->del = (*ret_panel)->del;
    st7701->reset = (*ret_panel)->reset;
    st7701->mirror = (*ret_panel)->mirror;
    st7701->disp_on_off = (*ret_panel)->disp_on_off;
    // Overwrite the functions of RGB panel
    (*ret_panel)->init = panel_st7701_init;
    (*ret_panel)->del = panel_st7701_del;
    (*ret_panel)->reset = panel_st7701_reset;
    (*ret_panel)->mirror = panel_st7701_mirror;
    (*ret_panel)->disp_on_off = panel_st7701_disp_on_off;
    (*ret_panel)->user_data = st7701;
    ESP_LOGD(TAG, "new st7701 panel @%p", st7701);

    return ESP_OK;

err:
    if (st7701) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(st7701);
    }
    return ret;
}

static const esp_panel_lcd_vendor_init_cmd_t vendor_specific_init_default[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},

    {0xC0, (uint8_t []){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t []){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t []){0x31, 0x05}, 2, 0},
    {0xCD, (uint8_t []){0x08}, 1, 0},

    {0xB0, (uint8_t []){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t []){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},

    // PAGE1
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t []){0x60}, 1, 0}, // Vop=4.7375v
    {0xB1, (uint8_t []){0x32}, 1, 0}, // VCOM=32
    {0xB2, (uint8_t []){0x07}, 1, 0}, // VGH
    {0xB3, (uint8_t []){0x80}, 1, 0},
    {0xB5, (uint8_t []){0x49}, 1, 0}, // VGL=-10.17v
    {0xB7, (uint8_t []){0x85}, 1, 0},
    {0xB8, (uint8_t []){0x21}, 1, 0}, // AVDD=6.6 & AVCL=-4.6
    {0xC1, (uint8_t []){0x78}, 1, 0},
    {0xC2, (uint8_t []){0x78}, 1, 0},

    {0xE0, (uint8_t []){0x00, 0x1B, 0x02}, 3, 0},

    {0xE1, (uint8_t []){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},

    {0xE2, (uint8_t []){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},

    {0xE3, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},

    {0xE4, (uint8_t []){0x44, 0x44}, 2, 0},

    {0xE5, (uint8_t []){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},

    {0xE6, (uint8_t []){0x00, 0x00, 0x11, 0x11}, 4, 0},

    {0xE7, (uint8_t []){0x44, 0x44}, 2, 0},

    {0xE8, (uint8_t []){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},

    {0xEB, (uint8_t []){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},

    {0xEC, (uint8_t []){0x3C, 0x00}, 2, 0},

    {0xED, (uint8_t []){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},

    //-----------VAP & VAN---------------
    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},

    {0xE5, (uint8_t []){0xE4}, 1, 0},

    {0xFF, (uint8_t []){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    {0x21, (uint8_t []){0x00}, 0, 0},   // 0x20 normal, 0x21 IPS
    {0x3A, (uint8_t []){0x60}, 1, 0}, // 0x70 RGB888, 0x60 RGB666, 0x50 RGB565

    {0x11, (uint8_t []){0x00}, 0, 120}, // Sleep Out
    {0x29, (uint8_t []){0x00}, 0, 0}, // Display On
};

static esp_err_t panel_st7701_send_init_cmds(st7701_panel_t *st7701)
{
    esp_lcd_panel_io_handle_t io = st7701->io;
    const esp_panel_lcd_vendor_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool is_command2_disable = true;
    bool is_cmd_overwritten = false;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_CND2BKxSEL, (uint8_t []) {
        ST7701_CMD_BKxSEL_BYTE0, ST7701_CMD_BKxSEL_BYTE1, ST7701_CMD_BKxSEL_BYTE2, ST7701_CMD_BKxSEL_BYTE3, 0x00
    }, 5), TAG, "Write cmd failed");
    // Set color format
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t []) {
        st7701->madctl_val
    }, 1), TAG, "Write cmd failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t []) {
        st7701->colmod_val
    }, 1), TAG, "Write cmd failed");

    // vendor specific initialization, it can be different between manufacturers
    // should consult the LCD supplier for initialization sequence code
    if (st7701->init_cmds) {
        init_cmds = st7701->init_cmds;
        init_cmds_size = st7701->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(esp_panel_lcd_vendor_init_cmd_t);
    }

    ESP_LOGI(TAG, "panel_st7701_send_init_cmds: init_cmds=%p size=%u enable_cmd2_disable=%d", init_cmds, init_cmds_size, (int)is_command2_disable);
    ESP_LOGI(TAG, "[ST7701-DRV] panel_st7701_send_init_cmds: init_cmds=%p size=%u enable_cmd2_disable=%d", init_cmds, init_cmds_size, (int)is_command2_disable);
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command has been used or conflicts with the internal only when command2 is disable
        if (is_command2_disable && (init_cmds[i].data_bytes > 0)) {
            switch (init_cmds[i].cmd) {
            case LCD_CMD_MADCTL:
                is_cmd_overwritten = true;
                st7701->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            case LCD_CMD_COLMOD:
                is_cmd_overwritten = true;
                st7701->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
                break;
            default:
                is_cmd_overwritten = false;
                break;
            }

            if (is_cmd_overwritten) {
                is_cmd_overwritten = false;
                ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence",
                         init_cmds[i].cmd);
            }
        }

        // Log the command we are about to send (for diagnostics)
        if (init_cmds[i].data_bytes > 0) {
            char dbuf[128] = {0};
            int p = 0;
            for (int b = 0; b < init_cmds[i].data_bytes && p + 4 < (int)sizeof(dbuf); ++b) {
                p += snprintf(&dbuf[p], sizeof(dbuf) - p, "%02X ", ((uint8_t *)init_cmds[i].data)[b]);
            }
            ESP_LOGD(TAG, "INIT CMD[%d]: CMD=0x%02X DATA(%u)= %s, delay=%ums", i, init_cmds[i].cmd, init_cmds[i].data_bytes, dbuf, init_cmds[i].delay_ms);
        } else {
            ESP_LOGD(TAG, "INIT CMD[%d]: CMD=0x%02X (no data) delay=%ums", i, init_cmds[i].cmd, init_cmds[i].delay_ms);
        }

        // Send command
        ESP_LOGD(TAG, "[ST7701-DRV] Sending INIT CMD[%d]: CMD=0x%02X data_bytes=%u delay=%ums", i, init_cmds[i].cmd, init_cmds[i].data_bytes, init_cmds[i].delay_ms);
        if (init_cmds[i].data_bytes > 0) {
            char dbuf[128] = {0};
            int p = 0;
            for (int b = 0; b < init_cmds[i].data_bytes && p + 4 < (int)sizeof(dbuf); ++b) {
                p += snprintf(&dbuf[p], sizeof(dbuf) - p, "%02X ", ((uint8_t *)init_cmds[i].data)[b]);
            }
            ESP_LOGD(TAG, "[ST7701-DRV] INIT CMD DATA[%d]: %s", i, dbuf);
        }
        esp_err_t txr = esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes);
        if (txr != ESP_OK) {
            ESP_LOGE(TAG, "send command failed at index %d: cmd=0x%02X -> %s", i, init_cmds[i].cmd, esp_err_to_name(txr));
            ESP_LOGE(TAG, "[ST7701-DRV] send command failed at index %d: cmd=0x%02X -> %s", i, init_cmds[i].cmd, esp_err_to_name(txr));
            return txr;
        }

        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));

        // Check if the current cmd is the command2 disable cmd
        if ((init_cmds[i].cmd == ST7701_CMD_CND2BKxSEL) && (init_cmds[i].data_bytes > 4)) {
            is_command2_disable = !(((uint8_t *)init_cmds[i].data)[4] & ST7701_CMD_CN2_BIT);
        }
    }

    // Try reading back some params to verify controller state
    uint8_t rb = 0;
    esp_err_t rr = esp_lcd_panel_io_rx_param(io, LCD_CMD_MADCTL, &rb, 1);
    if (rr == ESP_OK) {
        ESP_LOGI(TAG, "Readback: MADCTL=0x%02X", rb);
        ESP_LOGI(TAG, "[ST7701-DRV] Readback: MADCTL=0x%02X", rb);
    } else {
        ESP_LOGW(TAG, "Readback: MADCTL read failed: %s", esp_err_to_name(rr));
        ESP_LOGE(TAG, "[ST7701-DRV] Readback: MADCTL read failed: %s", esp_err_to_name(rr));
    }
    rr = esp_lcd_panel_io_rx_param(io, LCD_CMD_COLMOD, &rb, 1);
    if (rr == ESP_OK) {
        ESP_LOGI(TAG, "Readback: COLMOD=0x%02X", rb);
        ESP_LOGI(TAG, "[ST7701-DRV] Readback: COLMOD=0x%02X", rb);
    } else {
        ESP_LOGW(TAG, "Readback: COLMOD read failed: %s", esp_err_to_name(rr));
        ESP_LOGE(TAG, "[ST7701-DRV] Readback: COLMOD read failed: %s", esp_err_to_name(rr));
    }

    ESP_LOGI(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_st7701_init(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;

    // Log whether we're sending vendor init via 3-wire SPI earlier or during init
    if (st7701->flags.enable_io_multiplex) {
        ESP_LOGI(TAG, "panel_st7701_init: enable_io_multiplex=1 -> vendor init should have been sent earlier via 3-wire SPI");
    } else {
        if (st7701->init_cmds && st7701->init_cmds_size > 0) {
            ESP_LOGI(TAG, "panel_st7701_init: enable_io_multiplex=0 -> sending vendor init now");
            ESP_RETURN_ON_ERROR(panel_st7701_send_init_cmds(st7701), TAG, "send init commands failed");
        } else {
            // No vendor init commands configured (e.g., SWSPI vendor init was performed by application).
            ESP_LOGI(TAG, "panel_st7701_init: enable_io_multiplex=0 -> no vendor init configured; skipping driver init to match SWSPI demo flow");
        }
    }
    // Init RGB panel
    ESP_RETURN_ON_ERROR(st7701->init(panel), TAG, "init RGB panel failed");

    return ESP_OK;
}

static esp_err_t panel_st7701_del(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;

    if (st7701->reset_gpio_num >= 0) {
        gpio_reset_pin(st7701->reset_gpio_num);
    }
    // Delete RGB panel
    st7701->del(panel);
    free(st7701);
    ESP_LOGD(TAG, "del st7701 panel @%p", st7701);
    return ESP_OK;
}

static esp_err_t panel_st7701_reset(esp_lcd_panel_t *panel)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;

    // Perform hardware reset
    if (st7701->reset_gpio_num >= 0) {
        gpio_set_level(st7701->reset_gpio_num, st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st7701->reset_gpio_num, !st7701->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else if (io) { // Perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    // Reset RGB panel
    ESP_RETURN_ON_ERROR(st7701->reset(panel), TAG, "reset RGB panel failed");

    return ESP_OK;
}

static esp_err_t panel_st7701_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    uint8_t sdir_val = 0;

    if (st7701->flags.mirror_by_cmd) {
        ESP_RETURN_ON_FALSE(io, ESP_FAIL, TAG, "Panel IO is deleted, cannot send command");
        // Control mirror through LCD command
        if (mirror_x) {
            sdir_val = ST7701_CMD_SS_BIT;
        } else {
            sdir_val = 0;
        }
        if (mirror_y) {
            st7701->madctl_val |= LCD_CMD_ML_BIT;
        } else {
            st7701->madctl_val &= ~LCD_CMD_ML_BIT;
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, ST7701_CMD_SDIR, (uint8_t[]) {
            sdir_val,
        }, 1), TAG, "send command failed");;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
            st7701->madctl_val,
        }, 1), TAG, "send command failed");;
    } else {
        // Control mirror through RGB panel
        ESP_RETURN_ON_ERROR(st7701->mirror(panel, mirror_x, mirror_y), TAG, "RGB panel mirror failed");
    }
    return ESP_OK;
}

static esp_err_t panel_st7701_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st7701_panel_t *st7701 = (st7701_panel_t *)panel->user_data;
    esp_lcd_panel_io_handle_t io = st7701->io;
    int command = 0;

    if (st7701->flags.display_on_off_use_cmd) {
        ESP_RETURN_ON_FALSE(io, ESP_FAIL, TAG, "Panel IO is deleted, cannot send command");
        // Control display on/off through LCD command
        if (on_off) {
            command = LCD_CMD_DISPON;
        } else {
            command = LCD_CMD_DISPOFF;
        }
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    } else {
        // Control display on/off through display control signal
        ESP_RETURN_ON_ERROR(st7701->disp_on_off(panel, on_off), TAG, "RGB panel disp_on_off failed");
    }
    return ESP_OK;
}
#endif

#endif // ESP_PANEL_DRIVERS_LCD_ENABLE_ST7701
