#pragma once

#include <SdFat.h>

// Shared microSD facility for the LVGL UI. One mount, one bus discipline, used by
// any feature that needs the card (chat history today; emoji / map tiles / sound
// caches later). The card shares the LoRa HSPI controller on its own pins, so
// every access must hold an SdSvc::Lock (re-pins the bus to the SD on scope
// entry, restores it to LoRa on exit). The loop is single-threaded, so an access
// completes before the mesh touches the radio again.
//
// There is no card-detect pin on this board, so the volume is (re)mounted lazily
// on access via ensureMounted(), throttled so a missing card doesn't thrash the
// bus -- this also recovers a card that's been pulled and re-inserted.
//
// Typical use by a consumer:
//   if (SdSvc::ensureMounted()) {
//     SdSvc::Lock lk;
//     FsFile f = sd.open("/emoji/1f600.bin", O_RDONLY);
//     ...
//   }

extern SdFs sd;  // the shared filesystem handle (defined in the variant)

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
bool ensureMounted();  // lazily (re)mount if needed, throttled; call before access

}  // namespace SdSvc
