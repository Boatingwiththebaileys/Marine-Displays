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

/* Enable BMP decoder for loading BMP images from filesystem */
#define LV_USE_BMP 1

/* Use PSRAM for LVGL memory allocation - custom functions to force PSRAM usage */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM)

/* Performance optimizations */
#define LV_DISP_DEF_REFR_PERIOD 10  /* Refresh every 10ms for smoother animations */
#define LV_INDEV_DEF_READ_PERIOD 10 /* Read input every 10ms for responsive touch */

/* Cache adjacent screens for instant swiping with binary images */
#define LV_IMG_CACHE_DEF_SIZE 3     /* Cache current + adjacent screens */

/* Enable logging to debug image loading issues */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN  /* Show warnings and errors */
#define LV_LOG_PRINTF 1  /* Use printf for logging */

/* Reduce feature set to minimize unexpected dependencies (optional) */
/* Keep this file minimal; other configuration should use lv_conf_template.h */

#endif /*LV_CONF_H*/
