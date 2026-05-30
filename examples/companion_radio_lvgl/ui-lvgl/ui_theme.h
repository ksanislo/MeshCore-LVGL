#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// UI kit: the single source of truth for colors, layout metrics, and a few
// shared primitives (avatar palette + name->color hash). New UI code should pull
// tokens from here instead of hard-coding hexes/sizes; existing inline values are
// migrated opportunistically, not in a big sweep. Colors are raw 0xRRGGBB ints --
// wrap call sites in lv_color_hex(...) exactly as the existing code does, so this
// header stays free of an LVGL dependency.
//
// Palette base: the meshcore.io dark theme (Tailwind gray-900 background). Named
// tokens map to Tailwind shades so the scheme stays coherent as it grows.
// ---------------------------------------------------------------------------

// ----- Color tokens --------------------------------------------------------
// Canonical names. The short legacy aliases (BG_HEX/FG_HEX/...) below keep the
// hundreds of existing call sites compiling unchanged.
static constexpr uint32_t UI_BG          = 0x111827;  // gray-900  page background
static constexpr uint32_t UI_SURFACE     = 0x1F2937;  // gray-800  cards / top bars
static constexpr uint32_t UI_BORDER      = 0x374151;  // gray-700  hairlines / dividers
static constexpr uint32_t UI_FG          = 0xD1D5DB;  // gray-300  primary text
static constexpr uint32_t UI_FG_BRIGHT   = 0xF3F4F6;  // gray-100  emphasized text
static constexpr uint32_t UI_DIM         = 0x6B7280;  // gray-500  secondary / metadata
static constexpr uint32_t UI_ACCENT      = 0x60A5FA;  // blue-400  links / accents
static constexpr uint32_t UI_FAV         = 0xFBBF24;  // amber-400 favourite accent
static constexpr uint32_t UI_UNREAD      = 0xEF4444;  // red-500   unread dot
static constexpr uint32_t UI_AVATAR_NEUT = 0x374151;  // gray-700  non-chat avatar bg
static constexpr uint32_t UI_PRIMARY     = 0x2563EB;  // blue-600  primary action button
static constexpr uint32_t UI_DANGER      = 0x7F1D1D;  // red-900   destructive action button
static constexpr uint32_t UI_ERROR       = 0xF87171;  // red-400   inline error / failure text
static constexpr uint32_t UI_MSG_OUT     = 0x2563EB;  // blue-600  outgoing chat bubble
static constexpr uint32_t UI_MSG_IN      = 0x374151;  // gray-700  incoming chat bubble

// Legacy aliases (do not remove -- referenced widely in UITask.cpp).
static constexpr uint32_t BG_HEX  = UI_BG;
static constexpr uint32_t FG_HEX  = UI_FG;
static constexpr uint32_t DIM_HEX = UI_DIM;
static constexpr uint32_t FAV_HEX = UI_FAV;

// ----- Layout metrics ------------------------------------------------------
// (HEADER_H / TABBAR_H / COMPOSE_H / SEARCH_BAR_H still live in UITask.cpp.)
static constexpr int UI_CONTACT_ROW_H = 56;  // contacts list row height (recycler unit)
static constexpr int UI_AVATAR_D      = 40;  // avatar circle diameter
static constexpr int UI_DOT_D         = 10;  // unread dot diameter
static constexpr int UI_PAD           = 8;   // default inner padding

// ----- Avatar palette ------------------------------------------------------
// Name-seeded circle colors, curated to stay legible with white text on UI_BG.
// Tailwind 500/600 shades; deliberately excludes pure red so an avatar never
// reads as the unread dot. Order is irrelevant (selection is hash % count).
static constexpr uint32_t AVATAR_PALETTE[] = {
  0xE11D48,  // rose-600
  0xDB2777,  // pink-600
  0xC026D3,  // fuchsia-600
  0x9333EA,  // purple-600
  0x7C3AED,  // violet-600
  0x4F46E5,  // indigo-600
  0x2563EB,  // blue-600
  0x0284C7,  // sky-600
  0x0891B2,  // cyan-600
  0x0D9488,  // teal-600
  0x059669,  // emerald-600
  0x16A34A,  // green-600
  0xCA8A04,  // yellow-600
  0xD97706,  // amber-600
  0xEA580C,  // orange-600
};
static constexpr size_t AVATAR_PALETTE_N =
    sizeof(AVATAR_PALETTE) / sizeof(AVATAR_PALETTE[0]);

// Deterministic name -> avatar color. FNV-1a (same seed/prime as the snapshot
// signature hash in MeshProxy.cpp) so a given name always maps to one color.
static inline uint32_t nameColor(const char* name) {
  uint32_t h = 2166136261u;
  if (name) for (const char* p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return AVATAR_PALETTE[h % AVATAR_PALETTE_N];
}
