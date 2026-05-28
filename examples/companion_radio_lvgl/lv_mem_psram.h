#pragma once

// Backing allocator for LVGL's object heap (LV_MEM_CUSTOM=1).
// Routes all widget/object allocations to PSRAM so the ~320 KB internal
// SRAM pool is reserved for DMA-capable framebuffers and the rest of the
// firmware. ps_malloc/ps_realloc come from the Arduino-ESP32 core and
// allocate from SPIRAM; free() works for both heaps.

#include <stdlib.h>            // free
#include "esp32-hal-psram.h"   // ps_malloc, ps_realloc (extern "C", C-safe)
