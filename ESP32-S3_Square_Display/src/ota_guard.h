// ota_guard.h — checks for the Firmware Update page (/update)
//
// Releases ship two kinds of .bin for each display:
//   marine-display-*.bin       full flash image (bootloader + partition table
//                              + app) for the USB web installer, offset 0
//   ota-marine-display-*.bin   just the app, for this page
// The Update library accepts both, but writing a full image into the app
// partition leaves the display unable to start until it is reinstalled over
// USB. OtaGuard rejects anything that isn't an ESP32-S3 app image built for
// this board, before (first chunk) or instead of (end) committing it.
//
// The same file is used by every display project; only OTA_BOARD_ID differs.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Displays that share hardware share an ID (Signal K and NMEA 2000 Square),
// so moving between those firmwares over WiFi is allowed.
#ifndef OTA_BOARD_ID
#define OTA_BOARD_ID "waveshare-esp32-s3-touch-lcd-4"
#endif

// Embedded in this firmware's image; an update must contain the same text.
static const char OTA_BOARD_TAG[] = "MarineDisplays-board:" OTA_BOARD_ID ";";

class OtaGuard {
public:
    void begin() {
        checked = false;
        hdr_len = 0;
        board_found = false;
        tail_len = 0;
        error = nullptr;
    }

    // Feed each uploaded chunk in order. Returns false once the upload is
    // known to be the wrong kind of file (stop writing, abort the update).
    bool feed(const uint8_t* buf, size_t len) {
        if (error) return false;
        if (!checked) {
            // Collect the first HDR_LEN bytes, however the upload is chunked.
            // Nothing reaches flash before this check: the Update library
            // buffers the first 4 KB in RAM.
            size_t take = len < HDR_LEN - hdr_len ? len : HDR_LEN - hdr_len;
            memcpy(hdr + hdr_len, buf, take);
            hdr_len += take;
            if (hdr_len == HDR_LEN) {
                checked = true;
                error = check_start(hdr, hdr_len);
                if (error) return false;
            }
        }
        if (!board_found) scan(buf, len);
        return true;
    }

    // Call when the upload is complete. Returns false if it must not be applied.
    bool finish() {
        if (!error && !checked) error = "That file is too small to be a firmware update.";
        if (!error && !board_found) error = "That firmware is for a different Marine Display model.";
        return error == nullptr;
    }

    // Why the upload was rejected (nullptr if it wasn't).
    const char* message() const { return error; }

    static const char* board_tag() { return OTA_BOARD_TAG; }

    // Exposed for testing: nullptr if buf starts like an ESP32-S3 app image.
    static const char* check_start(const uint8_t* buf, size_t len) {
        // esp_image_header_t (24 bytes) + first segment header (8 bytes),
        // then esp_app_desc_t, which starts with its magic word.
        if (len < 36) return "That file is too small to be a firmware update.";
        if (buf[0] != 0xE9) return "That isn't a firmware file.";
        uint16_t chip_id = (uint16_t)(buf[12] | (buf[13] << 8));
        if (chip_id != 9) return "That firmware is for a different ESP32 chip.";
        uint32_t magic = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
                         ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
        if (magic != 0xABCD5432UL)
            return "That's the full installer file (marine-display-....bin), which can't be used here. "
                   "Use the update file named ota-marine-display-....bin from the GitHub release, "
                   "or install over USB with the web installer.";
        return nullptr;
    }

private:
    static const size_t TAG_LEN = sizeof(OTA_BOARD_TAG) - 1;
    static const size_t HDR_LEN = 36;

    // Look for the board tag, including across chunk boundaries.
    void scan(const uint8_t* buf, size_t len) {
        // Bytes spanning the previous chunk's tail and this chunk's start
        if (tail_len > 0) {
            uint8_t join[2 * TAG_LEN];
            size_t head = len < TAG_LEN - 1 ? len : TAG_LEN - 1;
            memcpy(join, tail, tail_len);
            memcpy(join + tail_len, buf, head);
            if (contains(join, tail_len + head)) { board_found = true; return; }
        }
        if (contains(buf, len)) { board_found = true; return; }
        // Keep the last TAG_LEN-1 bytes seen for the next chunk
        if (len >= TAG_LEN - 1) {
            tail_len = TAG_LEN - 1;
            memcpy(tail, buf + len - tail_len, tail_len);
        } else {
            size_t keep = tail_len + len > TAG_LEN - 1 ? TAG_LEN - 1 - len : tail_len;
            memmove(tail, tail + tail_len - keep, keep);
            memcpy(tail + keep, buf, len);
            tail_len = keep + len;
        }
    }

    static bool contains(const uint8_t* hay, size_t n) {
        if (n < TAG_LEN) return false;
        for (size_t i = 0; i + TAG_LEN <= n; i++) {
            if (hay[i] == (uint8_t)OTA_BOARD_TAG[0] && memcmp(hay + i, OTA_BOARD_TAG, TAG_LEN) == 0)
                return true;
        }
        return false;
    }

    bool checked = false;
    uint8_t hdr[HDR_LEN];
    size_t hdr_len = 0;
    bool board_found = false;
    uint8_t tail[TAG_LEN];
    size_t tail_len = 0;
    const char* error = nullptr;
};
