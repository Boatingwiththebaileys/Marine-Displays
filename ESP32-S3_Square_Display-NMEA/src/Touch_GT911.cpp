#include "Touch_GT911.h"
#include <Preferences.h>

// Runtime GT911 I2C address — set by Touch_Init() auto-detect
uint8_t gt911_addr = GT911_ADDR_PRIMARY;

struct GT911_Touch touch_data = {0};

// Rate-limit noisy I2C error messages (every 5 seconds max)
static unsigned long last_i2c_err_log = 0;
#define I2C_ERR_LOG_INTERVAL_MS 5000

bool I2C_Read_Touch(uint8_t Driver_addr, uint16_t Reg_addr, uint8_t *Reg_data, uint32_t Length);

static bool GT911_ReadProductIdAt(uint8_t addr, uint8_t *id)
{
  return I2C_Read_Touch(addr, ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG, id, 3)
      && id[0] == 0x39 && id[1] == 0x31 && id[2] == 0x31;
}

// Probe both GT911 addresses and switch to the one that returns a valid product ID.
static bool GT911_ProbeAndSelect(bool verbose)
{
  const uint8_t addrs[] = { GT911_ADDR_PRIMARY, GT911_ADDR_SECONDARY };
  uint8_t id[3] = {0};
  for (int i = 0; i < 2; i++) {
    if (GT911_ReadProductIdAt(addrs[i], id)) {
      if (gt911_addr != addrs[i] && verbose) {
        printf("[TOUCH] GT911 switched addr 0x%02X -> 0x%02X\n", gt911_addr, addrs[i]);
      } else if (verbose) {
        printf("[TOUCH] GT911 detected at 0x%02X\n", addrs[i]);
      }
      gt911_addr = addrs[i];
      return true;
    }
  }
  return false;
}

static bool GT911_V4_ResetAndPreferPrimary(void)
{
  uint8_t id[3] = {0};

  for (int attempt = 0; attempt < 2; ++attempt) {
    // Reassert the known-safe CH32 state.
    I2C_Write_EXIO(CH32V003_DIR_REG, CH32V003_DIR_ALL_OUT);
    I2C_Write_EXIO(CH32V003_OUTPUT_REG, CH32V003_OUT_NORMAL);

    // 1. Assert RST first — GT911 enters hardware reset and releases INT.
    //    On warm boot the GT911 drives INT as an output; asserting RST before
    //    driving INT prevents bus contention and allows us to hold INT LOW.
    Set_EXIO(pin_tp_rst(), Low);
    vTaskDelay(pdMS_TO_TICKS(5));  // Let GT911 release INT

    // 2. Drive INT LOW now that GT911 has released it.
    pinMode(GT911_INT_PIN, OUTPUT);
    digitalWrite(GT911_INT_PIN, LOW);
    vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 5 : 10));

    // 3. Release RST — GT911 samples INT (LOW → address 0x5D).
    Set_EXIO(pin_tp_rst(), High);
    vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 12 : 20));  // Hold INT LOW

    // 4. Release INT — GT911 drives it as output.
    digitalWrite(GT911_INT_PIN, LOW);
    pinMode(GT911_INT_PIN, INPUT);
    vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? 50 : 80));  // GT911 boot time

    if (GT911_ReadProductIdAt(GT911_ADDR_PRIMARY, id)) {
      gt911_addr = GT911_ADDR_PRIMARY;
      printf("[TOUCH] GT911 detected at 0x%02X\n", gt911_addr);
      return true;
    }

    printf("[TOUCH] V4 force-0x5D attempt %d did not latch\n", attempt + 1);
  }

  return false;
}


