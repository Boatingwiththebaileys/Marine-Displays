#include "Touch_CST820.h"
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// I2C读写
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool I2C_Read_Touch(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr); 
  if ( Wire.endTransmission(true)){
    printf("The I2C transmission fails. - I2C Read\r\n");
    return -1;
  }
  Wire.requestFrom(Driver_addr, Length);
  for (int i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  return 0;
}
bool I2C_Write_Touch(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);       
  for (int i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if ( Wire.endTransmission(true))
  {
    printf("The I2C transmission fails. - I2C Write\r\n");
    return -1;
  }
  return 0;
}
struct CST820_Touch touch_data = {0};

// Non-touch versions of this display have no touch chip. Polling a missing chip
// every LVGL read (10 ms) floods the log, can read garbage as phantom touches and
// keeps loop() too busy for the screen and web server, so once it's found
// missing we only look for it every TOUCH_RECHECK_MS.
static bool touch_present = true;
#define TOUCH_RECHECK_MS 3000
bool Touch_Present(void) { return touch_present; }
static bool touch_chip_answers(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission(true) == 0;
}
// While the chip is missing: true means skip this read.
static bool touch_missing_skip(uint8_t addr) {
  if (touch_present) return false;
  static unsigned long last_check = 0;
  unsigned long now = millis();
  if (now - last_check < TOUCH_RECHECK_MS) return true;
  last_check = now;
  if (!touch_chip_answers(addr)) return true;
  touch_present = true;
  printf("[TOUCH] Touch panel found\n");
  return false;
}
uint8_t Touch_Init(void) {
  pinMode(CST820_INT_PIN, INPUT_PULLUP);
  CST820_Touch_Reset();
  bool found = false;
  for (int i = 0; i < 3 && !found; i++) {
    found = touch_chip_answers(CST820_ADDR);
    if (!found) vTaskDelay(pdMS_TO_TICKS(30));
  }
  if (!found) {
    touch_present = false;
    printf("[TOUCH] No touch panel found - non-touch board? Checking again every %d s\r\n", TOUCH_RECHECK_MS / 1000);
    return false;
  }
  CST820_AutoSleep(false);
  uint16_t Verification = CST820_Read_cfg();

  // attachInterrupt(CST820_INT_PIN, Touch_CST820_ISR, interrupt);

  return true;
}
/* Reset controller */
uint8_t CST820_Touch_Reset(void)
{
  Set_EXIO(EXIO_PIN2,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(EXIO_PIN2,High);
  vTaskDelay(pdMS_TO_TICKS(50));
  return true;
}
uint16_t CST820_Read_cfg(void) {

  uint8_t buf[3]={0};
  I2C_Read_Touch(CST820_ADDR, CST820_REG_Version,buf, 1);
  printf("TouchPad_Version:0x%02x\r\n", buf[0]);
  I2C_Read_Touch(CST820_ADDR, CST820_REG_ChipID, buf, 3);
  printf("ChipID:0x%02x   ProjID:0x%02x   FwVersion:0x%02x \r\n",buf[0], buf[1], buf[2]);

  return true;
}
/*!
    @brief  Fall asleep automatically   
*/
void CST820_AutoSleep(bool Sleep_State) {
  CST820_Touch_Reset();
  uint8_t Sleep_State_Set = 0;
  if(Sleep_State)
    Sleep_State_Set = 0;
  else
    Sleep_State_Set = 0xFF;
  I2C_Write_Touch(CST820_ADDR, CST820_REG_DisAutoSleep, &Sleep_State_Set, 1);
}

// reads sensor and touches
// updates Touch Points
uint8_t Touch_Read_Data(void) {
  uint8_t buf[6] = {0};
  uint8_t touchpad_cnt = 0;
  if (touch_missing_skip(CST820_ADDR)) return true;
  if (I2C_Read_Touch(CST820_ADDR, CST820_REG_GestureID, buf, 6)) return true;  // returns true on failure
  /* touched gesture */
  if (buf[0] != 0x00) 
    touch_data.gesture = (GESTURE)buf[0];
  if (buf[1] != 0x00) {       
   
    noInterrupts();    
    /* Number of touched points */
    touch_data.points = (uint8_t)buf[1];
    if(touch_data.points > CST820_LCD_TOUCH_MAX_POINTS)
        touch_data.points = CST820_LCD_TOUCH_MAX_POINTS;
    /* Fill coordinates */
    touch_data.x = ((buf[2] & 0x0F) << 8) + buf[3];               
    touch_data.y = ((buf[4] & 0x0F) << 8) + buf[5];
      
    interrupts(); 
    // printf(" points=%d \r\n",touch_data.points);
  }
  return true;
}
void example_touchpad_read(void){
  Touch_Read_Data();
  if (touch_data.gesture != NONE ||  touch_data.points != 0x00) {
      // printf("Touch : X=%u Y=%u points=%d\r\n",  touch_data.x , touch_data.y,touch_data.points);
  } else {
      // data->state = LV_INDEV_STATE_REL;
  }
}
void Touch_Loop(void){
  if(Touch_interrupts){
    Touch_interrupts = false;
    example_touchpad_read();
  }
}

/*!
    @brief  handle interrupts
*/
uint8_t Touch_interrupts;
void IRAM_ATTR Touch_CST820_ISR(void) {
  Touch_interrupts = true;
}

/*!
    @brief  get the gesture event name
*/
String Touch_GestureName(void) {
  switch (touch_data.gesture) {
    case NONE:
      return "NONE";
      break;
    case SWIPE_DOWN:
      return "SWIPE DOWN";
      break;
    case SWIPE_UP:
      return "SWIPE UP";
      break;
    case SWIPE_LEFT:
      return "SWIPE LEFT";
      break;
    case SWIPE_RIGHT:
      return "SWIPE RIGHT";
      break;
    case SINGLE_CLICK:
      return "SINGLE CLICK";
      break;
    case DOUBLE_CLICK:
      return "DOUBLE CLICK";
      break;
    case LONG_PRESS:
      return "LONG PRESS";
      break;
    default:
      return "UNKNOWN";
      break;
  }
}
