#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>   // iOS-parity avatar color: fmodf/fabsf/lroundf

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

// ----- Color tokens (runtime, role-based palette) --------------------------
// ~13 canonical ROLES live in a UiPalette struct; the active one is g_ui_palette
// (defined in UITask.cpp, swappable at runtime). The token names below are macros
// that read g_ui_palette, so the hundreds of existing lv_color_hex(UI_*) call
// sites are untouched -- they just follow whatever palette is active. Semantic +
// legacy aliases map onto the canonical roles, so the palette stays small.
struct UiPalette {
  // Neutrals
  uint32_t bg;          // page background
  uint32_t surface;     // cards / top bars
  uint32_t border;      // hairlines, dividers, neutral fills
  uint32_t msg_in;      // incoming chat bubble (own role; themeable apart from border)
  uint32_t dim;         // secondary / metadata text
  uint32_t fg;          // primary text
  uint32_t fg_strong;   // emphasized text / wordmark
  uint32_t on_color;    // text/glyph on a saturated fill (avatar letters)
  uint32_t scrim;       // modal backdrop / scrim (used with opacity)
  // Brand
  uint32_t accent;      // links, chips, selected indicator
  uint32_t primary;     // primary action button / outgoing bubble
  // Status
  uint32_t alert;       // unread + error + alerts (one bright color)
  uint32_t danger;      // destructive-action button bg
  uint32_t fav;         // favourite accent
};

// The meshcore.io dark theme (Tailwind gray-900 base) -- the default palette and
// the base every other theme (incl. SD-card overrides) starts from.
static constexpr UiPalette UI_THEME_DARK = {
  /*bg*/        0x111827,  // gray-900
  /*surface*/   0x1F2937,  // gray-800
  /*border*/    0x374151,  // gray-700
  /*msg_in*/    0x374151,  // gray-700
  /*dim*/       0x6B7280,  // gray-500
  /*fg*/        0xD1D5DB,  // gray-300
  /*fg_strong*/ 0xF3F4F6,  // gray-100
  /*on_color*/  0xFFFFFF,  // white
  /*scrim*/     0x000000,  // black
  /*accent*/    0x60A5FA,  // blue-400
  /*primary*/   0x2563EB,  // blue-600
  /*alert*/     0xEF4444,  // red-500
  /*danger*/    0x7F1D1D,  // red-900
  /*fav*/       0xFBBF24,  // amber-400
};

extern UiPalette g_ui_palette;   // active palette (defined in UITask.cpp)

// Canonical role tokens -> active palette fields.
#define UI_BG          (g_ui_palette.bg)
#define UI_SURFACE     (g_ui_palette.surface)
#define UI_BORDER      (g_ui_palette.border)
#define UI_MSG_IN      (g_ui_palette.msg_in)
#define UI_DIM         (g_ui_palette.dim)
#define UI_FG          (g_ui_palette.fg)
#define UI_FG_STRONG   (g_ui_palette.fg_strong)
#define UI_ON_COLOR    (g_ui_palette.on_color)
#define UI_SCRIM       (g_ui_palette.scrim)
#define UI_ACCENT      (g_ui_palette.accent)
#define UI_PRIMARY     (g_ui_palette.primary)
#define UI_ALERT       (g_ui_palette.alert)
#define UI_DANGER      (g_ui_palette.danger)
#define UI_FAV         (g_ui_palette.fav)

// Semantic aliases (map onto the roles above; keep call sites unchanged).
#define UI_AVATAR_NEUT UI_BORDER     // non-chat avatar bg
#define UI_MSG_OUT     UI_PRIMARY    // outgoing chat bubble
#define UI_LOGO        UI_FG_STRONG  // wordmark tint (recolorable)
#define UI_UNREAD      UI_ALERT      // unread dot / mark
#define UI_ERROR       UI_ALERT      // inline error / failure text

// Legacy short aliases (do not remove -- referenced widely in UITask.cpp).
#define BG_HEX         UI_BG
#define FG_HEX         UI_FG
#define DIM_HEX        UI_DIM
#define FAV_HEX        UI_FAV

