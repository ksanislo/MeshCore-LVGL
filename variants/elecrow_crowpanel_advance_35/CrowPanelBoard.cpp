#include "CrowPanelBoard.h"

void CrowPanelBoard::begin() {
  ESP32Board::begin();

  pinMode(PIN_LORA_MIC_MUX, OUTPUT);
  digitalWrite(PIN_LORA_MIC_MUX, LOW);

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
}
