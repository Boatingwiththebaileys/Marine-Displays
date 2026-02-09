/*****************************************************************************
  | File        :   LVGL_Driver.c
  
  | help        : 
    The provided LVGL library file must be installed first
******************************************************************************/
#include "LVGL_Driver.h"
#include "esp_timer.h"
#include "SD_Card.h"
#include <SD_MMC.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

// Diagnostic: set to 1 to force byte-swapped (big-endian) drawing path
#define FORCE_BE_DRAW 0

static const char *TAG_LVGL = "LVGL";

lv_disp_drv_t disp_drv;

// LVGL filesystem driver callbacks for SD card access
// LVGL file wrapper that supports either Arduino `File*` (SD_MMC) or POSIX `FILE*` (/sdcard)
typedef struct {
  bool is_posix;
  File *file;   // owned when is_posix == false
  FILE *fp;     // owned when is_posix == true
} lv_file_wrapper_t;

static void * fs_open_cb(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    ESP_LOGD(TAG_LVGL, "SD: Opening file: %s", path);

    // LVGL passes paths with a drive-letter prefix like "S:/assets/...".
    // SD_MMC.open() expects paths relative to the SD root (e.g. "/assets/...").
    const char * sd_path = path;
    if (path && strlen(path) > 2 && path[1] == ':' && path[2] == '/') {
      sd_path = path + 2; // skip "S:"
    }

    // Try Arduino SD_MMC first (preferred)
    File* file = new File(SD_MMC.open(sd_path, FILE_READ));
    if (*file) {
      ESP_LOGD(TAG_LVGL, "SD: Successfully opened file via SD_MMC: %s (sd_path=%s size=%d bytes)", path, sd_path, file->size());
      lv_file_wrapper_t *w = (lv_file_wrapper_t*)malloc(sizeof(lv_file_wrapper_t));
      w->is_posix = false;
      w->file = file;
      w->fp = NULL;
      return (void*)w;
    }
    delete file;

    // Fallback: try POSIX fopen on the SDSPI VFS mount at /sdcard
    char alt[256];
    snprintf(alt, sizeof(alt), "/sdcard%s", sd_path);
    FILE *fp = fopen(alt, "rb");
    if (!fp) {
      ESP_LOGW(TAG_LVGL, "SD: Failed to open file via SD_MMC and POSIX fallback: %s (tried %s)", path, alt);
      return NULL;
    }
    ESP_LOGD(TAG_LVGL, "SD: Successfully opened file via POSIX fallback: %s (alt=%s)", path, alt);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)malloc(sizeof(lv_file_wrapper_t));
    w->is_posix = true;
    w->file = NULL;
    w->fp = fp;
    return (void*)w;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t * drv, void * file_p) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_OK;
    if (w->is_posix) {
      fclose(w->fp);
    } else {
      w->file->close();
      delete w->file;
    }
    free(w);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) {
      size_t r = fread(buf, 1, btr, w->fp);
      *br = (uint32_t)r;
      return LV_FS_RES_OK;
    } else {
      *br = w->file->read((uint8_t *)buf, btr);
      return (int32_t)(*br) < 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
    }
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) {
      int origin = SEEK_SET;
      if (whence == LV_FS_SEEK_SET) origin = SEEK_SET;
      else if (whence == LV_FS_SEEK_CUR) origin = SEEK_CUR;
      else if (whence == LV_FS_SEEK_END) origin = SEEK_END;
      int rc = fseek(w->fp, (long)pos, origin);
      return (rc == 0) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
    } else {
      SeekMode mode;
      if(whence == LV_FS_SEEK_SET) mode = SeekSet;
      else if(whence == LV_FS_SEEK_CUR) mode = SeekCur;
      else if(whence == LV_FS_SEEK_END) mode = SeekEnd;
      else return LV_FS_RES_UNKNOWN;
      w->file->seek(pos, mode);
      return LV_FS_RES_OK;
    }
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
    LV_UNUSED(drv);
    lv_file_wrapper_t *w = (lv_file_wrapper_t*)file_p;
    if (!w) return LV_FS_RES_UNKNOWN;
    if (w->is_posix) {
      long p = ftell(w->fp);
      if (p < 0) return LV_FS_RES_UNKNOWN;
      *pos_p = (uint32_t)p;
      return LV_FS_RES_OK;
    } else {
      *pos_p = w->file->position();
      return LV_FS_RES_OK;
    }
}

