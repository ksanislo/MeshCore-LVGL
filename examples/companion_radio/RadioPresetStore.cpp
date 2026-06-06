#include "RadioPresetStore.h"
#include <Arduino.h>

// The radio-preset override store lives in internal flash. The WiFi preset-update
// feature that writes it is ESP32/RP2040-only (both expose the Arduino fs::FS API).
// Other platforms (nRF52 / STM32 use Adafruit_LittleFS, a different File API, and
// have no WiFi preset-update path) compile portable stubs that always fall back to
// the compiled-in seed table.
#if defined(ESP32)
  #include <SPIFFS.h>
  #define PRESET_FS SPIFFS
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  #define PRESET_FS LittleFS
#endif

#if defined(PRESET_FS)

int loadRadioPresets(RadioPresetRec* out, int max) {
  File f = PRESET_FS.open(RADIO_PRESET_FILE, "r");
  if (!f) return 0;
  uint32_t magic = 0; uint16_t ver = 0, count = 0;
  if (f.read((uint8_t*)&magic, 4) != 4 || magic != RADIO_PRESET_MAGIC) { f.close(); return 0; }
  if (f.read((uint8_t*)&ver, 2) != 2 || ver != RADIO_PRESET_VERSION)   { f.close(); return 0; }
  if (f.read((uint8_t*)&count, 2) != 2)                                { f.close(); return 0; }
  if (count > RADIO_PRESET_MAX) count = RADIO_PRESET_MAX;
  int n = 0;
  for (int i = 0; i < count && n < max; i++) {
    RadioPresetRec r{};
    if (f.read((uint8_t*)&r, sizeof(r)) != (int)sizeof(r)) break;   // truncated -> stop
    r.title[RADIO_PRESET_TITLE_LEN - 1] = 0;
    out[n++] = r;
  }
  f.close();
  return n;
}

bool saveRadioPresets(const RadioPresetRec* recs, int n) {
  if (n < 0) n = 0;
  if (n > RADIO_PRESET_MAX) n = RADIO_PRESET_MAX;
  File f = PRESET_FS.open(RADIO_PRESET_FILE ".tmp", "w", true);
  if (!f) return false;
  uint32_t magic = RADIO_PRESET_MAGIC; uint16_t ver = RADIO_PRESET_VERSION, count = (uint16_t)n;
  bool ok = f.write((const uint8_t*)&magic, 4) == 4 &&
            f.write((const uint8_t*)&ver, 2) == 2 &&
            f.write((const uint8_t*)&count, 2) == 2;
  for (int i = 0; ok && i < n; i++)
    ok = f.write((const uint8_t*)&recs[i], sizeof(RadioPresetRec)) == (int)sizeof(RadioPresetRec);
  f.close();
  if (!ok) { PRESET_FS.remove(RADIO_PRESET_FILE ".tmp"); return false; }
  PRESET_FS.remove(RADIO_PRESET_FILE);                          // atomic-ish swap
  return PRESET_FS.rename(RADIO_PRESET_FILE ".tmp", RADIO_PRESET_FILE);
}

#else  // nRF52 / STM32 etc.: no internal preset-override store -> always use the seed table.

int  loadRadioPresets(RadioPresetRec*, int) { return 0; }
bool saveRadioPresets(const RadioPresetRec*, int) { return false; }

#endif
