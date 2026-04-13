// NMEA2000_esp32_twai.h — ESP32-S3 TWAI (CAN) driver for NMEA2000 library
// Replaces the legacy tNMEA2000_esp32 which uses register-level CAN API
// not available on ESP32-S3.  Uses the ESP-IDF TWAI driver instead.

#ifndef NMEA2000_ESP32_TWAI_H
#define NMEA2000_ESP32_TWAI_H

#include <NMEA2000.h>
#include <driver/twai.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class tNMEA2000_twai : public tNMEA2000 {
public:
    tNMEA2000_twai(gpio_num_t txPin = GPIO_NUM_16,
                   gpio_num_t rxPin = GPIO_NUM_36);

protected:
    bool CANSendFrame(unsigned long id, unsigned char len,
                      const unsigned char *buf, bool wait_sent = true) override;
    bool CANOpen() override;
    bool CANGetFrame(unsigned long &id, unsigned char &len,
                     unsigned char *buf) override;
    void InitCANFrameBuffers() override;

private:
    gpio_num_t _txPin;
    gpio_num_t _rxPin;
    bool       _isOpen = false;
};

#endif // NMEA2000_ESP32_TWAI_H
