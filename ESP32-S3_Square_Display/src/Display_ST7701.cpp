#include "Display_ST7701.h"
#include "driver/ledc.h"
#include "TCA9554PWR.h"   // is_board_v4(), backlight_set_pwm(), Set_EXIO, EXIO_PIN2
// ets_printf writes to hardware UART0 (CH343) regardless of USB CDC config
extern "C" int ets_printf(const char *fmt, ...);
#include "drivers/lcd/port/esp_lcd_st7701_interface.h"
#include "esp_lcd_panel_commands.h"
#include "port/esp_io_expander.h"
#include "port/esp_io_expander_tca9554.h"
#include "drivers/bus/port/esp_lcd_panel_io_additions.h"
#include "drivers/lcd/port/esp_panel_lcd_vendor_types.h"
#include "SD_Card.h"
#include "st7701_init.h"
#include <atomic>

// Guard to avoid multiple concurrent RX timing sweeps
static std::atomic<bool> g_rx_sweep_running(false);
static void rx_sweep_task(void *arg);


// VSync tracking globals
static volatile uint32_t g_vsync_count = 0;
static volatile uint32_t g_vsync_max_gap_us = 0;
static volatile uint32_t g_vsync_prev_us = 0;

// Persistent 3-wire control IO (kept for fallback commands if panel IO deleted)
static esp_lcd_panel_io_handle_t g_persistent_ctrl_io = NULL;

// Panel handle definition (instantiate the external declared in header)
esp_lcd_panel_handle_t panel_handle = NULL;

// Initialize RGB panel and allocate framebuffers (if PSRAM available)
void ST7701_Init()
{
  //  RGB
  esp_lcd_rgb_panel_config_t rgb_config = {
    .clk_src = LCD_CLK_SRC_PLL160M,                                                               // Use 160M PLL for more stable timing
    .timings =  {
      .pclk_hz = ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ,
      .h_res = ESP_PANEL_LCD_WIDTH,
      .v_res = ESP_PANEL_LCD_HEIGHT,
      .hsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_HPW,
      .hsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_HBP,
      .hsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_HFP,
      .vsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_VPW,
      .vsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_VBP,
      .vsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_VFP,
      .flags = {
        .pclk_active_neg = ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG,
      },
    },
    .data_width = ESP_PANEL_LCD_RGB_DATA_WIDTH,
    /* NOTE: bits_per_pixel, num_fbs, bounce_buffer_size_px not available in this IDF version - tracked in local vars */
    .psram_trans_align = 64,
    .hsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC,
    .vsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC,
    .de_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DE,
    .pclk_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_PCLK,
    .data_gpio_nums = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 },
    .disp_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DISP,
  };

  // Arduino demo mapping (diagram labels are misleading - these are actually B0-B4, G0-G5, R0-R4)
  rgb_config.data_gpio_nums[0]  = 5;   // D0 / B0
  rgb_config.data_gpio_nums[1]  = 45;  // D1 / B1
  rgb_config.data_gpio_nums[2]  = 48;  // D2 / B2
  rgb_config.data_gpio_nums[3]  = 47;  // D3 / B3
  rgb_config.data_gpio_nums[4]  = 21;  // D4 / B4
  rgb_config.data_gpio_nums[5]  = 14;  // D5 / G0
  rgb_config.data_gpio_nums[6]  = 13;  // D6 / G1
  rgb_config.data_gpio_nums[7]  = 12;  // D7 / G2
  rgb_config.data_gpio_nums[8]  = 11;  // D8 / G3
  rgb_config.data_gpio_nums[9]  = 10;  // D9 / G4
  rgb_config.data_gpio_nums[10] = 9;   // D10 / G5
  rgb_config.data_gpio_nums[11] = 46;  // D11 / R0
  rgb_config.data_gpio_nums[12] = 3;   // D12 / R1
  rgb_config.data_gpio_nums[13] = 8;   // D13 / R2
  rgb_config.data_gpio_nums[14] = 18;  // D14 / R3
  rgb_config.data_gpio_nums[15] = 17;  // D15 / R4

  rgb_config.flags.fb_in_psram = true;

  size_t free_internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t free_spiram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  // bits_per_pixel and num_fbs not in this IDF's esp_lcd_rgb_panel_config_t; track locally
  size_t num_fbs = ESP_PANEL_LCD_RGB_FRAME_BUF_NUM;
  size_t bytes_per_pixel = ESP_PANEL_LCD_RGB_PIXEL_BITS / 8;
  size_t est_fb_bytes = (size_t)ESP_PANEL_LCD_WIDTH * (size_t)ESP_PANEL_LCD_HEIGHT * bytes_per_pixel * num_fbs;

  // Check whether PSRAM appears available at runtime (free_spiram_before>0).
  bool spiram_ok = (free_spiram_before > 0);
  if (!spiram_ok) {
    Serial.println("[DISPLAY] PSRAM not initialized at runtime — falling back to internal RAM for framebuffers");
    rgb_config.flags.fb_in_psram = false;
    num_fbs = 1; // track locally; num_fbs not a field in this IDF version
    // bounce_buffer_size_px not available in this IDF version
  } else {
    
    if (est_fb_bytes > free_spiram_before) {
      Serial.printf("[DISPLAY] Not enough PSRAM for framebuffer (%u bytes needed, %u available). Skipping RGB panel init.\n", (unsigned)est_fb_bytes, (unsigned)free_spiram_before);
      return;
    }
  }

  // If configured, perform a software-SPI (SWSPI) vendor init to match Arduino demo
