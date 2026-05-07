/*
 * Audio alert driver for ESP32-S3-RLCD-4.2 Marine Display.
 *
 * Uses ES8311 codec over I2C (addr 0x18) and I2S for tone generation.
 * Generates a short beep pattern via a sine-wave played through the
 * onboard speaker (MX1.25 header).
 *
 * Also provides display inversion flash via ST7305 commands.
 *
 * I2S pins (from Waveshare board_cfg.txt, FactoryProgram):
 *   MCLK=16, BCLK=9, WS=45, DOUT=8, DIN=10, PA_EN=46
 */

#include "audio_alert.h"
#include "board_pins.h"
#include "screen_config.h"
#include "Display_ST7305.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <esp_log.h>

static const char *TAG = "audio";

// ─── ES8311 registers (subset needed for playback) ──────────────────────────
#define ES8311_REG00_RESET        0x00
#define ES8311_REG01_CLK_MGR      0x01
#define ES8311_REG02_CLK_MGR      0x02
#define ES8311_REG03_CLK_MGR      0x03
#define ES8311_REG04_CLK_MGR      0x04
#define ES8311_REG05_CLK_MGR      0x05
#define ES8311_REG06_CLK_MGR      0x06
#define ES8311_REG07_CLK_MGR      0x07
#define ES8311_REG08_CLK_MGR      0x08
#define ES8311_REG09_SDPIN        0x09
#define ES8311_REG0A_SDPOUT       0x0A
#define ES8311_REG0B_SYSTEM       0x0B
#define ES8311_REG0C_SYSTEM       0x0C
#define ES8311_REG0D_SYSTEM       0x0D
#define ES8311_REG0E_SYSTEM       0x0E
#define ES8311_REG0F_SYSTEM       0x0F
#define ES8311_REG10_SYSTEM       0x10
#define ES8311_REG11_SYSTEM       0x11
#define ES8311_REG12_SYSTEM       0x12
#define ES8311_REG13_SYSTEM       0x13
#define ES8311_REG14_SYSTEM       0x14
#define ES8311_REG32_DAC_VOL      0x32

static bool audio_initialized = false;
static SemaphoreHandle_t beep_mutex = NULL;

// Global buzzer settings (persisted by network_setup via NVS)
uint8_t buzzer_mode      = 1;   // 0=Off, 1=Global, 2=Per-screen
uint8_t alert_volume     = 80;  // default: 80%
uint16_t buzzer_cooldown_sec = 60; // default: 60s
static unsigned long last_buzzer_time = 0;

// External display object from LVGL_Driver.cpp
extern DisplayST7305 g_display;

// ─── I2C helpers ────────────────────────────────────────────────────────────

static void es8311_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        ESP_LOGE(TAG, "I2C write reg 0x%02X FAILED (err=%d)", reg, err);
    }
}

