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