bool I2C_Read_Touch(uint8_t Driver_addr, uint16_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8)); 
  Wire.write((uint8_t)Reg_addr);         
  if ( Wire.endTransmission(true)){
    unsigned long now = millis();
    if (now - last_i2c_err_log >= I2C_ERR_LOG_INTERVAL_MS) {
      last_i2c_err_log = now;
      printf("[TOUCH] I2C Read failed (addr=0x%02X, reg=0x%04X)\r\n", Driver_addr, Reg_addr);
    }
    return false;
  }
  uint32_t got = Wire.requestFrom(Driver_addr, Length);
  if (got != Length) {
    unsigned long now = millis();
    if (now - last_i2c_err_log >= I2C_ERR_LOG_INTERVAL_MS) {
      last_i2c_err_log = now;
      printf("[TOUCH] I2C short read (addr=0x%02X, reg=0x%04X, got=%u, need=%u)\r\n",
             Driver_addr, Reg_addr, (unsigned)got, (unsigned)Length);
    }
    while (Wire.available()) { (void)Wire.read(); }
    return false;
  }
  for (int i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  return true;
}
bool I2C_Write_Touch(uint8_t Driver_addr, uint16_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8));
  Wire.write((uint8_t)Reg_addr);        
  for (int i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if ( Wire.endTransmission(true))
  {
    unsigned long now = millis();
    if (now - last_i2c_err_log >= I2C_ERR_LOG_INTERVAL_MS) {
      last_i2c_err_log = now;
      printf("[TOUCH] I2C Write failed (addr=0x%02X, reg=0x%04X)\r\n", Driver_addr, Reg_addr);
    }
    return false;
  }
  return true;
}

