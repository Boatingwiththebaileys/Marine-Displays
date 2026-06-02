#include "TCA9554PWR.h"
#include <Arduino.h>
#include <Preferences.h>

// ets_printf writes to hardware UART0 even before USB CDC Serial is up
extern "C" int ets_printf(const char *fmt, ...);

// Runtime I2C address for the IO expander (default v3=0x20, v4=0x24)
uint8_t g_tca9554_address = TCA9554_ADDR_V3;
static bool g_board_v4 = false;

// V4 shadow registers — CH32V003 register reads are UNRELIABLE (return wrong
// register values after writes to other registers, not just PWM→DIR but also
// OUTPUT reads can return garbage).  We maintain software copies and never read
// direction or output registers from hardware on V4.
static uint8_t ch32v003_dir_shadow = CH32V003_DIR_ALL_OUT;  // 0xFF
static uint8_t ch32v003_out_shadow = CH32V003_OUT_NORMAL;   // 0x3A

bool detect_expander_address()
{
  // Check NVS for a cached board version from a previous successful probe.
  // If found, skip I2C probing (which can leave the expander in a bad state
  // on v4 boards and crash the display).
  Preferences prefs;
  uint8_t cached = 0xFF; // 0xFF = not yet probed
  if (prefs.begin("settings", true)) {
    cached = prefs.getUChar("board_ver", 0xFF);
    prefs.end();
  }

  if (cached == 0) {
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    ets_printf("[BOARD] Cached: v3 (expander 0x20)\r\n");
    return true;
  }
  if (cached == 1) {
    g_tca9554_address = TCA9554_ADDR_V4;
    g_board_v4 = true;
    ets_printf("[BOARD] Cached: v4 (expander 0x24)\r\n");
    return true;
  }

  // First boot: probe both addresses to auto-detect.
  // This may leave the expander in a bad state on v4, but the result is
  // persisted so it only happens once (display may crash → auto-reboot → cached).
  ets_printf("[BOARD] First boot: probing I2C for board version...\r\n");
  Wire.beginTransmission(TCA9554_ADDR_V4);
  uint8_t err_v4 = Wire.endTransmission();
  Wire.beginTransmission(TCA9554_ADDR_V3);
  uint8_t err_v3 = Wire.endTransmission();
  ets_printf("[BOARD] Probe 0x20 (v3): %s, 0x24 (v4): %s\r\n",
             err_v3 == 0 ? "ACK" : "NACK",
             err_v4 == 0 ? "ACK" : "NACK");

  uint8_t detected = 0; // default v3
  if (err_v4 == 0) {
    g_tca9554_address = TCA9554_ADDR_V4;
    g_board_v4 = true;
    detected = 1;
    ets_printf("[BOARD] Auto-detected v4 IO expander at 0x24\r\n");
  } else if (err_v3 == 0) {
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    detected = 0;
    ets_printf("[BOARD] Auto-detected v3 IO expander at 0x20\r\n");
  } else {
    ets_printf("[BOARD] WARNING: No expander found, defaulting to v3 (0x20)\r\n");
    g_tca9554_address = TCA9554_ADDR_V3;
    g_board_v4 = false;
    detected = 0;
  }

  // Cache the result so we never probe again
  if (prefs.begin("settings", false)) {
    prefs.putUChar("board_ver", detected);
    prefs.end();
    ets_printf("[BOARD] Cached board_ver=%d to NVS\r\n", detected);
  }
  return true;
}

bool is_board_v4()
{
  return g_board_v4;
}

uint8_t ch32v003_get_dir_shadow() { return ch32v003_dir_shadow; }
uint8_t ch32v003_get_out_shadow() { return ch32v003_out_shadow; }

uint8_t exio_output_reg()
{
  return g_board_v4 ? CH32V003_OUTPUT_REG : TCA9554_OUTPUT_REG;
}

uint8_t exio_dir_reg()
{
  return g_board_v4 ? CH32V003_DIR_REG : TCA9554_CONFIG_REG;
}

uint8_t exio_input_reg()
{
  return g_board_v4 ? CH32V003_INPUT_REG : TCA9554_INPUT_REG;
}

// Functional pin aliases — V4 pins shifted +1 vs V3 (CH32V003 added EXIO0/charger at bit0)
uint8_t pin_tp_rst()  { return g_board_v4 ? 2 : 1; }  // V3:IO0/bit0, V4:EXIO1/bit1
uint8_t pin_lcd_rst() { return g_board_v4 ? 4 : 3; }  // V3:IO2/bit2, V4:EXIO3/bit3
uint8_t pin_sdcs()    { return g_board_v4 ? 5 : 4; }  // V3:IO3/bit3, V4:EXIO4/bit4

/*****************************************************  Operation register REG   ****************************************************/   
// Safe I2C read — returns false on failure, leaving *out unchanged.
bool I2C_Read_EXIO_safe(uint8_t REG, uint8_t *out)
{
  // V4: direction and output register reads are UNRELIABLE — use shadow copies.
  if (g_board_v4) {
    if (REG == CH32V003_DIR_REG)    { *out = ch32v003_dir_shadow; return true; }
    if (REG == CH32V003_OUTPUT_REG) { *out = ch32v003_out_shadow; return true; }
  }
  Wire.beginTransmission(TCA9554_ADDRESS);
  Wire.write(REG);
  if (Wire.endTransmission() != 0) {
    printf("I2C read: endTransmission failed (addr=0x%02X, reg=0x%02X)\r\n",
           TCA9554_ADDRESS, REG);
    return false;
  }
  if (Wire.requestFrom((uint8_t)TCA9554_ADDRESS, (uint8_t)1) != 1) {
    printf("I2C read: requestFrom failed (addr=0x%02X)\r\n", TCA9554_ADDRESS);
    return false;
  }
  *out = Wire.read();
  return true;
}

