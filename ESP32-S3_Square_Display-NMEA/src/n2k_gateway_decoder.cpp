// n2k_gateway_decoder.cpp — see n2k_gateway_decoder.h

#include "n2k_gateway_decoder.h"

#include <NMEA2000.h>
#include <Seasmart.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ── YD RAW: a tNMEA2000 whose "CAN bus" is the text frames we parse ──────
// Feeding frames through the library means fast-packet reassembly (GNSS,
// AIS, engine dynamic, ...) is exactly the same as on the real CAN port.
class tN2kVirtualCan : public tNMEA2000 {
public:
    static const uint8_t RING = 32;

    bool push(unsigned long id, unsigned char len, const unsigned char* buf) {
        uint8_t next = (head + 1) % RING;
        if (next == tail) return false;          // full
        frames[head].id = id;
        frames[head].len = len;
        memcpy(frames[head].data, buf, len);
        head = next;
        return true;
    }
    bool empty() const { return head == tail; }
    bool is_open() const { return opened; }
    void clear() { head = tail = 0; }

protected:
    bool CANOpen() override { opened = true; return true; }
    bool CANSendFrame(unsigned long, unsigned char, const unsigned char*, bool) override {
        return true;                             // listen only: drop anything outgoing
    }
    bool CANGetFrame(unsigned long& id, unsigned char& len, unsigned char* buf) override {
        if (empty() || !ready_for_frames()) return false;
        id = frames[tail].id;
        len = frames[tail].len;
        memcpy(buf, frames[tail].data, len);
        tail = (tail + 1) % RING;
        return true;
    }

private:
    // While the library is still opening it reads and discards "rubbish"
    // frames, so hold ours back until it is fully open.
    bool ready_for_frames() { return opened && IsOpen(); }

    struct Frame { unsigned long id; unsigned char len; unsigned char data[8]; };
    Frame frames[RING];
    volatile uint8_t head = 0, tail = 0;
    bool opened = false;
};

// ── Helpers ─────────────────────────────────────────────────────────────
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ── Decoder ─────────────────────────────────────────────────────────────
tN2kGatewayDecoder::tN2kGatewayDecoder(N2kGwFormat fmt, N2kGwMsgHandler h)
    : format(fmt), handler(h) {
    if (format == N2KGW_FMT_YDRAW) {
        vcan = new tN2kVirtualCan();
        vcan->SetN2kCANMsgBufSize(16);           // concurrent fast-packet assemblies
        vcan->SetMode(tNMEA2000::N2km_ListenOnly, 25);
        vcan->SetMsgHandler(handler);
        vcan->Open();
    }
}

tN2kGatewayDecoder::~tN2kGatewayDecoder() {
    delete vcan;
}

bool tN2kGatewayDecoder::ready() {
    if (!vcan) return true;
    if (!vcan->IsOpen()) vcan->ParseMessages();  // advances the library's open sequence
    return vcan->IsOpen();
}

void tN2kGatewayDecoder::poll() {
    if (vcan) vcan->ParseMessages();
}

void tN2kGatewayDecoder::reset() {
    line_len = 0;
    line_overflow = false;
    act_len = 0;
    act_in_msg = false;
    act_escape = false;
    if (vcan) vcan->clear();
}

void tN2kGatewayDecoder::feed(const uint8_t* data, size_t len) {
    if (format == N2KGW_FMT_ACTISENSE) {
        for (size_t i = 0; i < len; i++) handle_actisense_byte(data[i]);
        return;
    }

    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (line_len > 0 && !line_overflow) {
                line_buf[line_len] = '\0';
                handle_line(line_buf);
            } else if (line_overflow) {
                bad_count++;
            }
            line_len = 0;
            line_overflow = false;
        } else if (line_len < LINE_BUF_LEN - 1) {
            line_buf[line_len++] = c;
        } else {
            line_overflow = true;
        }
    }
}

void tN2kGatewayDecoder::handle_line(char* line) {
    bool ok = (format == N2KGW_FMT_YDRAW) ? handle_ydraw_line(line)
                                          : handle_seasmart_line(line);
    if (!ok) bad_count++;
}

