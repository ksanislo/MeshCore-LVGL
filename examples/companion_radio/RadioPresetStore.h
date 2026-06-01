#pragma once
// On-device radio-preset override: a compact binary file in INTERNAL flash (SPIFFS),
// written by the backend after a WiFi fetch of the official preset list and read by
// the UI. When present + valid it overrides the compiled-in seed table, so updated
// presets become part of the device without reflashing. Crash-safe temp+rename write,
// same style as DataStore's /new_prefs.
//
// File layout: [u32 magic]["u16 version][u16 count][count * RadioPresetRec]
//   numeric fields are stored as parsed floats/ints (the API serves them as strings).

#include <stdint.h>

#define RADIO_PRESET_FILE       "/radio_presets"
#define RADIO_PRESET_MAGIC      0x4D435031u   // 'MCP1'
#define RADIO_PRESET_VERSION    1
#define RADIO_PRESET_MAX        48            // hard cap (RAM table + parse bound)
#define RADIO_PRESET_TITLE_LEN  32

struct RadioPresetRec {
  char    title[RADIO_PRESET_TITLE_LEN];   // NUL-terminated, truncated to fit
  float   freq;                            // MHz
  float   bw;                              // kHz
  uint8_t sf;                              // spreading factor
  uint8_t cr;                              // coding rate
};

// Read the override file into `out` (up to `max` records). Returns the count, or
// 0 if the file is missing/invalid (caller then uses the compiled seed table).
int  loadRadioPresets(RadioPresetRec* out, int max);

// Write `n` records to the override file (crash-safe). Returns false on I/O error.
bool saveRadioPresets(const RadioPresetRec* recs, int n);
