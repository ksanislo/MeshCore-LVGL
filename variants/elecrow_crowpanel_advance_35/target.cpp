#include <Arduino.h>
#ifdef HAS_SD_CARD
  #include <SdFat.h>
#endif
#include "target.h"

CrowPanelBoard board;

static SPIClass lora_spi(HSPI);

#ifdef HAS_SD_CARD
// Shared SD filesystem (SdFat, FAT16/32 + exFAT). Consumers (message store, and
// later emoji/map/sound caches) use this handle. The card shares the LoRa HSPI
// controller on its own pins, so we re-pin the bus to the SD for each access and
// restore it to LoRa after. (Single-threaded loop: an SD access completes before
// the mesh touches the radio again.)
// end() first: begin() won't re-pin an already-initialized bus (it just warns).
SdFs sd;

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
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, lora_spi);

WRAPPER_CLASS radio_driver(radio, board);

CrowPanelRTCClock rtc_clock;
EnvironmentSensorManager sensors;

#ifdef DISPLAY_CLASS
  DISPLAY_CLASS display;
#endif

bool radio_init() {
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

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);
}