#if defined(USE_SWSPI_VENDOR_INIT)
  // Initialize SWSPI pins and run sequence
  {
    // Setup pins (CS may be MCU GPIO if defined)
    if (LCD_CS_PIN >= 0) pinMode(LCD_CS_PIN, OUTPUT);
    pinMode(LCD_CLK_PIN, OUTPUT);
    pinMode(LCD_MOSI_PIN, OUTPUT);
    // Idle states
    if (LCD_CS_PIN >= 0) digitalWrite(LCD_CS_PIN, HIGH);
    digitalWrite(LCD_CLK_PIN, LOW);
    digitalWrite(LCD_MOSI_PIN, LOW);

    auto swspi_delay = [](){ delayMicroseconds(2); };
    auto swspi_begin = [&](){ if (LCD_CS_PIN >= 0) digitalWrite(LCD_CS_PIN, LOW); };
    auto swspi_end = [&](){ if (LCD_CS_PIN >= 0) digitalWrite(LCD_CS_PIN, HIGH); };
    auto swspi_write9 = [&](uint16_t val){
      // 9-bit: bit8 = DC (0=cmd,1=data), bits7..0 = value
      for (int i = 8; i >= 0; --i) {
        digitalWrite(LCD_MOSI_PIN, (val >> i) & 1);
        swspi_delay();
        digitalWrite(LCD_CLK_PIN, HIGH);
        swspi_delay();
        digitalWrite(LCD_CLK_PIN, LOW);
      }
    };

    // Helper lambdas
    auto writeCommand = [&](uint8_t c){
      swspi_write9(c & 0xFF); // DC=0 handled below by prefix
    };
    auto writeData = [&](uint8_t d){
      // set DC bit (1) as top bit in 9-bit sequence
      swspi_write9(((uint16_t)1 << 8) | (d & 0xFF));
    };

    // Parse the minimal init array from st7701_type1_init_operations
    extern const uint8_t st7701_type1_init_operations[];
    extern const size_t st7701_type1_init_operations_len;
    size_t i = 0;
    while (i < st7701_type1_init_operations_len) {
      uint8_t op = st7701_type1_init_operations[i++];
      switch (op) {
        case BEGIN_WRITE:
          swspi_begin();
          break;
        case END_WRITE:
          swspi_end();
          break;
        case WRITE_COMMAND_8: {
          if (i >= st7701_type1_init_operations_len) break;
          uint8_t cmd = st7701_type1_init_operations[i++];
          // Send 9-bit command with DC=0 -> we send DC=0 in top bit (bit8=0)
          swspi_write9(cmd);
          break;
        }
        case WRITE_BYTES: {
          if (i >= st7701_type1_init_operations_len) break;
          uint8_t n = st7701_type1_init_operations[i++];
          for (uint8_t k = 0; k < n && i < st7701_type1_init_operations_len; ++k) {
            uint8_t b = st7701_type1_init_operations[i++];
            // WRITE_BYTES assumes data stream after previously sent command; set DC=1
            swspi_write9(((uint16_t)1 << 8) | b);
          }
          break;
        }
        case WRITE_C8_D8: {
          if (i + 1 >= st7701_type1_init_operations_len) break;
          uint8_t c = st7701_type1_init_operations[i++];
          uint8_t d = st7701_type1_init_operations[i++];
          swspi_write9(c); // cmd (DC=0)
          swspi_write9(((uint16_t)1 << 8) | d); // data
          break;
        }
        case WRITE_C8_D16: {
          if (i + 2 >= st7701_type1_init_operations_len) break;
          uint8_t c = st7701_type1_init_operations[i++];
          uint8_t d1 = st7701_type1_init_operations[i++];
          uint8_t d2 = st7701_type1_init_operations[i++];
          swspi_write9(c); // cmd
          swspi_write9(((uint16_t)1 << 8) | d1);
          swspi_write9(((uint16_t)1 << 8) | d2);
          break;
        }
        case WRITE_DATA_8: {
          if (i >= st7701_type1_init_operations_len) break;
          uint8_t d = st7701_type1_init_operations[i++];
          swspi_write9(((uint16_t)1 << 8) | d);
          break;
        }
        case WRITE_DATA_16: {
          if (i + 1 >= st7701_type1_init_operations_len) break;
          uint8_t hi = st7701_type1_init_operations[i++];
          uint8_t lo = st7701_type1_init_operations[i++];
          swspi_write9(((uint16_t)1 << 8) | hi);
          swspi_write9(((uint16_t)1 << 8) | lo);
          break;
        }
        case DELAY: {
          if (i >= st7701_type1_init_operations_len) break;
          uint8_t ms = st7701_type1_init_operations[i++];
          delay(ms);
          break;
        }
        default:
          // skip unknown op
          break;
      }
    }
    // ensure CS released
    if (LCD_CS_PIN >= 0) digitalWrite(LCD_CS_PIN, HIGH);
  }
