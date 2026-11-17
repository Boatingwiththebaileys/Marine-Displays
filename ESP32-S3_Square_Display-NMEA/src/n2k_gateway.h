// n2k_gateway.h — NMEA 2000 data from a WiFi gateway instead of the CAN port
//
// When enabled, the CAN bus is not opened. A task on core 0 connects to the
// gateway (TCP client, or UDP listener), decodes its stream with
// tN2kGatewayDecoder, and passes each message to the same handler the CAN
// path uses. Receive only: nothing is ever sent to the gateway or the bus.

#ifndef N2K_GATEWAY_H
#define N2K_GATEWAY_H

#include <Arduino.h>
#include "n2k_gateway_decoder.h"

struct N2kGwSettings {
    bool use_wifi = false;              // false = CAN port (default)
    uint8_t format = N2KGW_FMT_YDRAW;   // N2kGwFormat
    bool udp = false;                   // false = TCP client, true = UDP listener
    String host;                        // gateway IP/hostname (TCP only)
    uint16_t port = 0;
};

void n2k_gw_load(N2kGwSettings& s);
void n2k_gw_save(const N2kGwSettings& s);
const N2kGwSettings& n2k_gw_settings();     // as loaded at boot

bool n2k_gw_enabled();                      // WiFi gateway selected?
void n2k_gw_start(N2kGwMsgHandler handler);
void n2k_gw_stop();

String n2k_gw_status();                     // human-readable connection status
uint32_t n2k_gw_bad_lines();

const char* n2k_gw_format_name(uint8_t format);

#endif // N2K_GATEWAY_H