static uint8_t es8311_read_reg(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

// ─── ES8311 init for DAC playback ───────────────────────────────────────────
// Register values taken from the official Espressif es8311 codec driver.
// MCLK must already be running (I2S started) before calling this.
// Clock coefficients for MCLK=4096000Hz (256*16000), Fs=16000Hz:
//   pre_div=1, pre_multi=1, adc_div=1, dac_div=1, fs_mode=0,
//   lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x20

static bool es8311_init_dac(void) {
    // Verify chip presence — read chip ID from reg 0xFD
    uint8_t id = es8311_read_reg(0xFD);
    ESP_LOGI(TAG, "ES8311 chip ID: 0x%02X (expect 0x83)", id);
    if ((id & 0xF0) != 0x80) {
        ESP_LOGE(TAG, "ES8311 not found on I2C addr 0x%02X!", ES8311_ADDR);
        return false;
    }

    // I2C noise immunity
    es8311_write_reg(0x44, 0x08);
    es8311_write_reg(0x44, 0x08);  // Write twice per Espressif recommendation

    // Initial clock manager — clocks off during configuration
    es8311_write_reg(ES8311_REG01_CLK_MGR, 0x30);
    es8311_write_reg(ES8311_REG02_CLK_MGR, 0x00);
    es8311_write_reg(ES8311_REG03_CLK_MGR, 0x10);  // ADC_OSR
    es8311_write_reg(ES8311_REG04_CLK_MGR, 0x10);  // DAC_OSR (reconfigured below)
    es8311_write_reg(ES8311_REG05_CLK_MGR, 0x00);

    // System power
    es8311_write_reg(ES8311_REG0B_SYSTEM, 0x00);
    es8311_write_reg(ES8311_REG0C_SYSTEM, 0x00);
    es8311_write_reg(ES8311_REG10_SYSTEM, 0x1F);  // VMID on, all analog on
    es8311_write_reg(ES8311_REG11_SYSTEM, 0x7F);  // All analog on

    // Enable clock state machine (slave mode, CSM_ON)
    es8311_write_reg(ES8311_REG00_RESET, 0x80);
    delay(10);

    // Turn on all clocks, MCLK from external pin (use_mclk=true)
    // Bit 7=0: MCLK from pin, Bit 6=0: not inverted, Bits 5-0=0x3F: all on
    es8311_write_reg(ES8311_REG01_CLK_MGR, 0x3F);

    // Clock coefficients for MCLK=4096000, Fs=16000
    // REG02: pre_div=1→(1-1)<<5=0, pre_multi=1→datmp=0<<3=0 → 0x00
    es8311_write_reg(ES8311_REG02_CLK_MGR, 0x00);
    // REG03: fs_mode=0<<6, adc_osr=0x10 → 0x10
    es8311_write_reg(ES8311_REG03_CLK_MGR, 0x10);
    // REG04: dac_osr=0x20
    es8311_write_reg(ES8311_REG04_CLK_MGR, 0x20);
    // REG05: (adc_div-1)<<4 | (dac_div-1) = 0x00
    es8311_write_reg(ES8311_REG05_CLK_MGR, 0x00);
    // REG06: bclk_div = 4-1 = 0x03
    es8311_write_reg(ES8311_REG06_CLK_MGR, 0x03);
    // REG07: lrck_h = 0x00
    es8311_write_reg(ES8311_REG07_CLK_MGR, 0x00);
    // REG08: lrck_l = 0xFF
    es8311_write_reg(ES8311_REG08_CLK_MGR, 0xFF);

    // SDP In (I2S → DAC): 16-bit, I2S standard, unmuted (bit6=0)
    es8311_write_reg(ES8311_REG09_SDPIN, 0x0C);
    // SDP Out (ADC → I2S): 16-bit, I2S standard
    es8311_write_reg(ES8311_REG0A_SDPOUT, 0x0C);

    // Output mixer: DAC to HP mixer
    es8311_write_reg(ES8311_REG13_SYSTEM, 0x10);

    // Start DAC — order matches official driver es8311_start()
    es8311_write_reg(ES8311_REG0E_SYSTEM, 0x02);  // DAC power
    es8311_write_reg(ES8311_REG12_SYSTEM, 0x00);  // DAC output enable
    es8311_write_reg(ES8311_REG14_SYSTEM, 0x1A);  // Charge pump + HP driver
    es8311_write_reg(ES8311_REG0D_SYSTEM, 0x01);  // Power up analog (DLL)
    delay(10);

    // DAC config and volume
    es8311_write_reg(0x37, 0x08);                  // DAC ramp rate
    // Apply saved volume (will be overwritten by load_preferences later if needed)
    audio_alert_set_volume(alert_volume);

    // Diagnostic: read back key registers to verify writes
    ESP_LOGI(TAG, "ES8311 reg readback: R00=0x%02X R01=0x%02X R06=0x%02X R09=0x%02X",
             es8311_read_reg(0x00), es8311_read_reg(0x01),
             es8311_read_reg(0x06), es8311_read_reg(0x09));
    ESP_LOGI(TAG, "  R0D=0x%02X R0E=0x%02X R12=0x%02X R13=0x%02X R14=0x%02X R32=0x%02X",
             es8311_read_reg(0x0D), es8311_read_reg(0x0E),
             es8311_read_reg(0x12), es8311_read_reg(0x13),
             es8311_read_reg(0x14), es8311_read_reg(0x32));

    ESP_LOGI(TAG, "ES8311 DAC initialised (MCLK=4.096MHz, Fs=16kHz)");
    return true;
}

// ─── I2S init ───────────────────────────────────────────────────────────────

static void i2s_init_playback(void) {
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = 16000;
    // 32-bit slot width — matches ES8311 coefficient table (bclk_div=4)
    // BCLK = 32 * 2 * 16000 = 1,024,000 Hz; MCLK/BCLK = 4 ✓
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 4;
    i2s_config.dma_buf_len = 256;
    i2s_config.use_apll = true;       // APLL for accurate MCLK
    i2s_config.tx_desc_auto_clear = true;
    i2s_config.fixed_mclk = 0;
    i2s_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // MCLK = 256 * Fs = 4.096 MHz

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num   = I2S_MCLK_PIN;
    pin_config.bck_io_num   = I2S_BCLK_PIN;
    pin_config.ws_io_num    = I2S_WS_PIN;
    pin_config.data_out_num = I2S_DOUT_PIN;
    pin_config.data_in_num  = I2S_DIN_PIN;

    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
    ESP_LOGI(TAG, "I2S playback initialised (16kHz, 32-bit slot, BCLK=1.024MHz)");
}

// ─── Tone generation ────────────────────────────────────────────────────────

// Generate a sine wave beep into the I2S buffer
// Uses 32-bit I2S slots: 16-bit audio data left-shifted into upper half
static void play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    const uint32_t sample_rate = 16000;
    const uint32_t num_samples = (sample_rate * duration_ms) / 1000;
    const float amplitude = 30000.0f;  // Near-max amplitude for alert

    // 32 stereo samples per chunk (32 * 2 = 64 int32_t values)
    int32_t buf[64];
    uint32_t written = 0;
    size_t total_bytes = 0;

    while (written < num_samples) {
        uint32_t chunk = num_samples - written;
        if (chunk > 32) chunk = 32;

        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(written + i) / (float)sample_rate;
            int16_t sample16 = (int16_t)(amplitude * sinf(2.0f * M_PI * freq_hz * t));
            // Left-align 16-bit audio in 32-bit slot (ES8311 extracts upper 16 bits)
            int32_t sample32 = ((int32_t)sample16) << 16;
            buf[i * 2]     = sample32;  // Left
            buf[i * 2 + 1] = sample32;  // Right
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, buf, chunk * 8, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK && written == 0) {
            ESP_LOGE(TAG, "i2s_write failed: %s", esp_err_to_name(err));
        }
        total_bytes += bytes_written;
        written += chunk;
    }
    ESP_LOGI(TAG, "play_tone(%luHz, %lums): wrote %u bytes", freq_hz, duration_ms, total_bytes);
}

