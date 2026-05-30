#pragma once

#include <lvgl.h>

// Splash asset sourced from meshcore.io, pre-composited onto the page's
// bluish-grey background (#111827) so the firmware needs no alpha math:
//
//   meshcore_logo_img  -- 260x29, RGB565  (from /assets/images/meshcore.svg)
//
// LV_IMG_CF_TRUE_COLOR, little-endian (matches LV_COLOR_16_SWAP=0).
//
// Regeneration (one-off venv):
//   python3 -m venv /tmp/svg-venv
//   /tmp/svg-venv/bin/pip install cairosvg pillow
//   curl -sL https://meshcore.io/assets/images/meshcore.svg > /tmp/meshcore.svg
//   ... then re-run the inline script that produced meshcore_logo_img.cpp.

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t meshcore_logo_img;

// Resolution-independent, palette-swappable wordmark (TRUE_COLOR_ALPHA, 600x67
// master from logo/meshcore.svg). White pixels + per-pixel alpha: scale via
// lv_img_set_zoom (this format is zoomable; ALPHA_8BIT is not) and tint via
// img_recolor=UI_LOGO at LV_OPA_COVER (which overrides the white).
extern const lv_img_dsc_t meshcore_logo_alpha;

#ifdef __cplusplus
}
#endif
