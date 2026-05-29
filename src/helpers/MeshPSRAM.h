#pragma once

// Optional PSRAM-backed allocation for large, low-traffic MeshCore datastores
// (e.g. the contacts table). Header-only and zero-cost unless a caller is
// compiled in behind its own opt-in flag (e.g. ENABLE_PSRAM_CONTACTS).
//
// On ESP32 with PSRAM, mesh_psram_alloc() prefers SPIRAM via ps_malloc() and
// falls back to the internal heap (malloc) when PSRAM is unavailable -- so the
// worst case is identical to a plain internal allocation, never a hard failure.
// On every other platform it is just malloc(); the call sites are gated by
// per-store opt-in flags that are only meaningful on ESP32-with-PSRAM, so this
// is normally never referenced there.
//
// These stores are process-lifetime singletons, so the allocation is one-shot
// and never freed (same lifetime as the static array it replaces); plain free()
// works for both heaps if a caller ever needs it.

#include <stdlib.h>   // malloc

#if defined(ESP32)
  #include "esp32-hal-psram.h"   // ps_malloc
  static inline void* mesh_psram_alloc(size_t bytes) {
    void* p = ps_malloc(bytes);  // NULL when no PSRAM is detected
    if (!p) p = malloc(bytes);   // fallback: behaves like a normal internal alloc
    return p;
  }
#else
  static inline void* mesh_psram_alloc(size_t bytes) { return malloc(bytes); }
#endif