// YD RAW:  "17:33:21.107 R 19F51323 01 2F 30 70 00 2F 30 70"
// Only received ("R") frames are bus traffic; "T" lines are the gateway's own.
bool tN2kGatewayDecoder::handle_ydraw_line(char* line) {
    char* save = nullptr;
    char* tok = strtok_r(line, " \t", &save);           // time
    if (!tok || !strchr(tok, ':')) return false;
    tok = strtok_r(nullptr, " \t", &save);              // direction
    if (!tok || tok[1] != '\0') return false;
    if (tok[0] == 'T' || tok[0] == 't') return true;   // our own TX echo: ignore, not an error
    if (tok[0] != 'R' && tok[0] != 'r') return false;
    tok = strtok_r(nullptr, " \t", &save);              // 29-bit CAN id
    if (!tok) return false;
    char* end = nullptr;
    unsigned long id = strtoul(tok, &end, 16);
    if (*end != '\0' || id > 0x1FFFFFFFUL) return false;

    unsigned char buf[8];
    unsigned char n = 0;
    while ((tok = strtok_r(nullptr, " \t", &save)) != nullptr) {
        if (n >= 8 || strlen(tok) != 2) return false;
        int hi = hexval(tok[0]), lo = hexval(tok[1]);
        if (hi < 0 || lo < 0) return false;
        buf[n++] = (unsigned char)(hi << 4 | lo);
    }
    if (n == 0) return false;
    if (!ready()) return true;                // library still starting: frame dropped

    // Drain if the ring is full (bursts of AIS), then queue the frame.
    while (!vcan->push(id, n, buf)) vcan->ParseMessages();
    while (!vcan->empty() && vcan->IsOpen()) vcan->ParseMessages();
    return true;
}

// SeaSmart: "$PCDIN,01F119,00000000,0F,2AAF00D1067414FF*59"
bool tN2kGatewayDecoder::handle_seasmart_line(char* line) {
    char* start = strstr(line, "$PCDIN");
    if (!start) return false;
    uint32_t timestamp;
    tN2kMsg msg;
    if (!SeasmartToN2k(start, timestamp, msg)) return false;
    handler(msg);
    return true;
}

// Actisense NGT binary: DLE STX <cmd> <len> <payload...> <crc> DLE ETX,
// with any DLE (0x10) inside doubled. cmd 0x93 = N2K message received.
void tN2kGatewayDecoder::handle_actisense_byte(uint8_t b) {
    const uint8_t DLE = 0x10, STX = 0x02, ETX = 0x03;

    if (act_escape) {
        act_escape = false;
        if (b == STX) {                       // start of a message (resyncs anywhere)
            act_in_msg = true;
            act_len = 0;
        } else if (b == ETX && act_in_msg) {  // end of message
            handle_actisense_message();
            act_in_msg = false;
        } else if (b == DLE && act_in_msg) {  // escaped DLE data byte
            if (act_len < ACT_MAX) act_buf[act_len++] = b; else act_in_msg = false;
        } else if (act_in_msg) {              // framing error
            bad_count++;
            act_in_msg = false;
        }
        return;
    }
    if (b == DLE) { act_escape = true; return; }
    if (!act_in_msg) return;                  // between messages / joined mid-stream
    if (act_len < ACT_MAX) act_buf[act_len++] = b;
    else { bad_count++; act_in_msg = false; }
}

void tN2kGatewayDecoder::handle_actisense_message() {
    // act_buf: cmd, len, prio, pgn(3), dst, src, time(4), dlen, data..., crc
    if (act_len < 2) { bad_count++; return; }
    uint8_t sum = 0;
    for (size_t i = 0; i < act_len; i++) sum += act_buf[i];
    if (sum != 0) { bad_count++; return; }

    if (act_buf[0] != 0x93) return;           // other Actisense commands: ignore
    const uint8_t* p = act_buf + 2;
    size_t plen = act_len - 3;                // minus cmd, len, crc
    if (act_buf[1] != plen || plen < 11) { bad_count++; return; }
    uint8_t dlen = p[10];
    if ((size_t)dlen + 11 != plen || dlen > tN2kMsg::MaxDataLen) { bad_count++; return; }

    tN2kMsg msg;
    msg.Priority = p[0] & 0x07;
    msg.PGN = (unsigned long)p[1] | ((unsigned long)p[2] << 8) | ((unsigned long)p[3] << 16);
    msg.Destination = p[4];
    msg.Source = p[5];
    msg.MsgTime = (unsigned long)p[6] | ((unsigned long)p[7] << 8) |
                  ((unsigned long)p[8] << 16) | ((unsigned long)p[9] << 24);
    msg.DataLen = dlen;
    memcpy(msg.Data, p + 11, dlen);
    handler(msg);
}
