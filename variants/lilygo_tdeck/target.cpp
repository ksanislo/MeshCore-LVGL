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
// T-Deck's SD shares LoRa's EXACT pins (SCLK40/MISO38/MOSI41) -- only CS differs -- so unlike
// CrowPanel there is NO bus re-pin: sd_bus_to_*() are no-ops. The caller (SdSvc::Lock /
// ensureMounted, SdCard.cpp) holds hspi_lock() across the whole SD span; on T-Deck that mutex ALSO
// gates the display flush (board_bus_lock) and the radio HAL (BusLockHal), so SD, radio, and the
// LVGL flush all serialize on the one SPI3 bus -- only the active device's CS is asserted at a time,
// so MISO is driven by SD alone during its reads. SdFat drives the bus through the SAME Arduino
// `spi` SPIClass the radio uses (proven to coexist with LovyanGFX's spi_master on this host).
SdFs sd;
volatile uint8_t sd_last_err_code = 0;   // SdFat sdErrorCode() from the last failed mount (UITask diag)
volatile uint8_t sd_last_err_data = 0;
void sd_bus_to_sd()   {}   // same pins as LoRa -> no re-pin needed
void sd_bus_to_lora() {}
static cid_t s_card_cid;          // CID of the mounted card: distinguishes a warm reinsert of the
static bool  s_have_cid = false;  // SAME card (reuse) from a swapped/new card (cold re-init).
bool sd_card_begin() {
  // SOFT remount first (mirror of CrowPanel): a still-present card answers CMD0 with "already
  // initialized", so a destructive end()+begin() would fail (e01) even though the volume is live.
  // If the SAME card (by CID) is still mounted, keep using it -- this is what lets a quick reinsert
  // recover without a reboot. A genuinely cold/new card won't match -> falls through to re-init.
  if (s_have_cid && sd.card()) {
    cid_t cid;
    if (sd.card()->readCID(&cid) && memcmp(&cid, &s_card_cid, sizeof(cid_t)) == 0
        && sd.vol()->fatType() != 0) {
      return true;
    }
  }
  // COLD init: boot, or a genuinely new / power-cycled card. Bounded 2 fast-fail attempts so a
  // missing card stays well under the task-WDT (ensureMounted then backs off + gives up). The init
  // handshake (CMD0/ACMD41) runs at 400 kHz regardless; SD_SCK_MHZ only sets the data-transfer
  // clock. 20 MHz matches CrowPanel; if reads prove flaky on this shared bus, lower it first.
  delay(5);
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
    if (attempt) delay(20);
    sd.end();
    ok = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &spi));
  }
  if (!ok) { sd_last_err_code = sd.sdErrorCode(); sd_last_err_data = sd.sdErrorData(); }
  else     { s_have_cid = sd.card() && sd.card()->readCID(&s_card_cid); }
  return ok;
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

// ---- Trackball (5-way nav) ---------------------------------------------------
// The T-Deck trackball pulses one GPIO per roll-direction (active-low, internal pull-up) plus a
// center click on PIN_USER_BTN. Each direction has a tiny IRAM ISR that just counts edges -- no bus
// access, safe from an ISR. The UI polls net dx/dy via board_trackball_read() and drives
// lv_obj_scroll_by (UITask::pollTrackball). Pins are the standard LilyGo T-Deck mapping; override in
// platformio.ini if a unit differs. Click is polled (not an ISR) so we never touch the BOOT pin's
// edge path. Strong overrides of UITask's weak board_has_trackball()/board_trackball_read().
#ifndef PIN_TRACKBALL_UP
  #define PIN_TRACKBALL_UP    3
  #define PIN_TRACKBALL_DOWN  15
  #define PIN_TRACKBALL_LEFT  1
  #define PIN_TRACKBALL_RIGHT 2
#endif
static volatile int32_t s_tb_up = 0, s_tb_down = 0, s_tb_left = 0, s_tb_right = 0;
static void IRAM_ATTR tb_up_isr()    { s_tb_up++; }
static void IRAM_ATTR tb_down_isr()  { s_tb_down++; }
static void IRAM_ATTR tb_left_isr()  { s_tb_left++; }
static void IRAM_ATTR tb_right_isr() { s_tb_right++; }
static void trackball_init() {
  pinMode(PIN_TRACKBALL_UP,    INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_DOWN,  INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_LEFT,  INPUT_PULLUP);
  pinMode(PIN_TRACKBALL_RIGHT, INPUT_PULLUP);
  pinMode(PIN_USER_BTN,        INPUT_PULLUP);   // center click (= BOOT pin; polled, no ISR)
  attachInterrupt(PIN_TRACKBALL_UP,    tb_up_isr,    FALLING);
  attachInterrupt(PIN_TRACKBALL_DOWN,  tb_down_isr,  FALLING);
  attachInterrupt(PIN_TRACKBALL_LEFT,  tb_left_isr,  FALLING);
  attachInterrupt(PIN_TRACKBALL_RIGHT, tb_right_isr, FALLING);
}
bool board_has_trackball() { return true; }
void board_trackball_read(int16_t* dx, int16_t* dy, bool* pressed) {
  // Net pulses since the last read; reset counters. +dy = rolled DOWN, +dx = rolled RIGHT.
  int32_t up = s_tb_up, down = s_tb_down, left = s_tb_left, right = s_tb_right;
  s_tb_up = s_tb_down = s_tb_left = s_tb_right = 0;
  if (dy) *dy = (int16_t)(down - up);
  if (dx) *dx = (int16_t)(right - left);
  if (pressed) *pressed = (digitalRead(PIN_USER_BTN) == LOW);   // active-low button
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
  trackball_init();      // counters + edge interrupts for the 5-way nav ball (UI polls deltas)
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
