#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "helpers/ESP32Board.h"

// Pin 45 muxes the LoRa SPI bus (GPIO9/10) vs I2S mic input.
// Drive LOW once at boot to claim the bus for the on-board SX1262.
#ifndef PIN_LORA_MIC_MUX
  #define PIN_LORA_MIC_MUX 45
#endif

// Backlight is active-high.
#ifndef PIN_TFT_BL
  #define PIN_TFT_BL 38
#endif

class CrowPanelBoard : public ESP32Board {
public:
  void begin();

  uint16_t getBattMilliVolts() {
    // CrowPanel battery ADC tap not documented in Elecrow's source files.
    // Return 0 until the schematic is checked.
    return 0;
  }

  const char* getManufacturerName() const {
    return "Elecrow CrowPanel Advance 3.5";
  }
};
