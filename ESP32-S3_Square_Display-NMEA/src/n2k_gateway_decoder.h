// n2k_gateway_decoder.h — turn NMEA 2000 data from a WiFi gateway into tN2kMsg
//
// Gateways put the NMEA 2000 bus on the network in one of a few text/binary
// formats. This decoder takes the raw bytes (a chunk of a TCP stream or one
// UDP datagram) and calls the handler with each complete tN2kMsg — the same
// handler the CAN bus path uses, so the rest of the firmware is unchanged.
//
// Supported formats:
//   YD RAW    — Yacht Devices YDWG-02 / YDEN-02 "RAW" (one CAN frame per line).
//               Frames are fed through the NMEA2000 library as a virtual CAN
//               bus so fast-packet messages are reassembled exactly as on CAN.
//   Actisense — Actisense NGT binary (as used by Actisense and many
//               open-source gateways). Decoded here rather than with the
//               library's tActisenseReader, which can spin forever on a stray
//               0x02 byte when a TCP stream is joined mid-message.
//   SeaSmart  — $PCDIN sentences (Digital Yacht, Chetco SeaSmart, ...).
//
// No Arduino/WiFi dependencies, so it can be unit tested on a computer.

#ifndef N2K_GATEWAY_DECODER_H
#define N2K_GATEWAY_DECODER_H

#include <stdint.h>
#include <stddef.h>
#include <N2kMsg.h>

enum N2kGwFormat : uint8_t {
    N2KGW_FMT_YDRAW     = 0,
    N2KGW_FMT_ACTISENSE = 1,
    N2KGW_FMT_SEASMART  = 2,
};

typedef void (*N2kGwMsgHandler)(const tN2kMsg& msg);

class tN2kVirtualCan;      // YD RAW: library instance fed from text frames

class tN2kGatewayDecoder {
public:
    tN2kGatewayDecoder(N2kGwFormat format, N2kGwMsgHandler handler);
    ~tN2kGatewayDecoder();

    // Feed raw bytes as they arrive. For UDP pass each datagram whole.
    void feed(const uint8_t* data, size_t len);

    // Call regularly (every loop). Needed for YD RAW while the library starts.
    void poll();

    // Ready to decode? (YD RAW needs ~200 ms after creation; others always true)
    bool ready();

    // Drop any partial line/message, e.g. after a reconnect.
    void reset();

    // Lines/frames that couldn't be parsed (wrong format selected?)
    uint32_t bad_lines() const { return bad_count; }

private:
    void handle_line(char* line);
    void handle_actisense_byte(uint8_t b);
    void handle_actisense_message();
    bool handle_ydraw_line(char* line);
    bool handle_seasmart_line(char* line);

    N2kGwFormat format;
    N2kGwMsgHandler handler;

    static const size_t LINE_BUF_LEN = 640;   // longest $PCDIN (AIS static) is ~600
    char line_buf[LINE_BUF_LEN];
    size_t line_len = 0;
    bool line_overflow = false;

    tN2kVirtualCan* vcan = nullptr;

    // Actisense framing state: DLE STX <payload, DLE doubled> DLE ETX
    static const size_t ACT_MAX = 300;
    uint8_t act_buf[ACT_MAX];
    size_t act_len = 0;
    bool act_in_msg = false;
    bool act_escape = false;

    uint32_t bad_count = 0;
};

#endif // N2K_GATEWAY_DECODER_H