// Write silence to flush the DMA buffers
static void play_silence(uint32_t duration_ms) {
    const uint32_t sample_rate = 16000;
    const uint32_t num_samples = (sample_rate * duration_ms) / 1000;
    int32_t buf[64] = {0};
    uint32_t written = 0;

    while (written < num_samples) {
        uint32_t chunk = num_samples - written;
        if (chunk > 32) chunk = 32;
        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, buf, chunk * 8, &bytes_written, portMAX_DELAY);
        written += chunk;
    }
}

// ─── Beep task (one-shot) ───────────────────────────────────────────────────

static void beep_task(void *arg) {
    if (xSemaphoreTake(beep_mutex, 0) != pdTRUE) {
        // Another beep is already in progress
        vTaskDelete(NULL);
        return;
    }

    // Enable PA
    gpio_set_level(PA_EN_PIN, 1);

    // Beep pattern: 200ms tone, 100ms silence, 200ms tone
    play_tone(1000, 200);   // 1kHz beep
    play_silence(100);
    play_tone(1000, 200);   // 1kHz beep
    play_silence(50);       // Flush

    // Disable PA
    gpio_set_level(PA_EN_PIN, 0);

    xSemaphoreGive(beep_mutex);
    vTaskDelete(NULL);
}

// ─── Continuous display flash task ──────────────────────────────────────────

