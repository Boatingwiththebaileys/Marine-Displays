#include <Arduino.h>
#include <Wire.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_io_expander_tca9554.h"
#include "esp_expander_utils.h"

// Board version detection from TCA9554PWR driver
extern bool is_board_v4();

#define I2C_TIMEOUT_MS (10)
#define IO_COUNT (8)
#define INPUT_REG_ADDR (0x00)
// OUTPUT and DIRECTION register addresses differ between TCA9554 (V3) and CH32V003 (V4)
// TCA9554: OUTPUT=0x01, DIR=0x03, dir polarity: 0=output,1=input
// CH32V003: OUTPUT=0x02, DIR=0x03, dir polarity: 1=output,0=input (INVERTED)
static uint8_t output_reg_addr() { return is_board_v4() ? 0x02 : 0x01; }
#define DIRECTION_REG_ADDR (0x03)
#define DIR_REG_DEFAULT_VAL (0xff)
#define OUT_REG_DEFAULT_VAL (0xff)

static const char *TAG = "tca9554_cpp";

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_io_expander_t base;
    i2c_port_t i2c_num; /* kept for signature compatibility */
    uint32_t i2c_address; /* 7-bit address */
    struct {
        uint8_t direction;
        uint8_t output;
    } regs;
} esp_io_expander_tca9554_t;

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value);
static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value);
static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t reset(esp_io_expander_t *handle);
static esp_err_t del(esp_io_expander_t *handle);

esp_err_t esp_io_expander_new_i2c_tca9554(i2c_port_t i2c_num, uint32_t i2c_address, esp_io_expander_handle_t *handle)
{
    ESP_LOGI(TAG, "version: %d.%d.%d", ESP_IO_EXPANDER_TCA9554_VER_MAJOR, ESP_IO_EXPANDER_TCA9554_VER_MINOR,
             ESP_IO_EXPANDER_TCA9554_VER_PATCH);
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)calloc(1, sizeof(esp_io_expander_tca9554_t));
    ESP_RETURN_ON_FALSE(tca, ESP_ERR_NO_MEM, TAG, "Malloc failed");

    tca->base.config.io_count = IO_COUNT;
    // TCA9554: dir_out_bit_zero=1 (bit 0 in dir reg = output)
    // CH32V003: dir_out_bit_zero=0 (bit 1 in dir reg = output)
    tca->base.config.flags.dir_out_bit_zero = is_board_v4() ? 0 : 1;
    tca->i2c_num = i2c_num;
    tca->i2c_address = i2c_address;
    tca->base.read_input_reg = read_input_reg;
    tca->base.write_output_reg = write_output_reg;
    tca->base.read_output_reg = read_output_reg;
    tca->base.write_direction_reg = write_direction_reg;
    tca->base.read_direction_reg = read_direction_reg;
    tca->base.del = del;
    tca->base.reset = reset;

    /* Ensure Wire is initialized (safe to call multiple times) */
    Wire.begin();

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(reset(&tca->base), err, TAG, "Reset failed");

    *handle = &tca->base;
    return ESP_OK;
err:
    free(tca);
    return ret;
}

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);

    Wire.beginTransmission((uint8_t)tca->i2c_address);
    Wire.write(INPUT_REG_ADDR);
    if (Wire.endTransmission() != 0) {
        ESP_LOGE(TAG, "Read input reg: endTransmission failed");
        return ESP_FAIL;
    }
    size_t got = Wire.requestFrom((uint8_t)tca->i2c_address, (uint8_t)1);
    if (got < 1) {
        ESP_LOGE(TAG, "Read input reg: requestFrom failed");
        return ESP_FAIL;
    }
    uint8_t temp = (uint8_t)Wire.read();
    *value = temp;
    return ESP_OK;
}

static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);
    value &= 0xff;

    Wire.beginTransmission((uint8_t)tca->i2c_address);
    Wire.write(output_reg_addr());
    Wire.write((uint8_t)value);
    if (Wire.endTransmission() != 0) {
        ESP_LOGE(TAG, "Write output reg failed");
        return ESP_FAIL;
    }
    tca->regs.output = (uint8_t)value;
    return ESP_OK;
}

static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);
    *value = tca->regs.output;
    return ESP_OK;
}

static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);
    value &= 0xff;

    Wire.beginTransmission((uint8_t)tca->i2c_address);
    Wire.write(DIRECTION_REG_ADDR);
    Wire.write((uint8_t)value);
    if (Wire.endTransmission() != 0) {
        ESP_LOGE(TAG, "Write direction reg failed");
        return ESP_FAIL;
    }
    tca->regs.direction = (uint8_t)value;
    return ESP_OK;
}

static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);
    *value = tca->regs.direction;
    return ESP_OK;
}

static esp_err_t reset(esp_io_expander_t *handle)
{
    // Read current register state instead of writing power-on defaults.
    // This preserves any pin configuration (direction, output levels) set
    // before this library was initialised — in particular the buzzer-off
    // state set by TCA9554PWR_Init() during early boot.  Writing the
    // defaults (DIR=0xFF all-inputs) would briefly float PIN6 (buzzer),
    // causing an audible glitch on every boot.
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);

    // Read direction register
    Wire.beginTransmission((uint8_t)tca->i2c_address);
    Wire.write(DIRECTION_REG_ADDR);
    if (Wire.endTransmission() != 0 || Wire.requestFrom((uint8_t)tca->i2c_address, (uint8_t)1) < 1) {
        ESP_LOGW(TAG, "Reset: failed to read direction reg, using defaults");
        tca->regs.direction = DIR_REG_DEFAULT_VAL;
        tca->regs.output    = OUT_REG_DEFAULT_VAL;
        return ESP_OK;
    }
    tca->regs.direction = (uint8_t)Wire.read();

    // Read output register (0x01 on TCA9554, 0x02 on CH32V003)
    Wire.beginTransmission((uint8_t)tca->i2c_address);
    Wire.write(output_reg_addr());
    if (Wire.endTransmission() != 0 || Wire.requestFrom((uint8_t)tca->i2c_address, (uint8_t)1) < 1) {
        ESP_LOGW(TAG, "Reset: failed to read output reg, using defaults");
        tca->regs.output = OUT_REG_DEFAULT_VAL;
        return ESP_OK;
    }
    tca->regs.output = (uint8_t)Wire.read();

    ESP_LOGI(TAG, "Reset: cached dir=0x%02x out=0x%02x (hardware preserved)", tca->regs.direction, tca->regs.output);
    return ESP_OK;
}

static esp_err_t del(esp_io_expander_t *handle)
{
    esp_io_expander_tca9554_t *tca = (esp_io_expander_tca9554_t *)__containerof(handle, esp_io_expander_tca9554_t, base);
    free(tca);
    return ESP_OK;
}

#ifdef __cplusplus
} // extern C
#endif
