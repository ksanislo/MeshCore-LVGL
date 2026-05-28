#include "CrowPanelBoard.h"

void CrowPanelBoard::begin() {
  ESP32Board::begin();

  pinMode(PIN_LORA_MIC_MUX, OUTPUT);
  digitalWrite(PIN_LORA_MIC_MUX, LOW);

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  // Quiet the audio path so it doesn't click on touch/SPI EMI (matches the
  // factory firmware's boot state: buzzer driven low, speaker-amp muted).
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_SPK_MUTE, OUTPUT);
  digitalWrite(PIN_SPK_MUTE, HIGH);
}