static volatile bool flash_active = false;
static TaskHandle_t flash_task_handle = NULL;

static void flash_task(void *arg) {
    bool inverted = true;  // Display starts in INVON (normal state)
    while (flash_active) {
        inverted = !inverted;
        g_display.setInversion(inverted);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    // Restore normal state (INVON) before exiting
    g_display.setInversion(true);
    flash_task_handle = NULL;
    vTaskDelete(NULL);
}

// ─── Public API ─────────────────────────────────────────────────────────────

void audio_alert_init(void) {
    if (audio_initialized) return;

    beep_mutex = xSemaphoreCreateMutex();

    // Init PA enable pin (active high)
    gpio_config_t pa_conf = {};
    pa_conf.intr_type    = GPIO_INTR_DISABLE;
    pa_conf.mode         = GPIO_MODE_OUTPUT;
    pa_conf.pin_bit_mask = (1ULL << PA_EN_PIN);
    pa_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    pa_conf.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpio_config(&pa_conf);
    gpio_set_level(PA_EN_PIN, 0);  // PA off initially

    // Init I2C — always call Wire.begin (safe to call multiple times on ESP32)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
    delay(10);

    // Init I2S FIRST — this starts MCLK output which ES8311 needs
    i2s_init_playback();
    delay(50);  // Let MCLK stabilise

    // Init ES8311 codec (needs MCLK running)
    if (!es8311_init_dac()) {
        ESP_LOGE(TAG, "ES8311 init failed — audio alerts disabled");
        return;
    }
    delay(50);  // Let codec PLL lock

    audio_initialized = true;
    ESP_LOGI(TAG, "Audio alert system ready (ES8311 + I2S, PA on GPIO%d)", PA_EN_PIN);

    // Startup test beep — confirms hardware works
    ESP_LOGI(TAG, "Starting test beep: PA on...");
    gpio_set_level(PA_EN_PIN, 1);
    delay(50);  // Let PA stabilise
    ESP_LOGI(TAG, "PA pin level: %d", gpio_get_level(PA_EN_PIN));
    play_tone(1000, 300);
    play_silence(100);
    gpio_set_level(PA_EN_PIN, 0);
    ESP_LOGI(TAG, "Startup beep done");
}

void audio_alert_beep(void) {
    if (!audio_initialized || buzzer_mode == 0) return;
    xTaskCreate(beep_task, "beep", 4096, NULL, 3, NULL);
}

void audio_alert_set_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    alert_volume = pct;
    // ES8311 reg 0x32: 0.5dB per step, 0x00 = -95.5dB (mute), 0xC0 = 0dB
    // Map 0% → 0x00 (mute), 1-100% → 0x60..0xC0 (linear over ~48dB range)
    uint8_t reg_val;
    if (pct == 0) {
        reg_val = 0x00;  // mute
    } else {
        // Map 1-100 → 0x60 (96, -48dB) to 0xC0 (192, 0dB)
        reg_val = 0x60 + (uint8_t)((uint16_t)(pct - 1) * 96 / 99);
    }
    if (audio_initialized) {
        es8311_write_reg(ES8311_REG32_DAC_VOL, reg_val);
        ESP_LOGI(TAG, "Volume set: %u%% → reg 0x%02X", pct, reg_val);
    }
}

void display_alert_flash_start(void) {
    if (flash_active) return;  // Already running
    flash_active = true;
    xTaskCreate(flash_task, "flash", 2048, NULL, 2, &flash_task_handle);
}

