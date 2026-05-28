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

#ifdef __cplusplus
}
#endif
