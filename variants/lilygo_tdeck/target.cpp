#include <Arduino.h>
#include "target.h"

TDeckBoard board;

// ---- SPI bus ----------------------------------------------------------------
// T-Deck shares ONE SPI bus across the display (ST7789), LoRa (SX1262) and SD. The radio/SD use
// the default Arduino SPIClass = HSPI = bus 2 = SPI3_HOST on the S3 (the host the working non-LVGL
// build uses). For the shared LVGL display, TDeckLGFX sets cfg.spi_host = SPI3_HOST to match, so
// both drive the SAME controller (serialized by the bus mutex). Do NOT force a different host here.
#if defined(P_LORA_SCLK)
  static SPIClass spi;
#endif

#ifdef MESH_PROXY
#include <SdFat.h>
// ---- Shared-bus serialization (LVGL only) -----------------------------------
// The radio (mesh core 0) and the LVGL flush + SD (UI core 1) take this mutex so a display
// flush / SD access never lands mid radio-transaction on the shared bus. The radio brackets
// each SPI transaction via BusLockHal; the flush via board_bus_lock() (UITask::disp_flush_cb);
// SD via SdCard.cpp's Lock (hspi_lock + sd_bus_to_sd). Named hspi_* to match SdCard.cpp's externs.
static SemaphoreHandle_t hspi_mutex = nullptr;
static void hspi_mutex_ensure() { if (!hspi_mutex) hspi_mutex = xSemaphoreCreateMutex(); }
void hspi_lock()   { if (hspi_mutex) xSemaphoreTake(hspi_mutex, portMAX_DELAY); }
void hspi_unlock() { if (hspi_mutex) xSemaphoreGive(hspi_mutex); }

// RadioLib HAL that brackets each SPI transaction with the shared bus mutex (mirror of
// CrowPanel's HspiLockHal). Holds are brief (one transaction), so radio timing is unaffected.
class BusLockHal : public ArduinoHal {
public:
  BusLockHal(SPIClass& spi, SPISettings settings) : ArduinoHal(spi, settings) {}
  void spiBeginTransaction() override { hspi_lock(); ArduinoHal::spiBeginTransaction(); }
  void spiEndTransaction()   override { ArduinoHal::spiEndTransaction(); hspi_unlock(); }
};
static BusLockHal lora_hal(spi, RADIOLIB_DEFAULT_SPI_SETTINGS);
RADIO_CLASS radio = new Module(&lora_hal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);

// ---- Shared SD (SdFat) -------------------------------------------------------
// T-Deck's SD shares LoRa's exact pins (SCLK40/MISO38/MOSI41) -- only CS differs -- so unlike
// CrowPanel there is NO bus re-pin: sd_bus_to_*() are no-ops, and the caller's hspi_lock() (held
// across SdCard::Lock) serializes SD vs radio vs display on the one bus.
SdFs sd;
volatile uint8_t sd_last_err_code = 0;   // SdFat sdErrorCode() from the last failed mount (UITask diag)
volatile uint8_t sd_last_err_data = 0;
void sd_bus_to_sd()   {}
void sd_bus_to_lora() {}
bool sd_card_begin() {
  // v1: SD mount is STUBBED OFF on T-Deck. The card shares the SPI bus with the display (LovyanGFX,
  // IDF spi_master) + radio, and SdFat reads on that shared bus aren't reliable yet (chat content
  // failed to load) -- and a real sd.begin() with no card wedged the boot. Return false so the
  // message store falls back to RAM. Restoring SD = the deferred shared-bus SD bring-up.
  (void)sd; (void)sd_last_err_code; (void)sd_last_err_data;
  return false;
}

// ---- LVGL board hooks (strong overrides of UITask's weak defaults) -----------
#ifndef PIN_TDECK_BL
  #define PIN_TDECK_BL 42   // ST7789 backlight (PIN_TFT_LEDA_CTL)
#endif
extern "C" void board_set_backlight(uint8_t duty) {
  static bool inited = false;
  if (!inited) { ledcSetup(7, 5000, 8); ledcAttachPin(PIN_TDECK_BL, 7); inited = true; }
  ledcWrite(7, duty);
}
int  board_touch_int_pin()      { return 16; }   // GT911 INT (T-Deck Plus) -- VERIFY
bool board_display_bus_shared() { return true; }
void board_bus_lock()           { hspi_lock(); }
void board_bus_unlock()         { hspi_unlock(); }

// Shared I2C bus lock (GT911 touch + keyboard + RTC all on Wire, SDA18/SCL8). Strong override of
// UITask's weak no-op so the INT-driven touch task (which calls board_i2c_lock around getTouch) and
// the keyboard read below actually serialize -- otherwise the high-prio touch task preempts the
// keyboard read mid-transaction and corrupts the bus (the crash-on-interaction).
static SemaphoreHandle_t s_i2c_mtx = nullptr;
bool board_i2c_lock(uint32_t timeout_ms) {
  if (!s_i2c_mtx) return true;   // pre-init (single-threaded) -> no contention
  return xSemaphoreTake(s_i2c_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void board_i2c_unlock() { if (s_i2c_mtx) xSemaphoreGive(s_i2c_mtx); }

// Physical keyboard: the T-Deck's BlackBerry keypad is an ESP32-C3 at I2C 0x55 (Wire, SDA18/SCL8) that
// returns the next pressed ASCII char (0 = none). Read under the I2C lock so it serializes with the
// touch task. UITask drives an LVGL keypad indev from this and suppresses the on-screen keyboard.
#define TDECK_KBD_ADDR 0x55
bool board_has_physical_kbd() { return true; }
int  board_kbd_read() {
  if (!board_i2c_lock(20)) return 0;
  int r = 0;
  if (Wire.requestFrom(TDECK_KBD_ADDR, 1) == 1) { int c = Wire.read(); r = (c > 0) ? c : 0; }
  board_i2c_unlock();
  return r;
}

// ---- Runtime radio controls (mirror CrowPanel target.cpp) --------------------
void radio_sleep()   { radio.sleep(); }
void radio_standby() { radio.standby(); }
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq); radio.setSpreadingFactor(sf); radio.setBandwidth(bw); radio.setCodingRate(cr);
}
void radio_set_tx_power(int8_t dbm) { radio.setOutputPower(dbm); }
uint32_t radio_get_rng_seed()       { return radio.random(0x7FFFFFFF); }

#elif defined(P_LORA_SCLK)
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif  // MESH_PROXY

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
  #ifndef MESH_PROXY
    MomentaryButton user_btn(PIN_USER_BTN, 1000, true);
  #endif
#endif

bool radio_init() {
#ifdef MESH_PROXY
  hspi_mutex_ensure();   // must exist before the radio HAL's first locked transaction (std_init below)
  if (!s_i2c_mtx) s_i2c_mtx = xSemaphoreCreateMutex();   // before any touch/keyboard/RTC I2C access
#endif
  fallback_clock.begin();
  rtc_clock.begin(Wire);
  Wire.begin(18, 8);

#if defined(P_LORA_SCLK)
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng); // create new random identity
}
