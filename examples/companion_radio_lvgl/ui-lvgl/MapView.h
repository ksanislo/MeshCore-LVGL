#pragma once
//
// MapView -- an offline raster-tile basemap with contact markers, for the LVGL
// companion. Renders 256x256 palette PNG tiles read from the SD card under
// /tiles/{z}/{x}/{y}.png (standard Web-Mercator XYZ slippy layout, the de-facto
// mesh-device offline-map format) into a single composited PSRAM RGB565 canvas.
// Contacts with a position are plotted as tappable lv_obj markers above the canvas.
//
// Self-contained: talks to the backend only through mproxy:: (snapshot reads) and
// to the card through SdSvc::; it does NOT include UITask.h. Marker taps are
// reported via a callback so the owner (UITask) can open the contact info card.
//
// Tile decode uses the LVGL-bundled lodepng (LV_USE_PNG=1) called directly --
// NOT the stock lv_img png decoder, which loads via stdio and allocates from
// LVGL's small heap. lodepng's allocators are routed to PSRAM in MapView.cpp
// (LODEPNG_NO_COMPILE_ALLOCATORS).
//
#include "lvgl.h"
#include <stdint.h>

class MapView {
public:
  // Reported when a contact marker is tapped; pubkey is the 6-byte prefix.
  typedef void (*MarkerTapCb)(const uint8_t* pubkey, void* user);

  // Create the canvas + overlay controls inside `parent` (the Map tab page).
  // w/h are the content-area pixel size. Call once.
  void build(lv_obj_t* parent, uint16_t w, uint16_t h, MarkerTapCb cb, void* user);
  void destroy();
  bool isBuilt() const { return _canvas != nullptr; }

  void onShow();                 // Map tab became active: center-on-self (first), recompose
  void onHide();                 // leaving the tab: stop work
  bool service(uint32_t now_ms); // drive from loop() when map tab active & settled
  void markContactsDirty();      // snapshot/own-pos changed -> remark overlays
  void panBy(int dx, int dy);    // trackball pan (T-Deck)

  // Spike/diagnostic: decode one known tile from SD, log dims/time/free-PSRAM.
  static void selfTest();

  // Shared tile reader+decoder (read under SdSvc::Lock, decode off-bus to RGBA8888 in PSRAM).
  // Caller heap_caps_free()s *out_rgba. Used by MapView and the static MapThumb below.
  static bool decodeTileRGBA(const char* path, uint8_t** out_rgba, uint32_t* w, uint32_t* h);

private:
  // --- widgets ---
  lv_obj_t*   _root   = nullptr;   // the tab page (buttons parent; stays fixed)
  lv_obj_t*   _map_layer = nullptr;// holds canvas + markers; shifted live during a drag
  lv_obj_t*   _canvas = nullptr;
  lv_color_t* _cbuf   = nullptr;   // canvas backing buffer (PSRAM), sized _cw x _ch (viewport + margins)
  uint16_t    _cw = 0, _ch = 0;    // BUFFER dims (= viewport + 2*margin); the canvas/layer span
  uint16_t    _vw = 0, _vh = 0;    // VIEWPORT dims (the on-screen tab content area)
  uint16_t    _mx = 0, _my = 0;    // pre-rendered margin each side (so a drag reveals real content live)
  lv_obj_t*   _btn_zin = nullptr, *_btn_zout = nullptr, *_btn_self = nullptr;
  lv_obj_t*   _empty_lbl = nullptr;   // bottom hint shown when a recompose drew zero tiles

  // --- marker overlay pool ---
  static const int MARKER_MAX = 64;
  lv_obj_t*   _markers[MARKER_MAX] = {};
  uint8_t     _marker_key[MARKER_MAX][32] = {};  // full pubkey per marker (for tap -> contact info)
  lv_obj_t*   _self_marker = nullptr;            // our own position (distinct, non-tappable)

  // --- viewport ---
  int      _z = 14;
  double   _center_lat = 0.0, _center_lon = 0.0;
  int64_t  _origin_wx = 0, _origin_wy = 0;   // world-pixel of canvas top-left at _z

  // --- flags / throttle ---
  bool     _recompose_pending = false;
  bool     _markers_dirty     = false;
  bool     _centered_once     = false;
  uint32_t _last_recompose_ms = 0;

