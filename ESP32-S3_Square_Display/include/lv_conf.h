#ifndef LV_CONF_H
#define LV_CONF_H

/* Minimal LVGL configuration to disable ARM/Helium ASM for Xtensa builds */

/* Use the simple include (ensure lv_conf.h is found as <lv_conf.h>) */
#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE
#endif

/* Disable draw-time assembly optimizations (NEON/Helium/etc.) */
#ifndef LV_USE_DRAW_SW_ASM
#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#endif

/* Disable native helium assembly helpers */
#ifndef LV_USE_NATIVE_HELIUM_ASM
#define LV_USE_NATIVE_HELIUM_ASM 0
#endif

/* Disable arm-2d/other arm-specific renderers */
#ifndef LV_USE_ARM2D
#define LV_USE_ARM2D 0
#endif

/* Enable PNG decoder for loading PNG images from filesystem */
#define LV_USE_PNG 1
#define LV_COLOR_DEPTH 16  /* 16-bit color (RGB565) for the display */
/* Swap the byte order of 16-bit colors when required by the display driver
	0 = native (RGB565 little-endian), 1 = swapped (big-endian) */
#define LV_COLOR_16_SWAP 0

/* Enable transparent screen support - REQUIRED for transform_zoom to work */
#define LV_COLOR_SCREEN_TRANSP 1

/* Enable BMP decoder for loading BMP images from filesystem */
#define LV_USE_BMP 1

/* Use PSRAM for LVGL memory allocation - custom functions to force PSRAM usage */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM)

/* Performance optimizations */
#define LV_DISP_DEF_REFR_PERIOD 16  /* Refresh every 16ms (~60fps) - balances smoothness and artifact reduction */
#define LV_INDEV_DEF_READ_PERIOD 10 /* Read input every 10ms for responsive touch */

/* Cache adjacent screens for instant swiping with binary images */
#define LV_IMG_CACHE_DEF_SIZE 3     /* Cache current + adjacent screens */

/* Enable logging to debug image loading issues */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN  /* Show warnings and errors */
#define LV_LOG_PRINTF 1  /* Use printf for logging */

/* Disable built-in Montserrat fonts - using custom Inter fonts instead */
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_48 0

/* Enable complex drawing for transform_zoom support */
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4

/* Configure layer buffers for transformed layers (zoom requires larger buffers) */
#define LV_LAYER_SIMPLE_BUF_SIZE (32 * 1024)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (8 * 1024)

/* Reduce feature set to minimize unexpected dependencies (optional) */
/* Keep this file minimal; other configuration should use lv_conf_template.h */

#endif /*LV_CONF_H*/
