#include <Arduino.h>
#ifdef HAS_SD_CARD
  #include <SdFat.h>
#endif
#include "target.h"

CrowPanelBoard board;

static SPIClass lora_spi(HSPI);

// ---- Shared HSPI ownership --------------------------------------------------
// The LoRa radio and the SD card share ONE HSPI controller (lora_spi), on
// different pins. Today the firmware is single-threaded so accesses never
// overlap; once the mesh/radio moves to its own core (core 0) the UI core can
// drive the SD while the mesh core drives the radio. This mutex serializes the
// two so a re-pin (sd_bus_to_*) never lands mid radio-transaction.
//
// Radio side: a custom RadioLibHal (HspiLockHal, below) takes/gives this mutex
// per SPI transaction. SD side: SdSvc::Lock / the lv_fs driver take it around
// the whole open..close span via hspi_lock()/hspi_unlock(). The RX path is
// ISR-flag-based and does no SPI, so the mutex is never used from an ISR.
static SemaphoreHandle_t hspi_mutex = nullptr;

static void hspi_mutex_ensure() {
  if (!hspi_mutex) hspi_mutex = xSemaphoreCreateMutex();
}
void hspi_lock()   { if (hspi_mutex) xSemaphoreTake(hspi_mutex, portMAX_DELAY); }
void hspi_unlock() { if (hspi_mutex) xSemaphoreGive(hspi_mutex); }

// RadioLib HAL that brackets each SPI transaction with the shared HSPI mutex.
// Thin subclass: the base does the real SPI work; we only add the lock around
// it. Holds are brief (one transaction, ~µs uncontended), so radio timing is
// unaffected. Confined to the variant -- no edits to shared src/ or RadioLib.
class HspiLockHal : public ArduinoHal {
public:
  HspiLockHal(SPIClass& spi, SPISettings settings) : ArduinoHal(spi, settings) {}
  void spiBeginTransaction() override {
    hspi_lock();
    ArduinoHal::spiBeginTransaction();
  }
  void spiEndTransaction() override {
    ArduinoHal::spiEndTransaction();
    hspi_unlock();
  }
};

static HspiLockHal lora_hal(lora_spi, RADIOLIB_DEFAULT_SPI_SETTINGS);

#ifdef HAS_SD_CARD
// Shared SD filesystem (SdFat, FAT16/32 + exFAT). Consumers (message store, and
// later emoji/map/sound caches) use this handle. The card shares the LoRa HSPI
// controller on its own pins, so we re-pin the bus to the SD for each access and
// restore it to LoRa after. (Single-threaded loop: an SD access completes before
// the mesh touches the radio again.)
// end() first: begin() won't re-pin an already-initialized bus (it just warns).
SdFs sd;

// NOTE: callers must hold hspi_lock() across the sd_bus_to_sd()..sd_bus_to_lora()
// span (SdSvc::Lock and the lv_fs driver do) so a re-pin never races the radio.
void sd_bus_to_sd()   { lora_spi.end(); lora_spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS); }
void sd_bus_to_lora() { lora_spi.end(); lora_spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI); }

volatile uint8_t  sd_last_err_code = 0;  // SdFat sdErrorCode() from the last failed cold mount (diag)
volatile uint8_t  sd_last_err_data = 0;
static   cid_t    s_card_cid;            // CID of the mounted card -- distinguishes a warm reinsert of
static   bool     s_have_cid = false;    // the SAME card (reuse) from a genuinely swapped/new card (re-init)
volatile uint32_t sd_dbg_soft = 0;       // soft-remount decision bits (diagnostic): 1=have_cid&card
                                         // 2=readCID ok  4=CID match  8=fatType!=0  0x10=reused