void display_alert_flash_stop(void) {
    if (!flash_active) return;
    flash_active = false;  // Task will see this, restore display, and self-delete
}

// ─── Unified alert check ────────────────────────────────────────────────────
// Called per-slot from screen_render for each value that has alert thresholds.
// Handles buzzer_mode logic, cooldown, and flash.

#include "screen_render.h"

// Track flash state globally (one flash task for any alert source)
static bool flash_running = false;
// Track whether any slot is in alert for this update cycle
static bool any_alert_this_cycle = false;
static bool cycle_started = false;
// Per-zone flash tracking
static bool zone_flash_active[FLASH_ZONE_COUNT] = {};
static bool zone_flash_phase = false;
static unsigned long zone_flash_timer = 0;
// Remote (non-active screen) alert tracking for icon flash
static bool remote_alert_this_cycle = false;

void check_alert(int32_t value, int16_t alert_low, int16_t alert_high,
                 uint8_t alert_flash, uint8_t alert_buzzer, int screen_idx,
                 int flash_zone) {
    bool in_alert = false;
    if (alert_low != ALERT_OFF && value <= alert_low)   in_alert = true;
    if (alert_high != ALERT_OFF && value >= alert_high)  in_alert = true;

    if (!in_alert) return;

    int active_screen = get_current_screen();

    // Flash: only if this slot enables it AND it's the active screen
    if (alert_flash && screen_idx == active_screen) {
        if (flash_zone == FLASH_ZONE_FULL) {
            // Whole-screen hardware inversion
            if (!flash_running) {
                display_alert_flash_start();
                flash_running = true;
            }
        } else if (flash_zone >= 0 && flash_zone < FLASH_ZONE_COUNT) {
            // Per-zone LVGL flash (applied in screen_render)
            zone_flash_active[flash_zone] = true;
        }
        any_alert_this_cycle = true;
    } else if (screen_idx != active_screen) {
        // Non-active screen has an alert — track for icon flash
        remote_alert_this_cycle = true;
    }

    // Buzzer: check mode and per-slot enable
    bool should_beep = false;
    if (alert_buzzer && buzzer_mode == 1) {
        // Global: beep for any screen's alert
        should_beep = true;
    } else if (alert_buzzer && buzzer_mode == 2) {
        // Per-screen: only beep if this is the active screen
        should_beep = (screen_idx == active_screen);
    }

    if (should_beep) {
        unsigned long now = millis();
        unsigned long cooldown_ms = (unsigned long)buzzer_cooldown_sec * 1000UL;
        if (cooldown_ms == 0 || (now - last_buzzer_time >= cooldown_ms)) {
            last_buzzer_time = now;
            audio_alert_beep();
        }
    }
}

// Call at start of each update cycle to reset flash tracking
void alert_cycle_begin(void) {
    any_alert_this_cycle = false;
    cycle_started = true;
    remote_alert_this_cycle = false;
    // Reset zone flash flags for this cycle
    for (int i = 0; i < FLASH_ZONE_COUNT; i++) zone_flash_active[i] = false;
    // Update zone flash phase (1s toggle)
    unsigned long now = millis();
    if (now - zone_flash_timer >= 1000) {
        zone_flash_timer = now;
        zone_flash_phase = !zone_flash_phase;
    }
}

// Call at end of each update cycle — stop flash if no alert was active
void alert_cycle_end(void) {
    if (cycle_started && !any_alert_this_cycle && flash_running) {
        display_alert_flash_stop();
        flash_running = false;
    }
    cycle_started = false;
}

bool is_zone_flash_on(int zone_id) {
    if (zone_id < 0 || zone_id >= FLASH_ZONE_COUNT) return false;
    return zone_flash_active[zone_id] && zone_flash_phase;
}

bool is_remote_alert_on(void) {
    return remote_alert_this_cycle && zone_flash_phase;
}
