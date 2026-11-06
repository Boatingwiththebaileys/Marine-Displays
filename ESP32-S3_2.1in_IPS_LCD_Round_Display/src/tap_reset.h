// tap_reset.h — "tap 5 times quickly to return to full brightness"
//
// Fed from the touch driver's read callback. A tap is a touch that lifts
// within TAP_MAX_MS and doesn't move more than TAP_MOVE_PX, so swipes and
// slider drags never count (and break a sequence in progress). When
// TAP_COUNT taps land within TAP_WINDOW_MS, update() returns true once.
//
// The same file is used by every touch display; only the brightness
// action that follows differs.

#pragma once
#include <stdint.h>
#include <stdlib.h>

class TapCounter {
public:
    static const uint8_t  TAP_COUNT     = 5;
    static const uint32_t TAP_WINDOW_MS = 1500;
    static const uint32_t TAP_MAX_MS    = 400;
    static const int      TAP_MOVE_PX   = 25;
    // The touch chip sometimes reports a lift and re-touch a few ms apart
    // within one tap. Nobody taps again that fast, so a touch starting this
    // soon after the last lift is the same tap and isn't counted again.
    static const uint32_t TAP_BOUNCE_MS = 40;

    // Call on every touch read. Returns true when the gesture completes.
    bool update(bool pressed, int x, int y, uint32_t now) {
        if (pressed) {
            if (!down) {
                down = true;
                x0 = x; y0 = y; t0 = now;
                moved = false;
                bounce = released_once && (now - last_release < TAP_BOUNCE_MS);
            } else if (abs(x - x0) > TAP_MOVE_PX || abs(y - y0) > TAP_MOVE_PX) {
                moved = true;
            }
            return false;
        }
        if (!down) return false;
        down = false;
        last_release = now; released_once = true;
        if (bounce) { bounce = false; return false; }
        if (ignore) { ignore = false; return false; }
        if (moved || now - t0 > TAP_MAX_MS) { n = 0; return false; }

        // Forget taps that are outside the window, then add this one.
        uint8_t keep = 0;
        for (uint8_t i = 0; i < n; i++) {
            if (now - taps[i] <= TAP_WINDOW_MS) taps[keep++] = taps[i];
        }
        n = keep;
        taps[n++] = now;
        if (n >= TAP_COUNT) { n = 0; return true; }
        return false;
    }

    // Forget any taps so far (e.g. while the Settings panel is open).
    void reset() { n = 0; down = false; ignore = false; }

    // The current press doesn't count (e.g. the touch that wakes the screen).
    void ignore_press() { ignore = true; down = true; moved = true; }

private:
    bool down = false, moved = false, ignore = false, bounce = false;
    uint32_t last_release = 0;
    bool released_once = false;
    int x0 = 0, y0 = 0;
    uint32_t t0 = 0;
    uint32_t taps[TAP_COUNT] = {0};
    uint8_t n = 0;
};

// Set by the touch driver, handled in loop() (outside the LVGL input read).
extern volatile bool g_full_brightness_request;