bool sd_card_begin() {
  sd_bus_to_sd();
  // SOFT remount first. JTAG proved a runtime re-mount of a still-present card answers CMD0 with
  // R1=0x00 ("not idle" = already initialized): the card was never power-cycled (a quick reinsert, or
  // a spurious removal detection), so a destructive sd.end()+sd.begin() can't reset it and fails (e01)
  // -- yet the card is perfectly usable and its filesystem is still live. So don't tear it down: if
  // the SAME card (matched by CID) is present and the volume is still mounted, just keep using it.
  // A genuinely cold/new card won't answer CMD10 (or its CID won't match) -> falls through to re-init.
  sd_dbg_soft = 0;
  if (s_have_cid && sd.card()) {
    sd_dbg_soft |= 1;
    cid_t cid;
    bool cidok = sd.card()->readCID(&cid);
    if (cidok) sd_dbg_soft |= 2;
    bool match = cidok && memcmp(&cid, &s_card_cid, sizeof(cid_t)) == 0;
    if (match) sd_dbg_soft |= 4;
    if (sd.vol()->fatType() != 0) sd_dbg_soft |= 8;
    if (match && (sd_dbg_soft & 8)) {
      sd_dbg_soft |= 0x10;
      sd_bus_to_lora();
      return true;   // same card, FS live -> reuse the existing init, no CMD0 reset needed
    }
  }
  // COLD init: boot, or a genuinely new / power-cycled card. Bounded 2 attempts (each fails fast, so
  // we stay well under the 5s task-WDT even with no card). On success, remember the CID for next time.
  delay(5);
  bool ok = false;
  for (int attempt = 0; attempt < 2 && !ok; attempt++) {
    if (attempt) delay(20);
    sd.end();
    ok = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &lora_spi));
  }
  if (!ok) { sd_last_err_code = sd.sdErrorCode(); sd_last_err_data = sd.sdErrorData(); }
  else { s_have_cid = sd.card() && sd.card()->readCID(&s_card_cid); }
  sd_bus_to_lora();
  return ok;
}
#endif  // HAS_SD_CARD
RADIO_CLASS radio = new Module(&lora_hal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);

WRAPPER_CLASS radio_driver(radio, board);

CrowPanelRTCClock rtc_clock;
#if ENV_INCLUDE_GPS
// Optional GPS on UART1 (routed to the rear plug's IO43/IO44 via setPins in
// EnvironmentSensorManager::initBasicGPS). No-op if no module answers at boot.
MicroNMEALocationProvider gps(Serial1, &rtc_clock);
EnvironmentSensorManager sensors(gps);
#else
EnvironmentSensorManager sensors;
#endif

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
#endif

// Strong overrides of the weak RTC hooks in the shared companion app + UITask. They
// route generic clock policy to this board's battery-backed BM8563 (see target.h).
bool board_rtc_valid_at_boot()     { return rtc_clock.validAtBoot(); }
void board_rtc_arm_hw_write(bool on){ rtc_clock.armHwWrite(on); }
bool board_rtc_reseed_from_hw()    { return rtc_clock.reseedFromHw(); }
extern "C" void board_rtc_set_time(uint32_t epoch) { rtc_clock.setCurrentTime(epoch); }