// Upload GT911 configuration for 480x480 touch panel.
// The GT911 on this board has no factory config in flash — all registers read 0.
// This writes a minimal config at every boot so touch works.
static bool GT911_Upload_Config(void) {
    // 184 bytes: registers 0x8047 through 0x80FE
    uint8_t cfg[184];
    memset(cfg, 0, sizeof(cfg));

    // Offset  Register  Description
    cfg[0]  = 0x60;  // 0x8047  Config_Version (must be > 0)
    cfg[1]  = 0xE0;  // 0x8048  X_Output_Max low  (480 = 0x01E0)
    cfg[2]  = 0x01;  // 0x8049  X_Output_Max high
    cfg[3]  = 0xE0;  // 0x804A  Y_Output_Max low  (480 = 0x01E0)
    cfg[4]  = 0x01;  // 0x804B  Y_Output_Max high
    cfg[5]  = 0x05;  // 0x804C  Touch_Number (5 simultaneous points)
    cfg[6]  = 0x05;  // 0x804D  Module_Switch1
                     //   bits 0-1 = 01 (falling edge INT — pulse LOW when data ready)
                     //   bit 2    = 1  (sito: small touch area enable)
                     //   bit 3    = 0  (no X2Y swap)
    cfg[7]  = 0x00;  // 0x804E  Module_Switch2
    cfg[8]  = 0x02;  // 0x804F  Shake_Count (debounce)
    cfg[9]  = 0x08;  // 0x8050  Filter
    cfg[10] = 0x00;  // 0x8051  Large_Touch
    cfg[11] = 0x0F;  // 0x8052  Noise_Reduction
    cfg[12] = 0x28;  // 0x8053  Screen_Touch_Level (40 — touch threshold)
    cfg[13] = 0x1E;  // 0x8054  Screen_Leave_Level (30 — release threshold)
    cfg[14] = 0x03;  // 0x8055  Low_Power_Control
    cfg[15] = 0x05;  // 0x8056  Refresh_Rate (period = 5 + val = 10ms → 100Hz)
    cfg[16] = 0x00;  // 0x8057  X_Threshold
    cfg[17] = 0x00;  // 0x8058  Y_Threshold
    // [18..183] = 0 — GT911 uses auto-detect for driver/sensor channels

    // Checksum: two's complement of sum of all 184 config bytes
    uint8_t sum = 0;
    for (int i = 0; i < 184; i++) sum += cfg[i];
    uint8_t checksum = (~sum) + 1;

    // Write config in 32-byte chunks (safe for all Wire buffer sizes)
    bool ok = true;
    for (int off = 0; off < 184 && ok; off += 32) {
        int len = ((184 - off) < 32) ? (184 - off) : 32;
        if (!I2C_Write_Touch(gt911_addr, (uint16_t)(0x8047 + off),
                             cfg + off, (uint32_t)len)) {
            printf("[TOUCH] Config write failed at offset %d\n", off);
            ok = false;
        }
    }
    if (!ok) return false;

    // Write checksum to 0x80FF
    if (!I2C_Write_Touch(gt911_addr, 0x80FF, &checksum, 1)) {
        printf("[TOUCH] Checksum write failed\n");
        return false;
    }

    // Write config_fresh flag (0x01) to 0x8100 — tells GT911 to apply config
    uint8_t fresh = 0x01;
    if (!I2C_Write_Touch(gt911_addr, 0x8100, &fresh, 1)) {
        printf("[TOUCH] Config fresh flag write failed\n");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Let GT911 process the new config

    // No software reset — the fresh flag (0x01 at 0x8100) is sufficient
    // to make GT911 apply the new config.  Sending 0x02 to 0x8040 causes
    // the GT911 to hang (command register stays stuck at 0x02) and can
    // invalidate subsequent config/interrupt settings.

    // Verify
    uint8_t vfy[6];
    if (I2C_Read_Touch(gt911_addr, 0x8047, vfy, 6)) {
        uint16_t xm = vfy[1] | ((uint16_t)vfy[2] << 8);
        uint16_t ym = vfy[3] | ((uint16_t)vfy[4] << 8);
        printf("[TOUCH] Config verify: ver=0x%02X X_max=%d Y_max=%d touch_num=%d\n",
               vfy[0], xm, ym, vfy[5]);
        if (xm == 480 && ym == 480 && vfy[5] > 0) {
            printf("[TOUCH] GT911 config applied successfully\n");
            return true;
        }
    }
    printf("[TOUCH] GT911 config verification failed\n");
    return false;
}

uint8_t Touch_Init(void) {

  // V4: Waveshare example does NO reset — just keeps TP_RST HIGH via
  // CH32V003 output register (already set in setup()).  Only V3 needs
  // the INT-based address-selection reset.
  if (!is_board_v4()) {
    GT911_Touch_Reset();
    // V3 legacy/stable path: fixed GT911 address and interrupt-driven touch.
    gt911_addr = GT911_ADDR_PRIMARY;
    printf("[TOUCH] GT911 detected at 0x%02X\n", gt911_addr);
    GT911_Read_cfg();
    attachInterrupt(GT911_INT_PIN, Touch_GT911_ISR, interrupt);
    return true;
  } else {
    printf("[TOUCH] V4: resetting GT911 via IO expander (prefer 0x5D)\n");
    GT911_V4_ResetAndPreferPrimary();
  }

  // Check NVS for a cached GT911 address from a previous successful probe.
  // After a soft reset (crash-reboot) the GT911 may not respond to probe
  // because it didn't get a power-cycle. Use the cached address in that case.
  Preferences prefs;
  uint8_t cached_addr = 0;
  if (prefs.begin("settings", true)) {
    cached_addr = prefs.getUChar("touch_addr", 0);
    prefs.end();
  }

  // Auto-detect GT911 I2C address using product-ID read on both addresses.
  // Always probe after reset — forced 0x5D may have failed and GT911 may be at 0x14.
  bool probed = GT911_ProbeAndSelect(true);
  if (!probed) {
    if (!is_board_v4() && (cached_addr == GT911_ADDR_PRIMARY || cached_addr == GT911_ADDR_SECONDARY)) {
      gt911_addr = cached_addr;
      printf("[TOUCH] GT911 not responding, using cached addr 0x%02X\n", gt911_addr);
    } else {
      gt911_addr = GT911_ADDR_PRIMARY;
      printf("[TOUCH] GT911 not found, no cache, defaulting to 0x%02X\n", gt911_addr);
    }
  }

  // Cache the address so crash-reboots use the right one
  if (probed) {
    if (!is_board_v4() || gt911_addr == GT911_ADDR_PRIMARY) {
      if (prefs.begin("settings", false)) {
        if (prefs.getUChar("touch_addr", 0) != gt911_addr) {
          prefs.putUChar("touch_addr", gt911_addr);
          printf("[TOUCH] Cached touch_addr=0x%02X to NVS\n", gt911_addr);
        }
        prefs.end();
      }
    } else {
      printf("[TOUCH] V4 detected non-primary addr 0x%02X; not caching\n", gt911_addr);
    }
  }

  GT911_Read_cfg();

  // Check if GT911 has a valid config; if not, upload one for 480x480
  {
    uint8_t cfg_check[6];
    if (I2C_Read_Touch(gt911_addr, 0x8047, cfg_check, 6)) {
      uint16_t x_max = cfg_check[1] | ((uint16_t)cfg_check[2] << 8);
      uint16_t y_max = cfg_check[3] | ((uint16_t)cfg_check[4] << 8);
      uint8_t  t_num = cfg_check[5];
      printf("[TOUCH] Config check: ver=%d X_max=%d Y_max=%d touch_num=%d\n",
             cfg_check[0], x_max, y_max, t_num);
      if (x_max == 0 || y_max == 0 || t_num == 0) {
        printf("[TOUCH] GT911 has no valid config — uploading 480x480 config\n");
        GT911_Upload_Config();
        // After config upload at 0x14, attempt a recovery reset to 0x5D.
        // The GT911 factory OTP config (ver 79, loaded at 0x5D) is more complete
        // than our minimal upload and is known to work reliably.
        if (is_board_v4() && gt911_addr != GT911_ADDR_PRIMARY) {
          printf("[TOUCH] Recovery reset to prefer 0x5D after config upload\n");
          if (!GT911_V4_ResetAndPreferPrimary()) {
            // Still not at 0x5D — re-probe to confirm actual address
            GT911_ProbeAndSelect(true);
          }
        }
        GT911_Read_cfg(); // re-read to confirm
      }
    }
  }

  // Read extended config for diagnostics
  {
    uint8_t cfg[7];
    if (I2C_Read_Touch(gt911_addr, 0x8047, cfg, 7)) {
      uint16_t x_max = cfg[1] | ((uint16_t)cfg[2] << 8);
      uint16_t y_max = cfg[3] | ((uint16_t)cfg[4] << 8);
      printf("[TOUCH] Config: ver=%d X=%d Y=%d n=%d\n",
             cfg[0], x_max, y_max, cfg[5]);
    }
  }

  // Clear any stale touch data from before reset
  {
    uint8_t status;
    if (I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &status, 1)) {
      if (status != 0) {
        uint8_t clear = 0;
        I2C_Write_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
      }
    }
  }

  // V4: use polling only (Waveshare example doesn't use INT pin).
  // V3: attach interrupt as before.
  if (!is_board_v4()) {
    attachInterrupt(GT911_INT_PIN, Touch_GT911_ISR, interrupt);
  }

  return true;
}
/* Reset controller — V3 boards only (V4 uses Waveshare approach: no reset).
   V3: INT LOW during RST release → address 0x5D */
