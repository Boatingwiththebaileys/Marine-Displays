#ifndef DUAL_NUMBER_DISPLAY_H
#define DUAL_NUMBER_DISPLAY_H

#include <lvgl.h>

// Create dual number displays for a screen (top and bottom halves)
void dual_number_display_create(int screen_num, 
                                  uint8_t top_font_size, const char* top_font_color,
                                  uint8_t bottom_font_size, const char* bottom_font_color,
                                  const char* bg_color);

// Update dual number displays with new values, units, and descriptions
void dual_number_display_update_top(int screen_num, float value, const char* unit, const char* description);
void dual_number_display_update_bottom(int screen_num, float value, const char* unit, const char* description);

// Destroy dual number displays for a screen
void dual_number_display_destroy(int screen_num);

#endif // DUAL_NUMBER_DISPLAY_H
