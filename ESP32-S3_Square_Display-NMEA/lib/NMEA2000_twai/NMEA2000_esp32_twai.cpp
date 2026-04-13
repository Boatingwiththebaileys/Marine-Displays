// NMEA2000_esp32_twai.cpp — ESP32-S3 TWAI driver for ttlappalainen/NMEA2000
//
// Uses the ESP-IDF TWAI peripheral driver which supports all ESP32 variants
// including S3.  NMEA 2000 uses CAN 2.0B extended frames at 250 kbit/s.

#include "NMEA2000_esp32_twai.h"
#include <cstring>

tNMEA2000_twai::tNMEA2000_twai(gpio_num_t txPin, gpio_num_t rxPin)
    : tNMEA2000(), _txPin(txPin), _rxPin(rxPin) {}

// ---------- InitCANFrameBuffers ----------
void tNMEA2000_twai::InitCANFrameBuffers() {
    if (MaxCANReceiveFrames < 10) MaxCANReceiveFrames = 50;
    if (MaxCANSendFrames < 10)    MaxCANSendFrames = 40;
    tNMEA2000::InitCANFrameBuffers();
}

// ---------- CANOpen ----------
bool tNMEA2000_twai::CANOpen() {
    if (_isOpen) return true;

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        _txPin, _rxPin, TWAI_MODE_LISTEN_ONLY);
    g_config.rx_queue_len = MaxCANReceiveFrames;
    g_config.tx_queue_len = MaxCANSendFrames;

    twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
    if (err != ESP_OK) return false;

    err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }

    _isOpen = true;
    return true;
}

// ---------- CANSendFrame ----------
bool tNMEA2000_twai::CANSendFrame(unsigned long id, unsigned char len,
                                   const unsigned char *buf, bool wait_sent) {
    if (!_isOpen) return false;

    twai_message_t msg = {};
    msg.identifier      = id;
    msg.data_length_code = (len > 8) ? 8 : len;
    msg.extd             = 1;   // NMEA 2000 always uses 29-bit extended IDs
    std::memcpy(msg.data, buf, msg.data_length_code);

    TickType_t ticks = wait_sent ? pdMS_TO_TICKS(50) : 0;
    return twai_transmit(&msg, ticks) == ESP_OK;
}

// ---------- CANGetFrame ----------
bool tNMEA2000_twai::CANGetFrame(unsigned long &id, unsigned char &len,
                                  unsigned char *buf) {
    if (!_isOpen) return false;

    twai_message_t msg;
    if (twai_receive(&msg, 0) != ESP_OK) return false;   // non-blocking
    if (!msg.extd) return false;                          // ignore standard frames

    id  = msg.identifier;
    len = msg.data_length_code;
    std::memcpy(buf, msg.data, len);
    return true;
}
