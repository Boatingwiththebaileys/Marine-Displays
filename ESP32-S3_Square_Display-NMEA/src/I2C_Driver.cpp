#include "I2C_Driver.h"                    

void I2C_Init(void) {
  // I2C bus recovery: firmware upload via esptool resets the ESP32 mid-transaction,
  // leaving GT911 / CH32V003 I2C state machines stuck (SDA held LOW waiting for
  // more clock pulses).  Send up to 9 SCL pulses to let the slave finish the byte,
  // then a STOP condition to return the bus to idle.  This is harmless when the
  // bus is already idle (SDA HIGH) — the check exits immediately.
  {
    const int SDA = I2C_SDA_PIN;  // GPIO15
    const int SCL = I2C_SCL_PIN;  // GPIO7
    pinMode(SCL, OUTPUT);
    digitalWrite(SCL, HIGH);
    pinMode(SDA, INPUT_PULLUP);
    delayMicroseconds(10);
    if (digitalRead(SDA) == LOW) {
      // SDA stuck — clock out up to 9 bits so the slave releases SDA
      for (int i = 0; i < 9; i++) {
        digitalWrite(SCL, LOW);
        delayMicroseconds(5);
        digitalWrite(SCL, HIGH);
        delayMicroseconds(5);
        if (digitalRead(SDA) == HIGH) break;
      }
      // STOP condition: SDA LOW→HIGH while SCL HIGH
      pinMode(SDA, OUTPUT);
      digitalWrite(SDA, LOW);
      delayMicroseconds(5);
      digitalWrite(SCL, HIGH);
      delayMicroseconds(5);
      digitalWrite(SDA, HIGH);
      delayMicroseconds(5);
    }
  }
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000); // 400kHz — matches Waveshare CH32V003 driver speed
}
// 寄存器地址为 8 位的
bool I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr); 
  if ( Wire.endTransmission(true)){
    // Rate-limit this log to avoid flooding serial
    static unsigned long last_log = 0;
    unsigned long now = millis();
    if (now - last_log > 5000) {
      last_log = now;
      printf("I2C Read NACK (addr=0x%02X reg=0x%02X)\r\n", Driver_addr, Reg_addr);
    }
    return -1;
  }
  Wire.requestFrom(Driver_addr, Length);
  for (int i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  return 0;
}
bool I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
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