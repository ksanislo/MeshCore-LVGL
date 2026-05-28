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

// Audio path: buzzer on GPIO8, speaker-amp mute on GPIO21. Left floating, the
// amp/buzzer pick up EMI from SPI/touch activity and click. The factory
// firmware drives the buzzer pin and asserts mute (GPIO21 HIGH) at boot.
#ifndef PIN_BUZZER
  #define PIN_BUZZER 8
#endif
// Factory speaker idle state: GPIO14 LOW + GPIO21 HIGH (mute). GPIO21 LOW
// unmutes to play; GPIO14 is the amp's other control line, held low.
#ifndef PIN_SPK_CTL
  #define PIN_SPK_CTL 14
#endif
#ifndef PIN_SPK_MUTE
  #define PIN_SPK_MUTE 21
#endif
// Speaker I2S output pins (BCLK/LRCK/DOUT). Held low so a floating clock/data
// line doesn't pick up SPI EMI and get amplified into the speaker.
#ifndef PIN_I2S_BCLK
  #define PIN_I2S_BCLK 13
#endif
#ifndef PIN_I2S_LRCK
  #define PIN_I2S_LRCK 11
#endif
#ifndef PIN_I2S_DOUT
  #define PIN_I2S_DOUT 12
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