uint8_t I2C_Read_EXIO(uint8_t REG)
{
  uint8_t val = 0xFF;
  I2C_Read_EXIO_safe(REG, &val);
  return val;
}
uint8_t I2C_Write_EXIO(uint8_t REG,uint8_t Data)
{
  Wire.beginTransmission(TCA9554_ADDRESS);                
  Wire.write(REG);                                        
  Wire.write(Data);                                       
  uint8_t result = Wire.endTransmission();                  
  if (result != 0) {    
    printf("Data write failure!!!\r\n");
    return -1;
  }
  // V4: update shadow registers so reads never touch hardware
  if (g_board_v4) {
    if (REG == CH32V003_DIR_REG)    ch32v003_dir_shadow = Data;
    if (REG == CH32V003_OUTPUT_REG) ch32v003_out_shadow = Data;
  }
  return 0;                                             
}
/********************************************************** Set EXIO mode **********************************************************/       
void Mode_EXIO(uint8_t Pin,uint8_t State)                 // State: 0=Output, 1=Input (TCA convention; auto-inverts for V4)
{
  uint8_t bitsStatus;
  if (!I2C_Read_EXIO_safe(exio_dir_reg(), &bitsStatus)) return; // bail — don't write garbage
  uint8_t Data;
  // V4 (CH32V003): direction polarity is inverted (1=output, 0=input)
  bool want_input = (State == 1);
  if (g_board_v4) want_input = !want_input; // invert for CH32V003
  if (want_input)
    Data = (0x01 << (Pin-1)) | bitsStatus;   // set bit
  else
    Data = (~(0x01 << (Pin-1))) & bitsStatus; // clear bit
  uint8_t result = I2C_Write_EXIO(exio_dir_reg(),Data); 
  if (result != 0) { 
    printf("I/O Configuration Failure !!!\r\n");
  }
}
void Mode_EXIOS(uint8_t PinState)                         // PinState in TCA convention (0=out,1=in); auto-inverts for V4   
{
  // V4 (CH32V003): direction polarity is inverted (1=output, 0=input)
  uint8_t hw_val = g_board_v4 ? (uint8_t)~PinState : PinState;
  uint8_t result = I2C_Write_EXIO(exio_dir_reg(), hw_val);  
  if (result != 0) {   
    printf("I/O Configuration Failure !!!\r\n");
  }
}
/********************************************************** Read EXIO status **********************************************************/       
uint8_t Read_EXIO(uint8_t Pin)
{
  uint8_t inputBits = I2C_Read_EXIO(exio_input_reg());          
  uint8_t bitStatus = (inputBits >> (Pin-1)) & 0x01; 
  return bitStatus;                                  
}
uint8_t Read_EXIOS(uint8_t REG)
{
  if (REG == 0xFF) REG = exio_input_reg();                // default: input register
  uint8_t inputBits = I2C_Read_EXIO(REG);                     
  return inputBits;     
}

/********************************************************** Set the EXIO output status **********************************************************/  
void Set_EXIO(uint8_t Pin,uint8_t State)                  // Sets the level of Pin without affecting others
{
  uint8_t Data;
  if(State < 2 && Pin < 9 && Pin > 0){  
    uint8_t bitsStatus;
    if (!I2C_Read_EXIO_safe(exio_output_reg(), &bitsStatus)) return; // bail — don't write garbage
    if(State == 1)                                     
      Data = (0x01 << (Pin-1)) | bitsStatus; 
    else if(State == 0)                  
      Data = (~(0x01 << (Pin-1))) & bitsStatus;      
    uint8_t result = I2C_Write_EXIO(exio_output_reg(),Data);  
    if (result != 0) {                         
      printf("Failed to set GPIO!!!\r\n");
    }
  }
  else                                           
    printf("Parameter error, please enter the correct parameter!\r\n");
}
void Set_EXIOS(uint8_t PinState)                          // Set all output pins (writes correct register for V3/V4)
{
  uint8_t result = I2C_Write_EXIO(exio_output_reg(),PinState); 
  if (result != 0) {                  
    printf("Failed to set GPIO!!!\r\n");
  }
}
/********************************************************** Flip EXIO state **********************************************************/  
void Set_Toggle(uint8_t Pin)
{
    uint8_t bitsStatus = Read_EXIO(Pin);                 
    Set_EXIO(Pin,(bool)!bitsStatus); 
}
/********************************************************* Init ***********************************************************/  
void TCA9554PWR_Init(uint8_t PinState)                  // PinState in TCA convention (0=out,1=in). V3 default=0x00 (all out)
{                  
  Mode_EXIOS(PinState);      
}
/********************************************************* V4 Backlight PWM ***********************************************************/  
void backlight_set_pwm(uint8_t duty)
{
  if (!g_board_v4) return; // V3 uses different backlight control
  I2C_Write_EXIO(CH32V003_PWM_REG, duty);
}
