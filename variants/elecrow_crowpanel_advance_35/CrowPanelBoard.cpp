#include "CrowPanelBoard.h"

// Deterministic GT911 power-on reset. The controller latches BOTH its I2C slave
// address and its startup state from the INT line level at the moment RST is
// released. LovyanGFX's Touch_GT911::init() pulses only RST and leaves INT
// floating, so it occasionally comes up ACKing I2C but never asserting data-ready
// -- touch is then dead with no recovery (the second lgfx.init() in
// ui_task.begin() early-returns once _inited latched true). We do the reset here,
// before display.begin(), with the datasheet INT/RST timing and INT held HIGH at
// the RST rising edge to select addr 0x14 (matching CrowPanelLGFX's touch config).
static void gt911_reset() {
  pinMode(PIN_TOUCH_RST, OUTPUT);
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);    // assert reset
  digitalWrite(PIN_TOUCH_INT, LOW);
  delay(11);                           // hold in reset (>10 ms)
  digitalWrite(PIN_TOUCH_INT, HIGH);   // INT high at release -> I2C addr 0x14
  delayMicroseconds(120);              // >100 us of INT setup before release
  digitalWrite(PIN_TOUCH_RST, HIGH);   // release reset; address/state latch now
  delay(6);                            // hold INT through the latch window (>5 ms)
  digitalWrite(PIN_TOUCH_INT, LOW);
  delay(55);                           // firmware boot (>50 ms) before first I2C
  pinMode(PIN_TOUCH_INT, INPUT);       // hand INT back as the interrupt line
}

void CrowPanelBoard::begin() {
  ESP32Board::begin();

  // Bring the touch controller up deterministically before anything probes it.
  gt911_reset();

  pinMode(PIN_LORA_MIC_MUX, OUTPUT);
  digitalWrite(PIN_LORA_MIC_MUX, LOW);

  // Backlight at ~60% via high-frequency (inaudible) PWM instead of full-on.
  // A low PWM frequency would whine; 30 kHz is well above hearing.
  ledcSetup(BL_LEDC_CHANNEL, 30000, 8);          // 30 kHz, 8-bit
  ledcAttachPin(PIN_TFT_BL, BL_LEDC_CHANNEL);
  ledcWrite(BL_LEDC_CHANNEL, BL_DEFAULT_DUTY);   // ~60%

  // Quiet the audio path so it doesn't click on touch/SPI EMI. Matches the
  // factory firmware's idle state: buzzer low, GPIO14 low, speaker-amp muted
  // (GPIO21 high). None of these should float.
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_SPK_CTL, OUTPUT);
  digitalWrite(PIN_SPK_CTL, LOW);
  pinMode(PIN_SPK_MUTE, OUTPUT);
  digitalWrite(PIN_SPK_MUTE, HIGH);
  // Park the I2S output lines low (no clock/data) so the amp can't pick up
  // EMI from the display SPI bus and turn it into audible noise.
  pinMode(PIN_I2S_BCLK, OUTPUT);  digitalWrite(PIN_I2S_BCLK, LOW);
  pinMode(PIN_I2S_LRCK, OUTPUT);  digitalWrite(PIN_I2S_LRCK, LOW);
  pinMode(PIN_I2S_DOUT, OUTPUT);  digitalWrite(PIN_I2S_DOUT, LOW);
}

// Backlight brightness hook used by the LVGL Settings tab (which is variant-
// agnostic and only knows this weak symbol). Drives the LEDC duty set up above.
extern "C" void board_set_backlight(uint8_t duty) {
  ledcWrite(BL_LEDC_CHANNEL, duty);
}

// ---- INA219 battery monitor -------------------------------------------------
// On the shared touch I2C bus (PIN_TOUCH_SDA/SCL), alongside the GT911 and the
// PCF8563 RTC. We take the same board_i2c_lock and re-assert the bus pins per
// access (touch reconfigures them on every read), exactly like CrowPanelRTCClock.
extern bool board_i2c_lock(uint32_t timeout_ms);
extern void board_i2c_unlock();

static bool ina219_read16(uint8_t reg, uint16_t& out) {
  if (!board_i2c_lock(100)) return false;
  Wire.end();                                   // drop whatever state the LGFX touch driver left on
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);     // I2C port 0 (it shares these pins); clean re-init
  Wire.setClock(100000);                        // 100 kHz: robust on the multi-drop touch bus
  bool ok = false;
  for (int attempt = 0; attempt < 4 && !ok; attempt++) {   // contended bus -> retry a few times
    Wire.beginTransmission(INA219_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(true) != 0) { delay(1); continue; }   // STOP (not repeated-start)
    if (Wire.requestFrom((int)INA219_ADDR, 2) != 2) { delay(1); continue; }
    out = ((uint16_t)Wire.read() << 8) | (uint8_t)Wire.read();
    ok = true;
  }
  board_i2c_unlock();
  return ok;
}

uint16_t CrowPanelBoard::getBattMilliVolts() {
  if (_power_monitor != 1) return 0;             // no monitor selected -> don't touch I2C
  uint16_t raw;
  if (!ina219_read16(0x02, raw)) return 0;       // bus-voltage reg; 0 = sensor absent/no ACK
  return (uint16_t)((raw >> 3) * 4);             // bits[15:3], 4 mV/LSB
}

int32_t CrowPanelBoard::getBattCurrentMa() {
  if (_power_monitor != 1) return 0;
  uint16_t raw;
  if (!ina219_read16(0x01, raw)) return 0;        // shunt-voltage reg (signed), 10 uV/LSB
  // I[mA] = Vshunt[uV] / Rshunt[mOhm] = (raw*10) / R_mOhm. Negated: this board's shunt IN+/IN- are
  // wired so the raw sign is inverted vs our convention -> make + = charging, - = discharging.
  return -(((int32_t)(int16_t)raw * 10) / (int32_t)INA219_SHUNT_MILLIOHM);
}
