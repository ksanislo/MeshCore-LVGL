#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include "CrowPanelBoard.h"
#include <RTClib.h>
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  #include "CrowPanelLGFX.h"
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"

// GT911 touch + BM8563 RTC share this I2C bus.
#ifndef P_TOUCH_SDA
  #define P_TOUCH_SDA 15
  #define P_TOUCH_SCL 16
#endif

// The on-board RTC is a BM8563 (PCF8563-register-compatible, I2C addr 0x51) on
// the same bus as the GT911 touch. LovyanGFX drives that bus register-level and
// reconfigures the pins for each touch read, and the firmware is single-threaded,
// so we keep the *live* clock on the ESP32 internal RTC (no bus traffic) and only
// reach the hardware RTC at two controlled moments: at boot to seed the live
// clock, and on setCurrentTime() to write it through. We re-assert the I2C pins
// at each access so it coexists with the touch controller.
class CrowPanelRTCClock : public mesh::RTCClock {
  ESP32RTCClock _internal;
  RTC_PCF8563   _hw;
  bool          _hw_ok;
public:
  CrowPanelRTCClock() : _hw_ok(false) {}
  void begin() {
    _internal.begin();
    Wire.begin(P_TOUCH_SDA, P_TOUCH_SCL);
    Wire.beginTransmission(0x51);
    if (Wire.endTransmission() == 0 && _hw.begin(&Wire)) {
      _hw_ok = true;
      bool lp = _hw.lostPower();
      uint32_t t = _hw.now().unixtime();
      Serial.printf("[RTC] begin: detected lostPower=%d hw_unix=%lu\n", (int)lp, (unsigned long)t);
      if (!lp && t > 1700000000UL) _internal.setCurrentTime(t);  // seed live clock from battery RTC
    } else {
      Serial.println("[RTC] begin: NOT detected at 0x51");
    }
  }
  uint32_t getCurrentTime() override { return _internal.getCurrentTime(); }
  void setCurrentTime(uint32_t time) override {
    _internal.setCurrentTime(time);
    if (_hw_ok) {
      Wire.begin(P_TOUCH_SDA, P_TOUCH_SCL);   // re-assert pins (touch reconfigures them per read)
      _hw.adjust(DateTime(time));             // keep the battery RTC in sync
      uint32_t rb = _hw.now().unixtime();     // read back to verify the write actually landed
      Serial.printf("[RTC] set: wrote=%lu readback=%lu %s\n", (unsigned long)time,
                    (unsigned long)rb, (rb + 3 >= time && rb <= time + 3) ? "OK" : "MISMATCH");
    } else {
      Serial.printf("[RTC] set: hw not ok, internal only=%lu\n", (unsigned long)time);
    }
  }
};

extern CrowPanelBoard board;
extern WRAPPER_CLASS radio_driver;

#ifdef HAS_SD_CARD
// SD card shares the LoRa HSPI bus (re-pinned per access). See target.cpp.
void sd_bus_to_sd();
void sd_bus_to_lora();
bool sd_card_begin();
#endif

// Shared HSPI mutex (LoRa radio + SD). Take across any SD bus access span; the
// radio takes it per SPI transaction via its HAL. See target.cpp.
void hspi_lock();
void hspi_unlock();
extern CrowPanelRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
#endif

bool radio_init();
uint32_t radio_get_rng_seed();
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(int8_t dbm);
mesh::LocalIdentity radio_new_identity();