// Shared GT911-touch / BM8563-RTC I2C bus lock (see target.h). The interrupt-driven touch
// task and the RTC both serialize their bus span on this so neither corrupts the other.
static SemaphoreHandle_t s_i2c_mtx = nullptr;
bool board_i2c_lock(uint32_t timeout_ms) {
  if (!s_i2c_mtx) return true;   // not yet created (pre-init) -> single-threaded, no contention
  return xSemaphoreTake(s_i2c_mtx, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void board_i2c_unlock() { if (s_i2c_mtx) xSemaphoreGive(s_i2c_mtx); }

// Battery hooks for the variant-agnostic UI (override the weak defaults in UITask). INA219 read.
int  board_batt_millivolts() { return (int)board.getBattMilliVolts(); }
int  board_batt_current_ma() { return board.getBattCurrentMa(); }
void board_set_power_monitor(int type) { board.setPowerMonitor((uint8_t)type); }  // 0 None, 1 INA219

// I2C bus scan (diagnostic, used by Node Info when the INA219 isn't found): which 7-bit addresses
// ACK on the shared touch bus. Expect 0x14/0x5D (GT911 touch), 0x51 (PCF8563 RTC), 0x40-ish (INA219).
int board_i2c_scan(uint8_t* out, int maxn) {
  if (!board_i2c_lock(200)) return 0;
  Wire.end();                            // clean re-init off the LGFX touch driver's state
  Wire.begin(P_TOUCH_SDA, P_TOUCH_SCL);
  Wire.setClock(100000);
  int cnt = 0;
  for (uint8_t a = 0x08; a <= 0x77 && cnt < maxn; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) out[cnt++] = a;
  }
  board_i2c_unlock();
  return cnt;
}
int  board_touch_int_pin() { return P_TOUCH_INT; }   // UITask uses this to drive touch off the INT

// Optional M5Stack CardKB (mini I2C QWERTY keyboard, addr 0x5F) on the shared touch I2C bus. It's an
// external accessory plugged into the same I2C port as the INA219. Detection is LAZY and hot-plug
// aware: the CardKB is an active microcontroller that often isn't ACKing yet at an early one-shot boot
// probe (the passive PCF8563 RTC beside it is), and it may be connected AFTER power-on. So instead of a
// single radio_init probe we re-probe on a ~1 Hz throttle from board_has_physical_kbd() -- which the UI
// loop calls every pass -- until the unit answers, then latch. Once detected the UI registers a keypad
// indev and drops the on-screen keyboard (same path as the T-Deck's built-in keyboard); the four arrows
// are 0xB4..0xB7 (mapped in UITask::kbd_read_cb). Detection + reads share the touch bus: claim port 0
// from the LGFX touch driver once (Wire.end()), then re-assert the pins per access (touch reconfigures
// them every read), exactly like the INA219 read / board_i2c_scan.
#ifndef CARDKB_ADDR
  #define CARDKB_ADDR 0x5F
#endif
static int8_t   s_cardkb = -1;          // -1 never seen, 0 absent (last probe), 1 present (latched)
static uint32_t s_cardkb_next_ms = 0;   // throttle the re-probe while not yet found
static bool     s_touchbus_claimed = false;
static void cardkb_bus_assert() {       // caller holds board_i2c_lock
  if (!s_touchbus_claimed) { Wire.end(); s_touchbus_claimed = true; }  // one-shot reclaim from touch
  Wire.begin(P_TOUCH_SDA, P_TOUCH_SCL);
  Wire.setClock(100000);                // 100 kHz: robust on the multi-drop touch bus
}
bool board_has_physical_kbd() {
  if (s_cardkb == 1) return true;       // latched once detected (no removal handling)
  uint32_t now = millis();
  if (s_cardkb_next_ms != 0 && (int32_t)(now - s_cardkb_next_ms) < 0) return false;  // throttled
  s_cardkb_next_ms = now + 1000;        // re-probe at most ~1 Hz while absent
  if (!board_i2c_lock(20)) return false;
  cardkb_bus_assert();
  Wire.beginTransmission(CARDKB_ADDR);
  s_cardkb = (Wire.endTransmission() == 0) ? 1 : 0;
  board_i2c_unlock();
  return s_cardkb == 1;
}
int  board_kbd_read() {                 // next pressed ASCII (0 = none); shares the touch bus + i2c lock
  if (s_cardkb != 1) return 0;
  if (!board_i2c_lock(20)) return 0;
  cardkb_bus_assert();
  int r = 0;
  if (Wire.requestFrom((int)CARDKB_ADDR, 1) == 1) { int c = Wire.read(); r = (c > 0) ? c : 0; }
  board_i2c_unlock();
  return r;
}

bool radio_init() {
  hspi_mutex_ensure();   // before any radio/SD SPI transaction can take it
  if (!s_i2c_mtx) s_i2c_mtx = xSemaphoreCreateMutex();   // before the first RTC bus access
  rtc_clock.begin();
  // CardKB detection is lazy (board_has_physical_kbd(), driven from the UI loop): a unit not yet
  // ACKing this early -- or hot-plugged later -- is still picked up. No boot probe needed here.

  lora_spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  return radio.std_init(&lora_spi);
}

uint32_t radio_get_rng_seed() {
  return radio.random(0x7FFFFFFF);
}

void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr) {
  radio.setFrequency(freq);
  radio.setSpreadingFactor(sf);
  radio.setBandwidth(bw);
  radio.setCodingRate(cr);
}

void radio_set_tx_power(int8_t dbm) {
  radio.setOutputPower(dbm);
}

// Radio kill-switch (safe antenna detach): sleep stops all TX/RX; the caller also
// gates the_mesh.loop so nothing tries to transmit. Re-enable = standby + re-apply
// params (the mesh loop then restarts RX).
void radio_sleep()   { radio.sleep(); }
void radio_standby() { radio.standby(); }

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
