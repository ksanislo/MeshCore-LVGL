#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "helpers/ESP32Board.h"

// Pin 45 muxes the LoRa SPI bus (GPIO9/10) vs I2S mic input.
// Drive LOW once at boot to claim the bus for the on-board SX1262.
#ifndef PIN_LORA_MIC_MUX
  #define PIN_LORA_MIC_MUX 45
#endif

// Backlight is active-high, driven by PWM (LEDC) for brightness control.
#ifndef PIN_TFT_BL
  #define PIN_TFT_BL 38
#endif
#ifndef BL_LEDC_CHANNEL
  #define BL_LEDC_CHANNEL 7
#endif
#ifndef BL_DEFAULT_DUTY
  #define BL_DEFAULT_DUTY 128   // ~50% of 255
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

// GT911 capacitive touch control lines (the I2C data pins live in target.h as
// P_TOUCH_SDA/SCL). We reset the controller ourselves in begin() with correct
// INT/RST timing, so RST is -1 in the LovyanGFX touch config.
#ifndef PIN_TOUCH_INT
  #define PIN_TOUCH_INT 47
#endif
#ifndef PIN_TOUCH_RST
  #define PIN_TOUCH_RST 48
#endif

// Touch I2C bus (GT911) -- also carries the PCF8563 RTC and the INA219 battery
// monitor. (Mirrors target.h's P_TOUCH_SDA/SCL; kept here so the board .cpp can
// re-assert the bus for the INA219 without pulling in target.h.)
#ifndef PIN_TOUCH_SDA
  #define PIN_TOUCH_SDA 15
#endif
#ifndef PIN_TOUCH_SCL
  #define PIN_TOUCH_SCL 16
#endif

// INA219 battery monitor on the touch I2C bus. Bidirectional current/power: bus
// voltage = pack voltage; shunt voltage / R_shunt = current (signed: + into pack
// = charging, - = system draw). Override the address / shunt value per the wiring.
#ifndef INA219_ADDR
  #define INA219_ADDR 0x40
#endif
#ifndef INA219_SHUNT_MILLIOHM
  #define INA219_SHUNT_MILLIOHM 100   // 0.1 ohm (typical breakout); set to the real shunt
#endif

// microSD slot (SPI). Wired to its own pin set, separate from the display (SPI2)
// and LoRa (HSPI) buses.
#ifndef PIN_SD_SCK
  #define PIN_SD_SCK  5
  #define PIN_SD_MISO 4
  #define PIN_SD_MOSI 6
  #define PIN_SD_CS   7
#endif

class CrowPanelBoard : public ESP32Board {
public:
  void begin();

  // Battery via the INA219 on the touch I2C bus (no internal ADC tap exists on this board).
  // Gated by the user's power-monitor selection -- both return 0 when no monitor is configured
  // (so we never poll a sensor the user doesn't have) or when the sensor doesn't ACK.
  uint16_t getBattMilliVolts();   // pack voltage (mV)
  int32_t  getBattCurrentMa();    // signed current (mA): + charging, - discharging
  void setPowerMonitor(uint8_t t) { _power_monitor = t; }  // 0 None, 1 INA219 (from prefs)

  const char* getManufacturerName() const {
    return "Elecrow CrowPanel Advance 3.5";
  }
private:
  uint8_t _power_monitor = 0;   // 0 = None (don't touch I2C), 1 = INA219
};
