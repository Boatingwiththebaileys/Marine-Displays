/*
 * LVGL driver for the ESP32-S3-RLCD-4.2.
 * Sets up LVGL with double-buffered full framebuffer in PSRAM,
 * a flush callback that converts 16-bit color to 1-bit monochrome,
 * and runs the LVGL timer handler on Core 0.
 */

#include "LVGL_Driver.h"
#include "Display_ST7305.h"
#include "board_pins.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

static const char *TAG = "LVGL_Driver";

// Global display instance (constructed here, accessed externally if needed)
DisplayST7305 g_display;

static lv_disp_draw_buf_t disp_buf;
static lv_disp_drv_t      disp_drv;
static SemaphoreHandle_t   lvgl_mux = nullptr;

// ─── Tick callback ──────────────────────────────────────────────────────────

static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ─── Flush callback: 16-bit → 1-bit threshold ──────────────────────────────

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_map) {
    uint16_t *buf = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            // Threshold: values below 0x7FFF → black, above → white
            uint8_t color = (*buf < 0x7FFF) ? 0 : 0xFF;
            g_display.setPixel(x, y, color);
            buf++;
        }
    }
    g_display.flush();
    lv_disp_flush_ready(drv);
}

// ─── LVGL task (runs on Core 0) ─────────────────────────────────────────────

static void lvgl_task(void *arg) {
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (LVGL_Lock(-1)) {
            delay_ms = lv_timer_handler();
            LVGL_Unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ─── Public API ─────────────────────────────────────────────────────────────

bool LVGL_Lock(int timeout_ms) {
    TickType_t ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, ticks) == pdTRUE;
}

void LVGL_Unlock() {
    xSemaphoreGive(lvgl_mux);
}

void LVGL_Init() {
    // Initialise display hardware
    g_display.init();

    // LVGL core init
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();

    // Allocate two full-framebuffers in PSRAM (16-bit color)
    int pixels = RLCD_WIDTH * RLCD_HEIGHT;
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
        pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
        pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    assert(buf1);
    assert(buf2);

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, pixels);

    // Display driver
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = RLCD_WIDTH;
    disp_drv.ver_res      = RLCD_HEIGHT;
    disp_drv.flush_cb     = lvgl_flush_cb;
    disp_drv.full_refresh = 1;            // Full-frame push required by ST7305
    disp_drv.draw_buf     = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "LVGL display driver registered (%dx%d)", RLCD_WIDTH, RLCD_HEIGHT);

    // Tick timer
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = &lvgl_tick_cb;
    tick_args.name     = "lvgl_tick";
    esp_timer_handle_t tick_timer = nullptr;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // LVGL handler task on Core 0
    xTaskCreatePinnedToCore(lvgl_task, "LVGL", 8 * 1024, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "LVGL task started on Core 0");
}