#endif


  // Create 3-wire SPI control-panel IO so vendor init sequence can be sent before RGB takeover.
  esp_lcd_panel_io_handle_t ctrl_io = NULL;

  esp_io_expander_handle_t io_expander = NULL;
  // Create IO expander handle for TCA9554 (I2C should already be initialized)
  esp_err_t rxe = esp_io_expander_new_i2c_tca9554(I2C_NUM_0, TCA9554_ADDRESS, &io_expander);
  if (rxe != ESP_OK) {
    Serial.printf("[DISPLAY] esp_io_expander_new_i2c_tca9554 failed: %s\n", esp_err_to_name(rxe));
    io_expander = NULL; // continue, but 3-wire SPI requires an expander for CS
  } else {
    // The io_expander library now reads (not resets) the TCA9554/CH32V003 registers,
    // so the hardware state set by TCA9554PWR_Init() in setup() is preserved.
    if (is_board_v4()) {
      // V4 (CH32V003): Direction already set correctly by TCA9554PWR_Init (0x3A).
      // BEE_EN is configured as input → buzzer safe. Don't override with all-output.
      // Just ensure the output pins that matter are HIGH.
      uint32_t out_mask = (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5); // EXIO1,3,4,5
      esp_io_expander_set_dir(io_expander, out_mask, IO_EXPANDER_OUTPUT);
      esp_io_expander_set_level(io_expander, out_mask, 1); // all output pins HIGH
    } else {
      // V3 (TCA9554): Set PIN6 LOW (buzzer safety) then all pins to output.
      esp_io_expander_set_level(io_expander, (1 << (EXIO_PIN6 - 1)), 0);
      esp_err_t dir_err = esp_io_expander_set_dir(io_expander, 0xFF, IO_EXPANDER_OUTPUT);
      if (dir_err != ESP_OK) {
        Serial.printf("[DISPLAY] io_expander set_dir failed: %s\n", esp_err_to_name(dir_err));
      }
    }
  }

  esp_lcd_panel_io_3wire_spi_config_t spi_cfg3 = {
    .line_config = {
      .cs_io_type = IO_TYPE_GPIO,
      .cs_gpio_num = LCD_CS_PIN,
      .scl_io_type = IO_TYPE_GPIO,
      .scl_gpio_num = LCD_CLK_PIN,
      .sda_io_type = IO_TYPE_GPIO,
      .sda_gpio_num = LCD_MOSI_PIN,
      .io_expander = io_expander,
    },
    .expect_clk_speed = 200000, // slow init clock
    .spi_mode = 0,
    .lcd_cmd_bytes = 1,
    .lcd_param_bytes = 1,
    .flags = {.use_dc_bit = 1, .dc_zero_on_data = 1, .lsb_first = 0, .cs_high_active = 0, .del_keep_cs_inactive = 0},
  };

  esp_err_t rc = esp_lcd_new_panel_io_3wire_spi(&spi_cfg3, &ctrl_io);
  if (rc != ESP_OK || ctrl_io == NULL) {
    Serial.printf("[DISPLAY] esp_lcd_new_panel_io_3wire_spi failed: %s\n", esp_err_to_name(rc));
    // Fall back to creating RGB panel directly (previous behavior)
    rc = esp_lcd_new_rgb_panel(&rgb_config, &panel_handle);
    if (rc != ESP_OK || panel_handle == NULL) {
      Serial.printf("[DISPLAY] esp_lcd_new_rgb_panel failed: %s (panel_handle=%p)\n", esp_err_to_name(rc), panel_handle);
      panel_handle = NULL;
      size_t free_internal_after_fail = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      size_t free_spiram_after_fail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      Serial.printf("[DISPLAY] Free internal after failed panel: %u bytes, PSRAM: %u bytes\n", (unsigned)free_internal_after_fail, (unsigned)free_spiram_after_fail);
      return;
    }
  } else {

    // Prepare minimal device config with vendor config pointing to our rgb_config
    esp_panel_lcd_vendor_config_t vendor_cfg = {};
    vendor_cfg.rgb_config = &rgb_config;
#if defined(USE_SWSPI_VENDOR_INIT)
    // since we performed vendor init manually via SWSPI above, disable the
    // driver's vendor IO multiplex init to avoid re-sending init commands
    vendor_cfg.flags.enable_io_multiplex = 0;
    // Also avoid the driver's vendor init entirely (we already ran it by SWSPI)
    vendor_cfg.init_cmds = NULL;
    vendor_cfg.init_cmds_size = 0;
#else
    vendor_cfg.flags.enable_io_multiplex = 1;
#endif

    esp_lcd_panel_dev_config_t dev_cfg = {};
    dev_cfg.reset_gpio_num = -1; // reset handled via expander
    dev_cfg.vendor_config = &vendor_cfg;
    // Ensure device config reflects our RGB settings so vendor-aware init sees correct pixel format
    dev_cfg.bits_per_pixel = ESP_PANEL_LCD_RGB_PIXEL_BITS;
    dev_cfg.color_space = ESP_LCD_COLOR_SPACE_RGB; // Match Arduino demo exactly

    rc = esp_lcd_new_panel_st7701_rgb(ctrl_io, &dev_cfg, &panel_handle);
    if (rc != ESP_OK || panel_handle == NULL) {
      Serial.printf("[DISPLAY] esp_lcd_new_panel_st7701_rgb failed: %s\n", esp_err_to_name(rc));
      // Clean up ctrl_io
      esp_lcd_panel_io_del(ctrl_io);
      ctrl_io = NULL;
      // Fall back to plain RGB panel
      rc = esp_lcd_new_rgb_panel(&rgb_config, &panel_handle);
      if (rc != ESP_OK || panel_handle == NULL) {
        Serial.printf("[DISPLAY] esp_lcd_new_rgb_panel failed: %s (panel_handle=%p)\n", esp_err_to_name(rc), panel_handle);
        panel_handle = NULL;
        size_t free_internal_after_fail = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_spiram_after_fail = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        Serial.printf("[DISPLAY] Free internal after failed panel: %u bytes, PSRAM: %u bytes\n", (unsigned)free_internal_after_fail, (unsigned)free_spiram_after_fail);
        return;
      }
      // Fallback panel created successfully - verify basic RGB mapping & pixel format
      Serial.println("[DISPLAY] Fallback RGB panel created - verifying rgb_config");
      Serial.printf("[DISPLAY] rgb_config bits_per_pixel (compile-time)=%d\n", (int)ESP_PANEL_LCD_RGB_PIXEL_BITS);
      Serial.print("[DISPLAY] rgb data_gpio_nums: ");
      for (int _i=0; _i<16; _i++) {
        Serial.printf("%d%s", rgb_config.data_gpio_nums[_i], _i==15 ? "\n" : ", ");
      }
      if (ESP_PANEL_LCD_RGB_PIXEL_BITS != 16) Serial.println("[DISPLAY][WARN] rgb_config bits_per_pixel != 16");
    } else {

      // Quick test: re-create 3-wire control IO so we can explicitly send Sleep-Out and Display-On
      // (Some vendor init paths delete the 3-wire IO; recreate to ensure panel is powered on)
      esp_lcd_panel_io_handle_t ctrl_io_re = NULL;
      esp_err_t rc2 = esp_lcd_new_panel_io_3wire_spi(&spi_cfg3, &ctrl_io_re);
      if (rc2 == ESP_OK && ctrl_io_re != NULL) {
        // Configure read timing BEFORE attempting any reads (dummy clocks + settle delay)
        esp_lcd_3wire_set_rx_timing(ctrl_io_re, 8, 100);  // 8 dummy clocks, 100us settle
        
        esp_lcd_panel_io_tx_param(ctrl_io_re, LCD_CMD_SLPOUT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_io_tx_param(ctrl_io_re, LCD_CMD_DISPON, NULL, 0);
        // Give controller a moment
        vTaskDelay(pdMS_TO_TICKS(50));

// Always attempt to verify controller MADCTL/COLMOD regardless of vendor init path
        // Attempt to read MADCTL/COLMOD; if not RGB666, patch them
        uint8_t readbuf[2] = {0};
        esp_err_t rr1 = esp_lcd_panel_io_rx_param(ctrl_io_re, LCD_CMD_MADCTL, readbuf, 1);
        esp_err_t rr2 = esp_lcd_panel_io_rx_param(ctrl_io_re, LCD_CMD_COLMOD, readbuf + 1, 1);

        if (rr1 != ESP_OK || rr2 != ESP_OK || readbuf[1] != 0x60) {
          uint8_t madctl = 0x60;
          esp_lcd_panel_io_tx_param(ctrl_io_re, LCD_CMD_MADCTL, &madctl, 1);
          uint8_t colmod = 0x60;
          esp_lcd_panel_io_tx_param(ctrl_io_re, LCD_CMD_COLMOD, &colmod, 1);
          vTaskDelay(pdMS_TO_TICKS(20));
        }

        // Keep the control IO persistent for fallback commands
        g_persistent_ctrl_io = ctrl_io_re;

#if !REMOVE_PANEL_TESTS
        // Start RX timing sweep in the background to avoid blocking init. This sweep is guarded
        // so it cannot run concurrently if already started elsewhere.
        if (g_persistent_ctrl_io) {
          BaseType_t xrc = xTaskCreate(rx_sweep_task, "rx_sweep", 4096, g_persistent_ctrl_io, tskIDLE_PRIORITY+1, NULL);
          if (xrc != pdPASS) Serial.printf("[SWEEP] Failed to create rx_sweep task: %d\n", (int)xrc);
        }
#else
        // Panel tests disabled
#endif

      } else {
        Serial.printf("[DISPLAY] Recreate control IO failed: %s\n", esp_err_to_name(rc2));
      }
    }
  }

  // Note: esp_lcd_rgb_panel_event_callbacks_t / esp_lcd_rgb_panel_register_event_callbacks not
  // available in this IDF version. Vsync tracking via register_event_callbacks omitted.
  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);

  // Log free heap after successful panel creation

}
// Reset helper stub for ST7701 (no-op if hardware reset handled elsewhere)
void ST7701_Reset()
{
  if (is_board_v4()) {
    // V4 boards: CH32V003 IO expander at 0x24.
    // TCA9554PWR_Init has already set direction (0x3A) and output latch (0xFF).
    // Now pulse LCD_RST (EXIO3/bit3) via the fixed driver functions.
    Serial.printf("[DISPLAY] ST7701_Reset: v4 board, pulsing LCD_RST via pin %d\n", pin_lcd_rst());
    Set_EXIO(pin_lcd_rst(), Low);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_EXIO(pin_lcd_rst(), High);
    vTaskDelay(pdMS_TO_TICKS(120));
    Serial.println("[DISPLAY] ST7701_Reset: v4 done");
  } else {
    // V3 boards: toggle EXIO_PIN3 (expander reset)
    Serial.printf("[DISPLAY] ST7701_Reset: v3 board (expander 0x%02X), toggling EXIO_PIN3\n", g_tca9554_address);
    Mode_EXIO(EXIO_PIN3, 0); // 0 = output
    Set_EXIO(EXIO_PIN3, Low);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_EXIO(EXIO_PIN3, High);
    vTaskDelay(pdMS_TO_TICKS(50));
    Serial.println("[DISPLAY] ST7701_Reset: v3 done");
  }
}

