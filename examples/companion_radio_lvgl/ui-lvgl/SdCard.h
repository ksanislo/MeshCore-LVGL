#pragma once

#include <SdFat.h>

// Shared microSD facility for the LVGL UI. One mount, one bus discipline, used by
// any feature that needs the card (chat history today; emoji / map tiles / sound
// caches later). The card shares the LoRa HSPI controller on its own pins, so
// every access must hold an SdSvc::Lock (re-pins the bus to the SD on scope
// entry, restores it to LoRa on exit). The loop is single-threaded, so an access
// completes before the mesh touches the radio again.
//
// There is no card-detect pin on this board. Mounting is therefore explicit (boot
// begin() + the top-bar SD button) -- on-access callers use ready() and never auto-
// mount (a missing-card sd.begin() blocks the UI core). pollPresence() is a cheap
// liveness probe that flips ready() false when a mounted card is pulled.
//
// Typical use by a consumer:
//   if (SdSvc::ready()) {
//     SdSvc::Lock lk;
//     FsFile f = sd.open("/emoji/1f600.bin", O_RDONLY);
//     ...
//   }

// Emoji pack identity: the format tag (matches the 'EMJ1' bundle magic) and the font sizes the UI
// renders. The pack downloader builds its GitHub asset names from these; keep in sync with the packer
// (tools/emoji_pack.py --sizes). A format bump (EMJ2) changes EMOJI_PACK_FMT -> a new emoji-emj2 release.
#define EMOJI_PACK_FMT      "emj1"
static const int EMOJI_SIZES[]   = {12, 14, 16, 20, 28};   // /emoji/<size>.bin
static const int EMOJI_SIZE_COUNT = 5;

extern SdFs sd;  // the shared filesystem handle (defined in the variant)
extern volatile uint8_t sd_last_err_code;  // SdFat error from the last failed mount (diagnostic)
extern volatile uint8_t sd_last_err_data;

namespace SdSvc {

// RAII bus guard: re-pin HSPI to the SD on construction, restore to LoRa on
// destruction. Hold one around every block of SD file operations.
struct Lock {
  Lock();
  ~Lock();
};

bool begin();          // mount now (after radio_init); true if mounted
void registerFs();     // register the 'S:' lv_fs driver -- call AFTER lv_init()
bool ready();          // is the card currently mounted?
void end();            // unmount (so the card can be removed)
bool ensureMounted();  // (re)mount if needed, throttled; only boot begin()/the SD button call this
void rescan();         // re-enable mounting after giving up (card (re)inserted)
void pollPresence();   // cheap liveness probe; flips ready() false when a mounted card is pulled

// Decoded-emoji bitmap cache (PSRAM), behind LVGL's image cache. Keyed by
// (size, codepoint) so each glyph reads SD once; survives LVGL eviction.
void emojiBitmapCacheEvict();                              // free all glyphs, reclaim PSRAM (UI thread)
void emojiBitmapCacheStats(uint32_t* glyphs, uint32_t* bytes);  // occupancy, for diagnostics

}  // namespace SdSvc
