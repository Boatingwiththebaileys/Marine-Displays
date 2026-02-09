#ifndef QUAD_NUMBER_DISPLAY_H
#define QUAD_NUMBER_DISPLAY_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Create quad number display for a screen (4 quadrants: TL, TR, BL, BR)
void quad_number_display_create(int screen_num,
                                  uint8_t tl_font_size, const char* tl_font_color,
                                  uint8_t tr_font_size, const char* tr_font_color,
                                  uint8_t bl_font_size, const char* bl_font_color,
                                  uint8_t br_font_size, const char* br_font_color,
                                  const char* bg_color);

// Destroy quad number display for a screen
void quad_number_display_destroy(int screen_num);

// Update quadrant values
void quad_number_display_update_tl(int screen_num, float value, const char* unit, const char* description);
void quad_number_display_update_tr(int screen_num, float value, const char* unit, const char* description);
void quad_number_display_update_bl(int screen_num, float value, const char* unit, const char* description);
void quad_number_display_update_br(int screen_num, float value, const char* unit, const char* description);

#ifdef __cplusplus
}
#endif

#endif // QUAD_NUMBER_DISPLAY_H
