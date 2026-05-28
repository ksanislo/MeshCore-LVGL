#include "CrowPanelBoard.h"

void CrowPanelBoard::begin() {
  ESP32Board::begin();

  pinMode(PIN_LORA_MIC_MUX, OUTPUT);
  digitalWrite(PIN_LORA_MIC_MUX, LOW);

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

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
