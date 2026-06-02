#ifndef DISPLAY_ST7305_H
#define DISPLAY_ST7305_H

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>

/*
 * ST7305 Reflective LCD driver for Waveshare ESP32-S3-RLCD-4.2.
 * 400×300 monochrome (1-bit), SPI interface.
 * Uses LUT-based pixel mapping for fast set-pixel operations.
 *
 * Based on Waveshare's display_bsp from the official demo.
 */

class DisplayST7305 {
public:
    DisplayST7305();
    ~DisplayST7305();

    void init();
    void clear(uint8_t color = 0xFF);   // 0xFF = white, 0x00 = black
    void flush();                        // Push framebuffer to display
    void setPixel(uint16_t x, uint16_t y, uint8_t color);

    int width()  const { return width_; }
    int height() const { return height_; }

    void setInversion(bool on);  // Toggle display inversion (0x20=off, 0x21=on)

private:
    static constexpr int width_  = 400;   // landscape
    static constexpr int height_ = 300;

    esp_lcd_panel_io_handle_t io_handle_ = nullptr;

    // Framebuffer: 1 bit per pixel, packed = 400*300/8 = 15000 bytes
    uint8_t *framebuf_ = nullptr;
    int      framebuf_len_ = 0;

    // LUT for fast pixel index/bit lookup (allocated in PSRAM)
    uint16_t (*pixelIndexLUT_)[300] = nullptr;
    uint8_t  (*pixelBitLUT_)[300]   = nullptr;

    void initLUT();
    void reset();
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t data);
    void sendBuffer(uint8_t *data, int len);
};

#endif // DISPLAY_ST7305_H