  // --- incremental recompose cursor (decode a few tiles per service() pass so a
  //     full-viewport redraw never blocks the UI core / trips the WDT) ---
  bool     _rc_in_progress = false;
  int      _rc_z = 0;
  int64_t  _rc_ox = 0, _rc_oy = 0;            // origin snapshot for this pass
  int      _rc_tx0=0,_rc_ty0=0,_rc_tx1=0,_rc_ty1=0, _rc_tx=0,_rc_ty=0;
  int      _rc_drawn=0;                        // tiles actually rendered this pass (0 -> show empty hint)
  bool     _rc_no_fill=false;                  // soft pass: keep shifted pixels, only redraw the exposed strip
  bool     _rc_was_full=false;                 // this pass cleared+redrew everything (gates the empty-state check)
  int      _rc_sx=0, _rc_sy=0;                 // pixel shift applied before a soft pass (exposed-strip geometry)
  int      _rc_phase=0;                        // full pass: 0=viewport tiles first, 1=surrounding margin
  bool     _rc_strip_only=false;              // this pass redraws ONLY the exposed strip (buffer was complete)
  bool     _buf_complete=false;               // whole buffer is rendered for the current origin (gates strip-only)

  // --- decoded-tile LRU cache (RGB565 in PSRAM, keyed by (z,tx,ty)) so panning
  //     back over a tile re-blits instead of re-reading+decoding from SD. Freed on
  //     leaving the map (onHide) to return the PSRAM. ~128 KB per 256^2 entry. ---
  static const int TILE_CACHE_MAX = 12;       // ~1.5 MB ceiling
  struct TileCacheEntry { int z=-1, tx=0, ty=0; uint32_t lru=0; lv_color_t* px=nullptr; uint16_t w=0, h=0; };
  TileCacheEntry _tcache[TILE_CACHE_MAX];
  uint32_t _lru_clock = 0;

  // --- drag / double-tap ---
  bool       _dragging = false;
  lv_point_t _drag_last = {0, 0};
  int        _drag_dx = 0, _drag_dy = 0;     // accumulated layer shift since press
  uint32_t   _last_tap_ms = 0;
  lv_point_t _last_tap_pt = {0, 0};

  // --- tap callback ---
  MarkerTapCb _tap_cb   = nullptr;
  void*       _tap_user = nullptr;

  // --- helpers (defined across build steps) ---
  void recompose();
  void beginRecompose();
  bool stepRecompose(int max_tiles);          // decode up to N tiles; true when the viewport is filled
  void cursorFromWorldRect(int64_t x0, int64_t y0, int64_t x1, int64_t y1);  // set tile cursor from a world-px rect
  void drawTile(int z, int tx, int ty, int64_t ox, int64_t oy);
  void shiftCanvas(int sx, int sy);           // scroll buffer pixels (sx,sy>0 = right/down); fill vacated edges with bg
  void blitRGBA(const uint8_t* rgba, uint32_t w, uint32_t h, int dst_x, int dst_y);
  void blit565(const lv_color_t* src, uint32_t w, uint32_t h, int dst_x, int dst_y);
  // (decodeTileRGBA is declared public above -- shared with MapThumb)
  lv_color_t* getTile(int z, int tx, int ty, uint16_t& w, uint16_t& h);  // cached RGB565 (decode on miss); null if missing
  void tileCacheEvictAll();
  void repositionMarkers();
  void centerOnSelf();
  bool firstContactPos(double& lat, double& lon);   // fallback center: first contact carrying a position
  bool markerScreenPos(double lat, double lon, int& sx, int& sy);  // lat/lon -> canvas px (false if off-canvas)
  void setCenter(double lat, double lon);
  void setZoomKeepCenter(int newz);
  void zoomInAt(int px, int py);              // double-tap: zoom in one level toward a canvas point
  void clampOrigin();

  // event trampolines
  static void canvas_event_cb(lv_event_t* e);
  static void zoom_btn_cb(lv_event_t* e);
  static void marker_event_cb(lv_event_t* e);
};

// MapThumb -- a small, static (non-interactive) square basemap preview, drawn at full zoom
// (z14) centered on a fixed lat/lon with a center marker. Used below the hero card on the
// contact-info page and the owner profile to show "where this node is". One PSRAM buffer per
// thumb; re-renders only when the location changes (cheap on a 1 Hz refresh).
class MapThumb {
public:
  void create(lv_obj_t* parent, int side);   // allocate the square canvas (hidden until render)
  void render(double lat, double lon);        // draw z14 centered on (lat,lon); reveal
  void hide();
  bool ok() const { return _canvas != nullptr; }

private:
  lv_obj_t*   _wrap   = nullptr;   // full-width centering wrapper (the flex item)
  lv_obj_t*   _canvas = nullptr;
  lv_color_t* _buf    = nullptr;
  int         _side   = 0;
  double      _lat    = 1e9, _lon = 1e9;   // last-rendered center (skip redundant re-renders)
  bool        _shown  = false;
};
