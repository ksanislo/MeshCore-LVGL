#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include <helpers/radiolib/CustomSX1262Wrapper.h>
#include <TDeckBoard.h>
#include <helpers/AutoDiscoverRTCClock.h>
#include <helpers/SensorManager.h>
#ifdef DISPLAY_CLASS
  #ifdef MESH_PROXY
    #include "TDeckLGFX.h"           // LVGL build: LovyanGFX ST7789 + GT911 (T-Deck Plus)
  #else
    #include <helpers/ui/ST7789LCDDisplay.h>
    #include <helpers/ui/MomentaryButton.h>
  #endif
#endif
#include "helpers/sensors/EnvironmentSensorManager.h"
#include "helpers/sensors/MicroNMEALocationProvider.h"

extern TDeckBoard board;
extern WRAPPER_CLASS radio_driver;
extern AutoDiscoverRTCClock rtc_clock;
extern EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  extern DISPLAY_CLASS display;
  #ifndef MESH_PROXY
    extern MomentaryButton user_btn;
  #endif
#endif

#ifdef MESH_PROXY
  // Shared SPI bus (display + LoRa + SD) serialization -- see target.cpp. Named hspi_* to match
  // what SdCard.cpp externs (the bus discipline the SD Lock uses), even though the T-Deck bus is FSPI.
  void hspi_lock();
  void hspi_unlock();
  void sd_bus_to_sd();    // no-ops on T-Deck (SD shares LoRa's pins; only CS differs)
  void sd_bus_to_lora();
  bool sd_card_begin();
  // Runtime radio controls the companion app / UI use (mirror CrowPanel's target.cpp).
  void radio_sleep();
  void radio_standby();
  void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
  void radio_set_tx_power(int8_t dbm);
  uint32_t radio_get_rng_seed();
#endif

bool radio_init();
mesh::LocalIdentity radio_new_identity();
