#ifndef LV_CONF_H
#define LV_CONF_H

/* Use the simple include (ensure lv_conf.h is found as <lv_conf.h>) */
#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE
#endif

/* Disable draw-time assembly optimizations (NEON/Helium/etc.) */
#ifndef LV_USE_DRAW_SW_ASM
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#endif
#ifndef LV_USE_NATIVE_HELIUM_ASM
#define LV_USE_NATIVE_HELIUM_ASM 0
#endif
#ifndef LV_USE_ARM2D
#define LV_USE_ARM2D 0
#endif

/*
 * Color depth: 16-bit internally, but the flush callback converts to 1-bit
 * monochrome for the ST7305 reflective LCD. Using 16-bit allows standard
 * LVGL widgets/themes to work; the flush callback thresholds to black/white.
 */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Use PSRAM for LVGL memory allocation */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM)

/*
 * Reflective LCD has no backlight and is slow-refresh (full frame SPI push).
 * Use a lower refresh rate to avoid hammering the SPI bus.
 */
#define LV_DISP_DEF_REFR_PERIOD 100  /* 100ms (~10fps) — adequate for reflective LCD */
#define LV_INDEV_DEF_READ_PERIOD 50  /* Button read every 50ms */

/* Image cache */
#define LV_IMG_CACHE_DEF_SIZE 8

/* Enable logging */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Montserrat fonts — for UI labels and small text */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_48 1

/* Enable complex drawing (for arcs, etc.) */
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4

#endif /*LV_CONF_H*/
