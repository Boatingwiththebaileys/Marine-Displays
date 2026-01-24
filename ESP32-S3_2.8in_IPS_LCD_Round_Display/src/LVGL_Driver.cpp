/*****************************************************************************
  | File        :   LVGL_Driver.c
  
  | help        : 
    The provided LVGL library file must be installed first
******************************************************************************/
#include "LVGL_Driver.h"
#include "esp_timer.h"
#include "SD_Card.h"
#include <SD_MMC.h>
#include "esp_log.h"

static const char *TAG_LVGL = "LVGL";

lv_disp_drv_t disp_drv;

// LVGL filesystem driver callbacks for SD card access
static void * fs_open_cb(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    LV_UNUSED(drv);
    ESP_LOGD(TAG_LVGL, "SD: Opening file: %s", path);

    const char * flags = "";
    if(mode == LV_FS_MODE_WR) flags = FILE_WRITE;
    else if(mode == LV_FS_MODE_RD) flags = FILE_READ;
    else if(mode == (LV_FS_MODE_WR | LV_FS_MODE_RD)) flags = FILE_WRITE;

    // LVGL passes paths with a drive-letter prefix like "S:/assets/...".
    // SD_MMC.open() expects paths relative to the SD root (e.g. "/assets/...").
    const char * sd_path = path;
    if (path && strlen(path) > 2 && path[1] == ':' && path[2] == '/') {
      sd_path = path + 2; // skip "S:"
    }

    File* file = new File(SD_MMC.open(sd_path, flags));
    if(!(*file)) {
      ESP_LOGW(TAG_LVGL, "SD: Failed to open file: %s (tried %s)", path, sd_path);
        delete file;
        return NULL;
    }
    ESP_LOGD(TAG_LVGL, "SD: Successfully opened file: %s (sd_path=%s size=%d bytes)", path, sd_path, file->size());
    return (void *)file;
}

static lv_fs_res_t fs_close_cb(lv_fs_drv_t * drv, void * file_p) {
    LV_UNUSED(drv);
    File* file = (File*)file_p;
    file->close();
    delete file;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read_cb(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    LV_UNUSED(drv);
    File* file = (File*)file_p;
    *br = file->read((uint8_t *)buf, btr);
    return (int32_t)(*br) < 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek_cb(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    LV_UNUSED(drv);
    File* file = (File*)file_p;
    SeekMode mode;
    if(whence == LV_FS_SEEK_SET) mode = SeekSet;
    else if(whence == LV_FS_SEEK_CUR) mode = SeekCur;
    else if(whence == LV_FS_SEEK_END) mode = SeekEnd;
    else return LV_FS_RES_UNKNOWN;
    
    file->seek(pos, mode);
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell_cb(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
    LV_UNUSED(drv);
    File* file = (File*)file_p;
    *pos_p = file->position();
    return LV_FS_RES_OK;
}

static lv_disp_draw_buf_t draw_buf;
void* buf1 = NULL;
void* buf2 = NULL;
static volatile uint32_t g_flush_max_us = 0;
static volatile uint32_t g_flush_count = 0;
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
  
  // With double buffering, trigger buffer swap so the newly drawn buffer becomes visible
  // This prevents visible tearing as LVGL draws to back buffer while display shows front buffer
  LCD_addWindow(area->x1, area->y1, area->x2, area->y2, ( uint8_t *)&color_p->full);
  
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
  
  // Use half-screen LVGL buffers for optimal balance between speed and smoothness
  // Completes full redraw in 2 flush operations instead of 30+
  buf1 = (lv_color_t*) heap_caps_malloc((ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT / 2) * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf2 = (lv_color_t*) heap_caps_malloc((ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT / 2) * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init( &draw_buf, buf1, buf2, ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT / 2);                    

  /*Initialize the display*/
  lv_disp_drv_init( &disp_drv );
  /*Change the following line to your display resolution*/
  disp_drv.hor_res = LVGL_WIDTH;
  disp_drv.ver_res = LVGL_HEIGHT;
  disp_drv.flush_cb = Lvgl_Display_LCD;
  // Use smaller buffers for incremental rendering
  disp_drv.draw_buf = &draw_buf;
  disp_drv.user_data = panel_handle;
  
  // Register display and optimize for responsiveness
  lv_disp_t * disp = lv_disp_drv_register( &disp_drv );
  lv_disp_set_default(disp);
  
  // Use internal API to speed up refresh timer from 30ms to 5ms
  lv_timer_t * refr_timer = _lv_disp_get_refr_timer(disp);
  if (refr_timer != NULL) {
    lv_timer_set_period(refr_timer, 5); // 5ms refresh period for maximum responsiveness
  }

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
