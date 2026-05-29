#include "SdCard.h"
#include <Arduino.h>

// Low-level bus + mount primitives provided by the variant (target.cpp): they
// know the SD pins and the LoRa SPI to share. sd_card_begin() brackets its own
// bus access and (re)mounts the shared `sd` handle.
extern void sd_bus_to_sd();
extern void sd_bus_to_lora();
extern bool sd_card_begin();

namespace SdSvc {

static bool     s_mounted = false;
static uint32_t s_retry_ms = 0;   // last (re)mount attempt

Lock::Lock()  { sd_bus_to_sd(); }
Lock::~Lock() { sd_bus_to_lora(); }

bool ready() { return s_mounted; }

bool ensureMounted() {
  if (s_mounted) return true;
  uint32_t now = millis();
  if (s_retry_ms != 0 && (uint32_t)(now - s_retry_ms) < 3000) return false;  // throttle
  s_retry_ms = now ? now : 1;
  s_mounted = sd_card_begin();   // variant re-pins, mounts, restores the bus
  return s_mounted;
}

bool begin() {
  s_mounted = false;
  s_retry_ms = 0;
  return ensureMounted();
}

void end() {
  if (s_mounted) { Lock lk; sd.end(); }
  s_mounted = false;
}

}  // namespace SdSvc
