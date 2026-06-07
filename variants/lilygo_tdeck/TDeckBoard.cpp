#include <Arduino.h>
#include "TDeckBoard.h"

#ifdef MESH_PROXY
extern "C" void board_set_backlight(uint8_t);   // defined in target.cpp (LVGL build); lights the panel
#endif

uint32_t deviceOnline = 0x00;

void TDeckBoard::begin() {
  
  ESP32Board::begin();
  
  // Enable peripheral power
  pinMode(PIN_PERF_POWERON, OUTPUT);
  digitalWrite(PIN_PERF_POWERON, HIGH);

#ifdef MESH_PROXY
  // LVGL build: light the backlight at boot to a sane default (matches UITask's 153 fallback). The
  // board owns the boot level here -- as on CrowPanel -- so display_brightness=0 ("board default")
  // shows a lit screen; UITask overrides it once the user sets a brightness. board_set_backlight()
  // (target.cpp) owns the LEDC channel.
  board_set_backlight(153);
#endif

  // Configure user button
  pinMode(PIN_USER_BTN, INPUT);

  // Configure LoRa Pins
  pinMode(P_LORA_MISO, INPUT_PULLUP);
  // pinMode(P_LORA_DIO_1, INPUT_PULLUP);

  #ifdef P_LORA_TX_LED
    digitalWrite(P_LORA_TX_LED, HIGH); // inverted pin for SX1276 - HIGH for off
  #endif

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_DEEPSLEEP) {
    long wakeup_source = esp_sleep_get_ext1_wakeup_status();
    if (wakeup_source & (1 << P_LORA_DIO_1)) {
      startup_reason = BD_STARTUP_RX_PACKET; // received a LoRa packet (while in deep sleep)
    }

    rtc_gpio_hold_dis((gpio_num_t)P_LORA_NSS);
    rtc_gpio_deinit((gpio_num_t)P_LORA_DIO_1);
  }
}