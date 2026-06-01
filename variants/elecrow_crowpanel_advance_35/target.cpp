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

bool sd_card_begin() {
  sd_bus_to_sd();
  sd.end();   // clean (re)mount -- lets a re-inserted card be picked up
  // SHARED_SPI: SdFat re-acquires the bus per access, matching our re-pin model.
  bool ok = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(20), &lora_spi));
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
int  board_touch_int_pin() { return P_TOUCH_INT; }   // UITask uses this to drive touch off the INT

bool radio_init() {
  hspi_mutex_ensure();   // before any radio/SD SPI transaction can take it
  if (!s_i2c_mtx) s_i2c_mtx = xSemaphoreCreateMutex();   // before the first RTC bus access
  rtc_clock.begin();

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