bool example_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data)
{
  uint32_t now = (uint32_t)esp_timer_get_time();
  if (g_vsync_prev_us != 0) {
    uint32_t delta = now - g_vsync_prev_us;
    if (delta > g_vsync_max_gap_us) g_vsync_max_gap_us = delta;
  }
  g_vsync_prev_us = now;
  g_vsync_count = g_vsync_count + 1;
  return false;
}

uint32_t get_vsync_count() {
  return g_vsync_count;
}

uint32_t get_vsync_max_gap_us() {
  return g_vsync_max_gap_us;
}

void reset_vsync_stats() {
  g_vsync_count = 0;
  g_vsync_max_gap_us = 0;
  g_vsync_prev_us = 0;
}

#if !REMOVE_PANEL_TESTS
// Background task used to run an automated RX timing sweep. It attempts a matrix of
// dummy clock counts and settle delays, forcing SLPOUT/DISPON for each variant,
// then logs MADCTL/COLMOD read results. Guarded by g_rx_sweep_running to avoid
// multiple simultaneous executions.
static void rx_sweep_task(void *arg)
{
  esp_lcd_panel_io_handle_t io = (esp_lcd_panel_io_handle_t)arg;
  if (!io) {
    Serial.println("[SWEEP] No control IO provided; aborting");
    vTaskDelete(NULL);
    return;
  }
  if (g_rx_sweep_running.exchange(true)) {
    Serial.println("[SWEEP] Another sweep already running; aborting");
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[SWEEP] Starting 3-wire RX timing sweep");

  // Diagnostic precaution: disable SD (assert EXIO_PIN4 LOW) to avoid shared-SPI conflicts
  // while we exercise 3-wire reads. SD may be mounted; pulsing/holding CS low prevents
  // the SD card from driving MISO/SDA during our reads.
  Serial.println("[SWEEP] Asserting EXIO_PIN4 LOW to disable SD during sweep");
  SD_D3_Dis(); // drives EXIO_PIN4 LOW
  vTaskDelay(pdMS_TO_TICKS(20));

  int phases[] = {0, 1}; // 0 = normal, 1 = invert sampling phase
  int dummy_vals[] = {0, 1, 2, 4, 8, 16, 32, 64};
  uint32_t settle_vals[] = {0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
  uint8_t readbuf[2] = {0, 0};

  for (size_t pi = 0; pi < sizeof(phases)/sizeof(phases[0]); ++pi) {
    int phase = phases[pi];
    esp_lcd_3wire_set_rx_phase(io, phase);
    Serial.printf("[SWEEP] phase=%d (0=normal,1=invert)\n", phase);

    for (size_t di = 0; di < sizeof(dummy_vals)/sizeof(dummy_vals[0]); ++di) {
      for (size_t si = 0; si < sizeof(settle_vals)/sizeof(settle_vals[0]); ++si) {
        uint8_t dummy = (uint8_t)dummy_vals[di];
        uint32_t settle = settle_vals[si];

        esp_lcd_3wire_set_rx_timing(io, dummy, settle);

        // Force controller out of sleep and onto the display on state
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(50));


        esp_err_t rr1 = esp_lcd_panel_io_rx_param(io, LCD_CMD_MADCTL, readbuf, 1);
        esp_err_t rr2 = esp_lcd_panel_io_rx_param(io, LCD_CMD_COLMOD, readbuf + 1, 1);

        Serial.printf("[SWEEP] phase=%d dummy=%u settle=%uus MADCTL=0x%02x (%s) COLMOD=0x%02x (%s)\n",
                      phase, (unsigned)dummy, (unsigned)settle, (unsigned)readbuf[0], esp_err_to_name(rr1), (unsigned)readbuf[1], esp_err_to_name(rr2));

        // small delay between tests
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
  }

  // Clear overrides (restore default behavior)
  esp_lcd_3wire_set_rx_timing(io, 0xFF, 0xFFFFFFFF);
  esp_lcd_3wire_set_rx_phase(io, -1);

  Serial.println("[SWEEP] Completed 3-wire RX timing sweep");

  // Re-enable SD (release EXIO_PIN4) now that sweep is finished
  Serial.println("[SWEEP] Releasing EXIO_PIN4 HIGH to re-enable SD (if present)");
  SD_D3_EN();
  vTaskDelay(pdMS_TO_TICKS(10));

  // Probe SDA directly to see whether the panel ever drives the line low
  esp_err_t pr = esp_lcd_3wire_sda_probe(io);
  Serial.printf("[SWEEP] SDA probe -> %s\n", esp_err_to_name(pr));

  esp_err_t hr = esp_lcd_3wire_sda_hold_test(io, 500);
  Serial.printf("[SWEEP] SDA hold test -> %s\n", esp_err_to_name(hr));

  g_rx_sweep_running = false;
  vTaskDelete(NULL);
}

#else
// Stub when panel tests are removed so any references still link
static void rx_sweep_task(void *arg) { (void)arg; Serial.println("[SWEEP] Disabled by REMOVE_PANEL_TESTS"); vTaskDelete(NULL); }
#endif

// SDA diagnostics disabled: return not supported
esp_err_t Display_SDA_Probe() {
  Serial.println("[DISPLAY] SDA probe disabled by REMOVE_PANEL_TESTS");
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t Display_SDA_Hold_Test(uint32_t hold_ms) {
  Serial.println("[DISPLAY] SDA hold test disabled by REMOVE_PANEL_TESTS");
  (void)hold_ms;
  return ESP_ERR_NOT_SUPPORTED;
}


// Fill the panel with a solid 16-bit color (RGB565) using a single line buffer.
static void LCD_FillSolid(uint16_t color565)
{
  if (!panel_handle) return;
  size_t line_pixels = ESP_PANEL_LCD_WIDTH;
  Serial.printf("[DISPLAY] LCD_FillSolid: color=0x%04x lines=%u\n", color565, (unsigned)ESP_PANEL_LCD_HEIGHT);
  size_t line_bytes = line_pixels * sizeof(uint16_t);
  uint16_t *line = (uint16_t*)heap_caps_malloc(line_bytes, MALLOC_CAP_INTERNAL);
  if (!line) {
    // fallback to PSRAM if internal allocation fails
    line = (uint16_t*)heap_caps_malloc(line_bytes, MALLOC_CAP_SPIRAM);
    if (!line) {
      ESP_LOGE("DISPLAY", "LCD_FillSolid: failed to allocate line buffer (%u bytes)", (unsigned)line_bytes);
      return;
    }
  }
  for (size_t i = 0; i < line_pixels; ++i) line[i] = color565;

  for (int y = 0; y < ESP_PANEL_LCD_HEIGHT; ++y) {
    esp_err_t r = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, ESP_PANEL_LCD_WIDTH, y + 1, line);
    if (r != ESP_OK) {
      ESP_LOGE("DISPLAY", "LCD_FillSolid: draw_bitmap failed at row %d: %s", y, esp_err_to_name(r));
      break;
    }
    // Small yield to keep watchdog happy on long loops
    if ((y & 31) == 0) vTaskDelay(pdMS_TO_TICKS(1));
  }

  free(line);
}

void LCD_Init() {
  // Ensure expander-controlled backlight is off briefly to emulate a power-cycle.
  // EXIO_PIN2 = bit1: on v3 this is the BL pin; on v4 bit1 = TP_RST (GT911 reset).
  // Only do this on v3 — touching it on v4 puts GT911 into hardware reset and
  // corrupts the CH32V003 shadow register before Touch_Init runs.
  if (!is_board_v4()) {
    Mode_EXIO(EXIO_PIN2, 0); // ensure BL pin is output
    Set_EXIO(EXIO_PIN2, Low);
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ST7701_Reset();
  ST7701_Init();

  // Restore backlight state
  Backlight_Init();

  if (panel_handle) {
    esp_err_t rc = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (rc != ESP_OK && g_persistent_ctrl_io != NULL) {
      esp_lcd_panel_io_tx_param(g_persistent_ctrl_io, LCD_CMD_SLPOUT, NULL, 0);
      vTaskDelay(pdMS_TO_TICKS(120));
      esp_lcd_panel_io_tx_param(g_persistent_ctrl_io, LCD_CMD_DISPON, NULL, 0);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

}


void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint8_t* color) {
  Xend = Xend + 1;      // esp_lcd_panel_draw_bitmap: x_end End index on x-axis (x_end not included)
  Yend = Yend + 1;      // esp_lcd_panel_draw_bitmap: y_end End index on y-axis (y_end not included)
  if (Xend >= ESP_PANEL_LCD_WIDTH)
    Xend = ESP_PANEL_LCD_WIDTH;
  if (Yend >= ESP_PANEL_LCD_HEIGHT)
    Yend = ESP_PANEL_LCD_HEIGHT;
   
  if (!panel_handle) {
    ESP_LOGE("DISPLAY", "LCD_addWindow: panel_handle is NULL, skipping draw (%d,%d)-(%d,%d)", Xstart, Ystart, Xend, Yend);
    return;
  }
  esp_lcd_panel_draw_bitmap(panel_handle, Xstart, Ystart, Xend, Yend, color);                     // x_end End index on x-axis (x_end not included)
}


// backlight
uint8_t LCD_Backlight = 50;
void Backlight_Init()
{
  // Configure LEDC timer/channel only if an MCU pin is provided.
  if (LCD_Backlight_PIN >= 0) {
    ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = (ledc_timer_bit_t)Resolution,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = Frequency,
      .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
      .gpio_num = LCD_Backlight_PIN,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = (ledc_channel_t)PWM_Channel,
      .intr_type = LEDC_INTR_DISABLE,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0
    };
    ledc_channel_config(&ledc_channel);
  }

  // Ensure expander BL_EN (EXIO2) is configured as output and set initial state
  Mode_EXIO(EXIO_PIN2, 0);
  Set_Backlight(LCD_Backlight); // initialize to saved brightness (will toggle EXIO2)
}

void Set_Backlight(uint8_t Light)
{
  if (Light > Backlight_MAX) {
    printf("Set Backlight parameters in the range of 0 to 100 \r\n");
    return;
  }
  uint32_t max_duty = (1u << Resolution) - 1u;
  uint32_t duty = (uint32_t)Light * max_duty / Backlight_MAX;
  if (LCD_Backlight_PIN >= 0) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_Channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)PWM_Channel);
  }
  if (is_board_v4()) {
    // V4: CH32V003 PWM register, inverted (0=full bright, 255=off).
    // Map Light 0-100 → PWM 255-0.
    uint8_t pwm = (Light == 0) ? 255 : (uint8_t)((uint32_t)(100 - Light) * 255 / 100);
    backlight_set_pwm(pwm);
  } else {
    // V3: BL_EN power gate on EXIO2.
    if (Light > 0) Set_EXIO(EXIO_PIN2, High);
    else           Set_EXIO(EXIO_PIN2, Low);
  }
}

