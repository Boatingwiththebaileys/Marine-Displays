#ifndef GAUGE_NUMBER_DISPLAY_H
#define GAUGE_NUMBER_DISPLAY_H

#include <lvgl.h>

// Create gauge+number display for a screen (gauge on top, number in center)
void gauge_number_display_create(int screen_num, 
                                   uint8_t center_font_size, 
                                   const char* center_font_color);

// Update center number display with new value, units, and description
void gauge_number_display_update_center(int screen_num, float value, const char* unit, const char* description);

// Destroy gauge+number display for a screen
void gauge_number_display_destroy(int screen_num);

#endif // GAUGE_NUMBER_DISPLAY_H
