/*
 * Screen rendering engine for RLCD Marine Display.
 *
 * Reads screen_configs[] and draws the appropriate LVGL layout
 * for each display type on the monochrome 400×300 reflective LCD.
 */

#ifndef SCREEN_RENDER_H
#define SCREEN_RENDER_H

#include <stdint.h>

// Get/set current active screen (0-based internally)
int  get_current_screen();
void set_current_screen(int screen);

// Build the LVGL UI for the given screen index (0-based).
// Clears existing screen content and recreates widgets.
void render_screen(int screen);

// Update displayed values (call periodically from loop).
// Currently shows placeholder text; will display SignalK data once connected.
void update_screen_values();

#endif // SCREEN_RENDER_H