// ----- Built-in themes -----------------------------------------------------
// True-black (OLED): pure-black bg, near-black surfaces, dark theme otherwise.
static constexpr UiPalette UI_THEME_BLACK = {
  /*bg*/0x000000, /*surface*/0x0D0D0D, /*border*/0x262626, /*msg_in*/0x1A1A1A,
  /*dim*/0x6B7280, /*fg*/0xD1D5DB, /*fg_strong*/0xF3F4F6, /*on_color*/0xFFFFFF, /*scrim*/0x000000,
  /*accent*/0x60A5FA, /*primary*/0x2563EB, /*alert*/0xEF4444, /*danger*/0x7F1D1D, /*fav*/0xFBBF24,
};
// Light: light bg, dark text; deeper accents/status for contrast on white. The
// incoming bubble is a mid grey (darker than the page) so dark text reads clearly.
static constexpr UiPalette UI_THEME_LIGHT = {
  /*bg*/0xF9FAFB, /*surface*/0xFFFFFF, /*border*/0xD1D5DB, /*msg_in*/0xCBD5E1,
  /*dim*/0x6B7280, /*fg*/0x111827, /*fg_strong*/0x000000, /*on_color*/0xFFFFFF, /*scrim*/0x000000,
  /*accent*/0x2563EB, /*primary*/0x2563EB, /*alert*/0xDC2626, /*danger*/0xB91C1C, /*fav*/0xD97706,
};
// Accent variants: the dark neutrals, only the brand colors (accent + primary) change.
static constexpr UiPalette UI_THEME_EMERALD = {
  0x111827, 0x1F2937, 0x374151, 0x374151, 0x6B7280, 0xD1D5DB, 0xF3F4F6, 0xFFFFFF, 0x000000,
  /*accent*/0x34D399, /*primary*/0x059669, 0xEF4444, 0x7F1D1D, 0xFBBF24,
};
static constexpr UiPalette UI_THEME_AMBER = {
  0x111827, 0x1F2937, 0x374151, 0x374151, 0x6B7280, 0xD1D5DB, 0xF3F4F6, 0xFFFFFF, 0x000000,
  /*accent*/0xFBBF24, /*primary*/0xD97706, 0xEF4444, 0x7F1D1D, 0xFBBF24,
};
static constexpr UiPalette UI_THEME_VIOLET = {
  0x111827, 0x1F2937, 0x374151, 0x374151, 0x6B7280, 0xD1D5DB, 0xF3F4F6, 0xFFFFFF, 0x000000,
  /*accent*/0xA78BFA, /*primary*/0x7C3AED, 0xEF4444, 0x7F1D1D, 0xFBBF24,
};

// Registry: name (also the Settings label) + palette. SD-card themes extend this
// list at runtime by filename.
struct UiTheme { const char* name; UiPalette pal; };
static const UiTheme UI_BUILTIN_THEMES[] = {
  { "Dark",    UI_THEME_DARK    },
  { "Black",   UI_THEME_BLACK   },
  { "Light",   UI_THEME_LIGHT   },
  { "Emerald", UI_THEME_EMERALD },
  { "Amber",   UI_THEME_AMBER   },
  { "Violet",  UI_THEME_VIOLET  },
};
static constexpr int UI_BUILTIN_THEME_N =
    sizeof(UI_BUILTIN_THEMES) / sizeof(UI_BUILTIN_THEMES[0]);

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

// FNV-1a over the name's raw UTF-8 bytes (same seed/prime as the snapshot
// signature hash in MeshProxy.cpp, and as the iOS app) -> stable per name.
static inline uint32_t nameHash(const char* name) {
  uint32_t h = 2166136261u;
  if (name) for (const char* p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

// Curated scheme: hash into a hand-picked palette that excludes pure red so an
// avatar never collides with the red unread chevron.
static inline uint32_t nameColorCurated(const char* name) {
  return AVATAR_PALETTE[nameHash(name) % AVATAR_PALETTE_N];
}

// iOS-app parity: continuous hue wheel, HSV(hue = hash % 360, S = 0.75, V = 0.80).
// Pixel-verified against the closed-source iOS MeshCore app (see IOS_AVATAR_COLOR.md)
// so a contact gets the SAME color on the phone and here. Reintroduces red avatars.
static inline uint32_t nameColorIos(const char* name) {
  float H = (nameHash(name) % 360) / 60.0f;
  float C = 0.80f * 0.75f;                          // chroma = 0.60
  float X = C * (1.0f - fabsf(fmodf(H, 2.0f) - 1.0f));
  float m = 0.80f - C;                              // = 0.20
  float r = 0, g = 0, b = 0;
  switch ((int)H) {
    case 0: r = C; g = X;        break;
    case 1: r = X; g = C;        break;
    case 2: g = C; b = X;        break;
    case 3: g = X; b = C;        break;
    case 4: r = X; b = C;        break;
    default: r = C; b = X;       break;
  }
  uint8_t R = (uint8_t)lroundf((r + m) * 255);
  uint8_t G = (uint8_t)lroundf((g + m) * 255);
  uint8_t B = (uint8_t)lroundf((b + m) * 255);
  return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

// Live avatar-color scheme, mirrored from NodePrefs.avatar_palette (0 = curated,
// 1 = iOS). Defined in UITask.cpp; UITask refreshes it from prefs and rebinds the
// contact rows when the Settings toggle changes. Read-only everywhere else.
extern uint8_t g_avatar_palette_mode;

// Deterministic name -> avatar color, dispatched on the active scheme. Same
// signature/return as before, so the lv_color_hex(nameColor(...)) call sites are
// untouched. Not in the scroll hot path (called only at row/header bind).
static inline uint32_t nameColor(const char* name) {
  return g_avatar_palette_mode ? nameColorIos(name) : nameColorCurated(name);
}