uint8_t GT911_Touch_Reset(void)
{
  printf("[TOUCH] GT911_Touch_Reset: board=v3, expander=0x%02X\n", g_tca9554_address);

  pinMode(GT911_INT_PIN, OUTPUT);

  // 1. Assert reset
  Set_EXIO(pin_tp_rst(), Low);   // TP_RST LOW via IO expander
  // 2. INT LOW during reset hold (V3: LOW → address 0x5D)
  digitalWrite(GT911_INT_PIN, LOW);
  vTaskDelay(pdMS_TO_TICKS(10));

  // 3. V3: INT stays LOW → address 0x5D
  vTaskDelay(pdMS_TO_TICKS(1));

  // 4. Release reset — GT911 latches address from INT level
  Set_EXIO(pin_tp_rst(), High);
  vTaskDelay(pdMS_TO_TICKS(10));

  // 5. Switch INT to floating input — GT911 drives it as output now
  digitalWrite(GT911_INT_PIN, LOW);
  pinMode(GT911_INT_PIN, INPUT);
  vTaskDelay(pdMS_TO_TICKS(50));

  printf("[TOUCH] GT911_Touch_Reset: done\n");
  return true;
}
void GT911_Read_cfg(void) {
  uint8_t buf[4];
  I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_PRODUCT_ID_REG, buf, 3);
  printf("TouchPad_ID:0x%02x,0x%02x,0x%02x\r\n", buf[0], buf[1], buf[2]);
  I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_CONFIG_REG, buf, 1);
  printf("TouchPad_Config_Version:%d \r\n", buf[0]);
}