static lv_disp_draw_buf_t draw_buf;
void* buf1 = NULL;
void* buf2 = NULL;
static volatile uint32_t g_flush_max_us = 0;
static volatile uint32_t g_flush_count = 0;

// Pixel remapping support for diagnosing wiring/endian/color issues
// g_remap_table[src_bit] -> target_bit
static bool g_remap_enabled = false;
static uint8_t g_remap_table[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

void Lvgl_ToggleRemap() {
  g_remap_enabled = !g_remap_enabled;
  ESP_LOGI(TAG_LVGL, "LVGL remap -> %d", g_remap_enabled ? 1 : 0);
}

void Lvgl_EnableRemap(bool en) {
  g_remap_enabled = en;
  ESP_LOGI(TAG_LVGL, "LVGL remap -> %d", g_remap_enabled ? 1 : 0);
}

void Lvgl_SetRemapTable(const uint8_t table[16]) {
  for (int i = 0; i < 16; ++i) g_remap_table[i] = table[i];
  ESP_LOGI(TAG_LVGL, "LVGL remap table set");
}

void Lvgl_PrintRemap() {
  ESP_LOGI(TAG_LVGL, "LVGL remap_enabled=%d", g_remap_enabled ? 1 : 0);
  char buf[256];
  int p = 0;
  p += snprintf(buf + p, sizeof(buf) - p, "remap src->tgt: ");
  for (int i = 0; i < 16; ++i) p += snprintf(buf + p, sizeof(buf) - p, "%d:%d%s", i, g_remap_table[i], (i == 15) ? "" : ",");
  ESP_LOGI(TAG_LVGL, "%s", buf);
}

static inline uint16_t lvgl_remap_pixel(uint16_t v) {
  uint16_t r = 0;
  for (int i = 0; i < 16; ++i) if (v & (1u << i)) r |= (1u << g_remap_table[i]);
  return r;
}

static void lvgl_remap_pixels_inplace(lv_color_t *colors, int pixel_count) {
  uint16_t *p = (uint16_t *)colors;
  for (int i = 0; i < pixel_count; ++i) p[i] = lvgl_remap_pixel(p[i]);
}
// static lv_color_t buf1[ LVGL_BUF_LEN ];
// static lv_color_t buf2[ LVGL_BUF_LEN ];
// static lv_color_t* buf1 = (lv_color_t*) heap_caps_malloc(LVGL_BUF_LEN, MALLOC_CAP_SPIRAM);
// static lv_color_t* buf2 = (lv_color_t*) heap_caps_malloc(LVGL_BUF_LEN, MALLOC_CAP_SPIRAM);
    


/* Serial debugging */
void Lvgl_print(const char * buf)
{
    // Serial.printf(buf);
    // Serial.flush();
}

/*  Display flushing 
    Displays LVGL content on the LCD
    This function implements associating LVGL data to the LCD screen
*/
void Lvgl_Display_LCD( lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p )
{
  uint32_t t0 = (uint32_t)esp_timer_get_time();
  
  // Optionally remap pixels before sending to panel (diagnostic)
  if (g_remap_enabled) {
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t count = w * h;
    lvgl_remap_pixels_inplace(color_p, (int)count);
  }

  // Try passthrough with correct GPIO pinout
  LCD_addWindow(area->x1, area->y1, area->x2, area->y2, ( uint8_t *)color_p);
  uint32_t dur = (uint32_t)esp_timer_get_time() - t0;
  if (dur > g_flush_max_us) g_flush_max_us = dur;
  g_flush_count++;
  lv_disp_flush_ready( disp_drv );
}
/*Read the touchpad*/
void Lvgl_Touchpad_Read( lv_indev_drv_t * indev_drv, lv_indev_data_t * data )
{
  uint16_t touchpad_x[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t touchpad_y[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t strength[GT911_LCD_TOUCH_MAX_POINTS]   = {0};
  uint8_t touchpad_cnt = 0;
  Touch_Read_Data();
  uint8_t touchpad_pressed = Touch_Get_XY(touchpad_x, touchpad_y, strength, &touchpad_cnt, GT911_LCD_TOUCH_MAX_POINTS);
    if (touchpad_pressed && touchpad_cnt > 0) {
    data->point.x = touchpad_x[0];
    data->point.y = touchpad_y[0];
    data->state = LV_INDEV_STATE_PR;
    ESP_LOGD(TAG_LVGL, "LVGL : X=%u Y=%u num=%d", touchpad_x[0], touchpad_y[0], touchpad_cnt);
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}
void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}
void Lvgl_Init(void)
{
  lv_init();

  // Log LVGL compile-time endianness flags to catch accidental configuration mismatches
#ifdef LV_BIG_ENDIAN_SYSTEM
  ESP_LOGI(TAG_LVGL, "LV_BIG_ENDIAN_SYSTEM=%d", LV_BIG_ENDIAN_SYSTEM);
#else
  ESP_LOGI(TAG_LVGL, "LV_BIG_ENDIAN_SYSTEM not defined (assumed little-endian)");
#endif

  // Set LVGL image cache size (default is 1, increase for better caching)
  lv_img_cache_set_size(8); // Cache up to 8 images in RAM

  // Set up LVGL filesystem driver for SD card
  static lv_fs_drv_t fs_drv;
  lv_fs_drv_init(&fs_drv);
  fs_drv.letter = 'S';  // Drive letter - matches "S:" prefix in image paths
  fs_drv.open_cb = fs_open_cb;
  fs_drv.close_cb = fs_close_cb;
  fs_drv.read_cb = fs_read_cb;
  fs_drv.seek_cb = fs_seek_cb;
  fs_drv.tell_cb = fs_tell_cb;
  lv_fs_drv_register(&fs_drv);
  ESP_LOGI(TAG_LVGL, "LVGL SD card filesystem driver registered (S:)");
  
  // Use 1/5 screen LVGL buffers to reduce number of redraw passes
  // Larger buffers = fewer passes = less visible jumping during redraws
  size_t lv_buf_px = (ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT / 5);
  size_t lv_buf_bytes = lv_buf_px * sizeof(lv_color_t);
  const size_t lv_buf_align = 64;
  buf1 = (lv_color_t*) heap_caps_aligned_alloc(lv_buf_align, lv_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  buf2 = (lv_color_t*) heap_caps_aligned_alloc(lv_buf_align, lv_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
  lv_disp_draw_buf_init( &draw_buf, buf1, buf2, lv_buf_px);
  ESP_LOGI(TAG_LVGL, "LVGL buffers (PSRAM+DMA, align=%d): buf1=%p buf2=%p size=%u", lv_buf_align, buf1, buf2, (unsigned)lv_buf_bytes);
  if (!buf1 || !buf2) {
    ESP_LOGW(TAG_LVGL, "Initial LVGL DMA buffer allocation failed (buf1=%p buf2=%p). Trying internal DMA fallback.", buf1, buf2);
    if (!buf1) buf1 = (lv_color_t*) heap_caps_aligned_alloc(lv_buf_align, lv_buf_bytes, MALLOC_CAP_DMA);
    if (!buf2) buf2 = (lv_color_t*) heap_caps_aligned_alloc(lv_buf_align, lv_buf_bytes, MALLOC_CAP_DMA);
    // Re-init draw buffer with any newly allocated pointers
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, lv_buf_px);
    ESP_LOGI(TAG_LVGL, "LVGL buffers after DMA fallback: buf1=%p buf2=%p", buf1, buf2);
    if (!buf1 || !buf2) {
      ESP_LOGW(TAG_LVGL, "Internal DMA fallback failed, falling back to non-DMA internal allocation (may show color corruption).", buf1, buf2);
      if (!buf1) buf1 = (lv_color_t*) heap_caps_malloc(lv_buf_bytes, MALLOC_CAP_INTERNAL);
      if (!buf2) buf2 = (lv_color_t*) heap_caps_malloc(lv_buf_bytes, MALLOC_CAP_INTERNAL);
      lv_disp_draw_buf_init(&draw_buf, buf1, buf2, lv_buf_px);
      ESP_LOGI(TAG_LVGL, "LVGL buffers after final fallback: buf1=%p buf2=%p", buf1, buf2);
      if (!buf1 || !buf2) {
        ESP_LOGE(TAG_LVGL, "LVGL buffer allocation failed after all fallbacks (buf1=%p buf2=%p). Display likely won't render.", buf1, buf2);
      }
    }
  }

  /*Initialize the display*/
  lv_disp_drv_init( &disp_drv );
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = LVGL_WIDTH;
  disp_drv.ver_res = LVGL_HEIGHT;
  disp_drv.flush_cb = Lvgl_Display_LCD;
  // Use smaller buffers for incremental rendering
  disp_drv.draw_buf = &draw_buf;
  disp_drv.user_data = panel_handle;
  disp_drv.sw_rotate = 1;           // Software rotation (hardware MADCTL reads don't work)
  disp_drv.rotated = LV_DISP_ROT_180; // 180° rotation
  disp_drv.direct_mode = 0;         // Disable direct mode
  disp_drv.full_refresh = 0;        // Partial refresh
  
  // Register display and optimize for responsiveness
  lv_disp_t * disp = lv_disp_drv_register( &disp_drv );
  lv_disp_set_default(disp);
  
  // NOTE: Refresh timer period is set in lv_conf.h via LV_DISP_DEF_REFR_PERIOD
  // Overriding it here can cause tearing and conflicts
  // lv_timer_t * refr_timer = _lv_disp_get_refr_timer(disp);
  // if (refr_timer != NULL) {
  //   lv_timer_set_period(refr_timer, 5); // Disabled - use lv_conf.h setting
  // }

  /*Initialize the (dummy) input device driver*/
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init( &indev_drv );
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = Lvgl_Touchpad_Read;
  indev_drv.gesture_min_velocity = 3;   // Lower threshold - easier to trigger
  indev_drv.gesture_limit = 30;         // Lower minimum movement distance
  lv_indev_t *indev = lv_indev_drv_register( &indev_drv );
  ESP_LOGD(TAG_LVGL, "Gesture settings: min_vel=%d limit=%d", indev_drv.gesture_min_velocity, indev_drv.gesture_limit);

  /* Create simple label */
  lv_obj_t *label = lv_label_create( lv_scr_act() );
  lv_label_set_text( label, "Hello Ardino and LVGL!");
  lv_obj_align( label, LV_ALIGN_CENTER, 0, 0 );

  const esp_timer_create_args_t lvgl_tick_timer_args = {
    .callback = &example_increase_lvgl_tick,
    .name = "lvgl_tick"
  };
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

  // Panel tests disabled: skipping post-LVGL display fill test
  ESP_LOGI(TAG_LVGL, "[DISPLAY-TEST] Panel tests disabled; skipping post-LVGL fill test");

}

  uint32_t get_flush_max_us() {
    return g_flush_max_us;
  }

  uint32_t get_flush_count() {
    return g_flush_count;
  }

  void reset_flush_stats() {
    g_flush_max_us = 0;
    g_flush_count = 0;
  }
void Lvgl_Loop(void)
{
  lv_timer_handler(); /* let the GUI do its work */
  // delay( 5 );
}
