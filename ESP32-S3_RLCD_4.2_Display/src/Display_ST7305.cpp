/*
 * ST7305 Reflective LCD display driver.
 * Ported from Waveshare ESP32-S3-RLCD-4.2 demo (display_bsp.cpp).
 * Uses SPI via esp_lcd panel IO, LUT-optimised pixel mapping.
 */

#include "Display_ST7305.h"
#include "board_pins.h"

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "ST7305";

// ─── Constructor / Destructor ───────────────────────────────────────────────

DisplayST7305::DisplayST7305() {
    int transfer = width_ * height_;

    // SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num    = -1;
    buscfg.mosi_io_num    = RLCD_MOSI_PIN;
    buscfg.sclk_io_num    = RLCD_SCK_PIN;
    buscfg.quadwp_io_num  = -1;
    buscfg.quadhd_io_num  = -1;
    buscfg.max_transfer_sz = transfer;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Panel IO (SPI)
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num     = RLCD_DC_PIN;
    io_config.cs_gpio_num     = RLCD_CS_PIN;
    io_config.pclk_hz         = 10 * 1000 * 1000;  // 10 MHz
    io_config.lcd_cmd_bits    = 8;
    io_config.lcd_param_bits  = 8;
    io_config.spi_mode        = 0;
    io_config.trans_queue_depth = 10;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &io_handle_));

    // RST pin
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type    = GPIO_INTR_DISABLE;
    gpio_conf.mode         = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = (1ULL << RLCD_RST_PIN);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    gpio_set_level(RLCD_RST_PIN, 1);

    // Framebuffer in PSRAM (1 bit per pixel)
    framebuf_len_ = transfer >> 3;  // 400*300 / 8 = 15000 bytes
    framebuf_ = (uint8_t *)heap_caps_malloc(framebuf_len_, MALLOC_CAP_SPIRAM);
    assert(framebuf_);

    // LUT tables in PSRAM for fast pixel mapping
    pixelIndexLUT_ = (uint16_t(*)[300])heap_caps_malloc(
        transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    pixelBitLUT_ = (uint8_t(*)[300])heap_caps_malloc(
        transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(pixelIndexLUT_);
    assert(pixelBitLUT_);

    initLUT();
    ESP_LOGI(TAG, "ST7305 driver constructed (400x300, SPI, LUT mode)");
}

DisplayST7305::~DisplayST7305() {
    // Not expected to be destroyed at runtime
}

// ─── Public API ─────────────────────────────────────────────────────────────

void DisplayST7305::init() {
    reset();

    // ST7305 initialisation sequence from Waveshare demo
    sendCommand(0xD6);  sendData(0x17); sendData(0x02);   // NVM Load Control
    sendCommand(0xD1);  sendData(0x01);                    // Booster Enable
    sendCommand(0xC0);  sendData(0x11); sendData(0x04);   // Gate Voltage Control

    sendCommand(0xC1);  // VSHP Setting
    sendData(0x69); sendData(0x69); sendData(0x69); sendData(0x69);

    sendCommand(0xC2);
    sendData(0x19); sendData(0x19); sendData(0x19); sendData(0x19);

    sendCommand(0xC4);
    sendData(0x4B); sendData(0x4B); sendData(0x4B); sendData(0x4B);

    sendCommand(0xC5);
    sendData(0x19); sendData(0x19); sendData(0x19); sendData(0x19);

    sendCommand(0xD8);  sendData(0x80); sendData(0xE9);
    sendCommand(0xB2);  sendData(0x02);

    sendCommand(0xB3);  // Waveform setting
    sendData(0xE5); sendData(0xF6); sendData(0x05); sendData(0x46);
    sendData(0x77); sendData(0x77); sendData(0x77); sendData(0x77);
    sendData(0x76); sendData(0x45);

    sendCommand(0xB4);
    sendData(0x05); sendData(0x46); sendData(0x77); sendData(0x77);
    sendData(0x77); sendData(0x77); sendData(0x76); sendData(0x45);

    sendCommand(0x62);  sendData(0x32); sendData(0x03); sendData(0x1F);
    sendCommand(0xB7);  sendData(0x13);
    sendCommand(0xB0);  sendData(0x64);

    sendCommand(0x11);  // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(200));

    sendCommand(0xC9);  sendData(0x00);
    sendCommand(0x36);  sendData(0x48);  // Memory Access Control
    sendCommand(0x3A);  sendData(0x11);  // Interface Pixel Format (1-bit)
    sendCommand(0xB9);  sendData(0x20);
    sendCommand(0xB8);  sendData(0x29);
    sendCommand(0x21);                   // Display Inversion On

    sendCommand(0x2A);  sendData(0x12); sendData(0x2A);  // Column Address Set
    sendCommand(0x2B);  sendData(0x00); sendData(0xC7);  // Page Address Set
    sendCommand(0x35);  sendData(0x00);                   // Tearing Effect On
    sendCommand(0xD0);  sendData(0xFF);
    sendCommand(0x38);                   // Idle Mode Off
    sendCommand(0x29);                   // Display On

    clear(0xFF);  // Start with white screen
    flush();

    ESP_LOGI(TAG, "ST7305 display initialised");
}

void DisplayST7305::clear(uint8_t color) {
    memset(framebuf_, color, framebuf_len_);
}

void DisplayST7305::flush() {
    sendCommand(0x2A);  sendData(0x12); sendData(0x2A);  // Column Address Set
    sendCommand(0x2B);  sendData(0x00); sendData(0xC7);  // Page Address Set
    sendCommand(0x2C);                                     // Memory Write
    sendBuffer(framebuf_, framebuf_len_);
}

void DisplayST7305::setPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx  = pixelIndexLUT_[x][y];
    uint8_t  mask = pixelBitLUT_[x][y];
    if (color)
        framebuf_[idx] |= mask;
    else
        framebuf_[idx] &= ~mask;
}

void DisplayST7305::setInversion(bool on) {
    sendCommand(on ? 0x21 : 0x20);  // INVON / INVOFF
}

// ─── Private helpers ────────────────────────────────────────────────────────

void DisplayST7305::initLUT() {
    // Landscape LUT (width=400, height=300)
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++) {
        uint16_t inv_y   = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++) {
            uint16_t byte_x  = x >> 1;
            uint8_t  local_x = x & 1;
            uint32_t index   = byte_x * H4 + block_y;
            uint8_t  bit     = 7 - ((local_y << 1) | local_x);
            pixelIndexLUT_[x][y] = index;
            pixelBitLUT_[x][y]   = (1 << bit);
        }
    }
    ESP_LOGI(TAG, "Landscape LUT initialised (%dx%d)", width_, height_);
}

void DisplayST7305::reset() {
    gpio_set_level(RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(RLCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void DisplayST7305::sendCommand(uint8_t cmd) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle_, cmd, NULL, 0));
}

void DisplayST7305::sendData(uint8_t data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle_, -1, &data, 1));
}

void DisplayST7305::sendBuffer(uint8_t *data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle_, -1, data, len));
}