// reads sensor and touches
// updates Touch Points, but if not touched, resets all Touch Point Information
uint8_t Touch_Read_Data(void) {
  uint8_t buf[41];
  uint8_t touch_cnt = 0;
  uint8_t clear = 0;
  uint8_t Over = 0xAB;
  size_t i = 0,num=0;
  if (!I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, buf, 1)) {
    if (is_board_v4()) {
      // Runtime recovery (v4 only): if touch moved address or glitched, re-probe both addresses.
      if (GT911_ProbeAndSelect(true)) {
        if (!I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, buf, 1)) {
          return true; // still failing; keep UI responsive and try again next cycle
        }
      } else {
        return true;
      }
    } else {
      return true;
    }
  }
  if ((buf[0] & 0x80) == 0x00) {                                              
    I2C_Write_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);  // No touch data
  } else {
    /* Count of touched points */
    touch_cnt = buf[0] & 0x0F;
    if (touch_cnt > GT911_LCD_TOUCH_MAX_POINTS || touch_cnt == 0) {
      I2C_Write_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
      return true;
    }
    /* Read all points */
    if (!I2C_Read_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG+1, &buf[1], touch_cnt * 8)) {
      I2C_Write_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
      return true; // I2C failed — discard partial read
    }
    /* Clear all */
    I2C_Write_Touch(gt911_addr, ESP_LCD_TOUCH_GT911_READ_XY_REG, &clear, 1);
    // printf(" points=%d \r\n",touch_cnt);
    noInterrupts(); 

    /* Number of touched points */
    if(touch_cnt > GT911_LCD_TOUCH_MAX_POINTS)
        touch_cnt = GT911_LCD_TOUCH_MAX_POINTS;
    touch_data.points = (uint8_t)touch_cnt;
    /* Fill all coordinates */
    for (i = 0; i < touch_cnt; i++) {
      touch_data.coords[i].x = (uint16_t)(((uint16_t)buf[(i * 8) + 3] << 8) + buf[(i * 8) + 2]);               
      touch_data.coords[i].y = (uint16_t)(((uint16_t)buf[(i * 8) + 5] << 8) + buf[(i * 8) + 4]);;
      touch_data.coords[i].strength = (uint16_t)(((uint16_t)buf[(i * 8) + 7] << 8) + buf[(i * 8) + 6]);
    }
    interrupts(); 
    // printf(" points=%d \r\n",touch_data.points);
  }
  return true;
}
void Touch_Loop(void){
  if(Touch_interrupts){
    Touch_interrupts = false;
    example_touchpad_read();
  }
}
uint8_t Touch_Get_XY(uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num) {

  assert(x != NULL);
  assert(y != NULL);
  assert(point_num != NULL);
  assert(max_point_num > 0);
  
  noInterrupts(); 
  /* Count of points */
  if(touch_data.points > max_point_num)
    touch_data.points = max_point_num;
  for (size_t i = 0; i < touch_data.points; i++) {
      x[i] = touch_data.coords[i].x;
      y[i] = touch_data.coords[i].y;
      if (strength) {
          strength[i] = touch_data.coords[i].strength;
      }
  }
  *point_num = touch_data.points;
  /* Invalidate */
  touch_data.points = 0;
  interrupts(); 
  return (*point_num > 0);
}
void example_touchpad_read(void){
  uint16_t touchpad_x[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t touchpad_y[GT911_LCD_TOUCH_MAX_POINTS] = {0};
  uint16_t strength[GT911_LCD_TOUCH_MAX_POINTS]   = {0};
  uint8_t touchpad_cnt = 0;
  Touch_Read_Data();
  uint8_t touchpad_pressed = Touch_Get_XY(touchpad_x, touchpad_y, strength, &touchpad_cnt, GT911_LCD_TOUCH_MAX_POINTS);
  if (touchpad_pressed && touchpad_cnt > 0) {
      // data->point.x = touchpad_x[0];
      // data->point.y = touchpad_y[0];
      // data->state = LV_INDEV_STATE_PR;
      printf("Touch : X=%u Y=%u num=%d\r\n", touchpad_x[0], touchpad_y[0],touchpad_cnt);
  } else {
      // data->state = LV_INDEV_STATE_REL;
  }
}
/*!
    @brief  handle interrupts
*/
uint8_t Touch_interrupts;
void IRAM_ATTR Touch_GT911_ISR(void) {
  Touch_interrupts = true;
}
