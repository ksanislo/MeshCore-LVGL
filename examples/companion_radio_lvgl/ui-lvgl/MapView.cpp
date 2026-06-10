#include "MapView.h"
#include <Arduino.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "SdCard.h"
#include "MeshProxy.h"
#include "ui_theme.h"

// --- lodepng allocators -> PSRAM ------------------------------------------------
// With -DLODEPNG_NO_COMPILE_ALLOCATORS the bundled lodepng (LV_USE_PNG=1) declares
// these extern and we own them. Route every lodepng allocation (the ~256 KB ARGB
// output + inflate scratch) to PSRAM so a tile decode never touches the tight
// internal SRAM floor (DMA draw buffers / BLE / WiFi / OTA). PSRAM-only on purpose:
// on exhaustion we return NULL and the decode fails gracefully (tile renders blank)
// rather than spilling into internal RAM.
extern "C" {
void* lodepng_malloc(size_t size)                 { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
void* lodepng_realloc(void* ptr, size_t new_size) { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
void  lodepng_free(void* ptr)                     { heap_caps_free(ptr); }

// lodepng decode entry (prototype declared here to avoid depending on lodepng.h's
// include path; signature matches lvgl/src/extra/libs/png/lodepng.h).
unsigned lodepng_decode32(unsigned char** out, unsigned* w, unsigned* h,
                          const unsigned char* in, size_t insize);
}

// --- Web-Mercator slippy-map math ----------------------------------------------
// World-pixel space at zoom z is (256<<z) square, origin top-left. Tiles are 256px.
static inline double mapSizeD(int z) { return (double)(256u << z); }

static inline double lonToWorldX(double lon, int z) {
  return (lon + 180.0) / 360.0 * mapSizeD(z);
}
static inline double latToWorldY(double lat, int z) {
  double s = sin(lat * M_PI / 180.0);
  if (s > 0.9999) s = 0.9999; else if (s < -0.9999) s = -0.9999;
  return (0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI)) * mapSizeD(z);
}
static inline double worldXToLon(double x, int z) {
  return x / mapSizeD(z) * 360.0 - 180.0;
}
static inline double worldYToLat(double y, int z) {
  double n = M_PI - 2.0 * M_PI * y / mapSizeD(z);
  return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

// --- tile read + decode ---------------------------------------------------------
// Read the whole compressed PNG into a PSRAM buffer under one SdSvc::Lock, then
// RELEASE the lock before decoding (the WDT rule: never hold the shared SD/HSPI
// bus across the multi-ms decode). Caller heap_caps_free()s the returned buffer.
static uint8_t* readFilePSRAM(const char* path, size_t* out_len) {
  *out_len = 0;
  if (!SdSvc::ready()) return nullptr;
  SdSvc::Lock lk;                          // re-pin bus to SD; restored on scope exit
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) return nullptr;
  size_t n = f.size();
  uint8_t* buf = (n > 0) ? (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : nullptr;
  if (buf && f.read(buf, n) != (int)n) { heap_caps_free(buf); buf = nullptr; n = 0; }
  f.close();
  *out_len = buf ? n : 0;
  return buf;                              // lock released here
}

bool MapView::decodeTileRGBA(const char* path, uint8_t** out_rgba, uint32_t* w, uint32_t* h) {
  *out_rgba = nullptr; *w = 0; *h = 0;
  size_t len = 0;
  uint8_t* png = readFilePSRAM(path, &len);
  if (!png) return false;
  unsigned iw = 0, ih = 0; unsigned char* rgba = nullptr;
  unsigned err = lodepng_decode32(&rgba, &iw, &ih, png, len);   // off-bus, PSRAM
  heap_caps_free(png);
  if (err || !rgba) { if (rgba) heap_caps_free(rgba); return false; }
  *out_rgba = rgba; *w = iw; *h = ih;
  return true;
}

// --- viewport helpers -----------------------------------------------------------
// _origin_wx/wy is the world pixel of the BUFFER top-left. Clamp so the VIEWPORT (origin+margin
// .. +viewport) stays within the world; the buffer margins may hang past the edge (those tiles
// just render blank).
void MapView::clampOrigin() {
  int64_t ms = (int64_t)(256u << _z);
  int64_t vx = _origin_wx + _mx, vy = _origin_wy + _my;
  int64_t vxmax = ms - _vw, vymax = ms - _vh;
  if (vxmax < 0) vxmax = 0;
  if (vymax < 0) vymax = 0;
  if (vx < 0) vx = 0; else if (vx > vxmax) vx = vxmax;
  if (vy < 0) vy = 0; else if (vy > vymax) vy = vymax;
  _origin_wx = vx - _mx; _origin_wy = vy - _my;
}

void MapView::setCenter(double lat, double lon) {
  _center_lat = lat; _center_lon = lon;
  // Buffer center == viewport center (symmetric margins), so origin = pointWorld - buffer/2.
  _origin_wx = (int64_t)llround(lonToWorldX(lon, _z)) - _cw / 2;
  _origin_wy = (int64_t)llround(latToWorldY(lat, _z)) - _ch / 2;
  clampOrigin();
  if (_map_layer) lv_obj_set_pos(_map_layer, -_mx, -_my);   // drop any drag offset
  _recompose_pending = true;
  _rc_in_progress = false;   // restart any in-flight pass
  _rc_no_fill = false;       // a center/zoom is a full (clear + redraw) recompose
  _rc_strip_only = false;
  _buf_complete = false;
}

void MapView::setZoomKeepCenter(int newz) {
  if (newz < 1) newz = 1; else if (newz > 14) newz = 14;
  if (newz == _z) return;
  // viewport center in lat/lon at the OLD zoom, then re-center at the new zoom
  double lat = worldYToLat(_origin_wy + _ch / 2.0, _z);
  double lon = worldXToLon(_origin_wx + _cw / 2.0, _z);
  _z = newz;
  setCenter(lat, lon);
}

void MapView::zoomInAt(int px, int py) {
  if (_z >= 14) return;
  // px/py are viewport coords; the viewport sits at (_mx,_my) within the buffer.
  double lat = worldYToLat((double)_origin_wy + _my + py, _z);   // geo point under the tap
  double lon = worldXToLon((double)_origin_wx + _mx + px, _z);
  _z += 1;
  setCenter(lat, lon);                                       // bring it to center, one level deeper
  _markers_dirty = true;
}

bool MapView::firstContactPos(double& lat, double& lon) {
  bool found = false;
  mproxy::beginUiRead();
  int n = mproxy::getNumContacts();
  for (int i = 0; i < n; i++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (c.gps_lat == 0 && c.gps_lon == 0) continue;
    lat = c.gps_lat / 1e6; lon = c.gps_lon / 1e6; found = true; break;
  }
  mproxy::endUiRead();
  return found;
}

void MapView::centerOnSelf() {
  _z = 14;
  int32_t lat_e6 = 0, lon_e6 = 0;
  mproxy::selfLatLon(lat_e6, lon_e6);
  if (lat_e6 != 0 || lon_e6 != 0) { setCenter(lat_e6 / 1e6, lon_e6 / 1e6); return; }
  // No own fix -> center on the first contact that carries a position.
  double lat = 0, lon = 0;
  if (firstContactPos(lat, lon)) { setCenter(lat, lon); return; }
  // Nothing positioned yet: a low zoom over the coverage centroid is more useful than
  // z14 at null island. Drop to z3 and clamp (the basemap shows the wider area).
  _z = 3;
  setCenter(0.0, 0.0);
}

// lat/lon -> canvas pixel for the current viewport. Returns false if off-canvas.
bool MapView::markerScreenPos(double lat, double lon, int& sx, int& sy) {
  int64_t wx = (int64_t)llround(lonToWorldX(lon, _z));
  int64_t wy = (int64_t)llround(latToWorldY(lat, _z));
  sx = (int)(wx - _origin_wx);
  sy = (int)(wy - _origin_wy);
  return (sx >= 0 && sx < _cw && sy >= 0 && sy < _ch);
}

// --- compositing ----------------------------------------------------------------
// Snapshot the viewport, clear to background, and set up the tile cursor. The
// actual decoding is drained a few tiles at a time by stepRecompose() so a full
// redraw never blocks the UI core. A pan/zoom mid-pass clears _rc_in_progress
// (in setCenter/panBy), so the next service() restarts cleanly at the new origin.
void MapView::cursorFromWorldRect(int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
  _rc_tx0 = (int)(x0 / 256); _rc_ty0 = (int)(y0 / 256);
  _rc_tx1 = (int)(x1 / 256); _rc_ty1 = (int)(y1 / 256);
  _rc_tx = _rc_tx0; _rc_ty = _rc_ty0;
}

void MapView::beginRecompose() {
  _rc_z = _z; _rc_ox = _origin_wx; _rc_oy = _origin_wy;
  _rc_drawn = 0;
  _rc_was_full = !_rc_strip_only;   // strip-only never repaints the whole viewport -> can't drive empty-state
  // Clear to bg ONLY for a center/zoom (no shifted pixels to keep). Pans set _rc_no_fill.
  if (!_rc_no_fill) lv_canvas_fill_bg(_canvas, lv_color_hex(BG_HEX), LV_OPA_COVER);
  if (_rc_strip_only) {
    _rc_phase = 1;   // buffer was complete: sweep the whole buffer, strip-filter keeps only the exposed edge
    cursorFromWorldRect(_rc_ox, _rc_oy, _rc_ox + _cw - 1, _rc_oy + _ch - 1);
  } else {
    _rc_phase = 0;   // full redraw (clear or gap-fill): visible viewport first, then the off-screen margin
    cursorFromWorldRect(_rc_ox + _mx, _rc_oy + _my, _rc_ox + _mx + _vw - 1, _rc_oy + _my + _vh - 1);
  }
  _rc_in_progress = true;
}

bool MapView::stepRecompose(int max_tiles) {
  const int ntiles = 1 << _rc_z;   // tiles per axis at this zoom
  bool done = false;
  while (max_tiles > 0) {
    if (_rc_ty > _rc_ty1) {        // cursor walked past the last row
      if (_rc_phase == 0) {        // viewport filled -> now sweep the surrounding margin
        _rc_phase = 1;
        cursorFromWorldRect(_rc_ox, _rc_oy, _rc_ox + _cw - 1, _rc_oy + _ch - 1);
        continue;                  // (viewport tiles re-blit as cheap cache hits)
      }
      _rc_in_progress = false;
      _rc_no_fill = false;         // consume the pan flags
      _rc_strip_only = false;
      _buf_complete = true;        // whole buffer now valid for this origin -> next pan can strip-only
      done = true;
      // Empty-state only reflects a FULL render (a soft pass only touches the exposed strip,
      // so a near-empty strip mustn't hide a populated interior).
      if (_empty_lbl && _rc_was_full) {
        if (_rc_drawn == 0) lv_obj_clear_flag(_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(_empty_lbl, LV_OBJ_FLAG_HIDDEN);
      }
      break;
    }
    int tx = _rc_tx, ty = _rc_ty;
    if (++_rc_tx > _rc_tx1) { _rc_tx = _rc_tx0; _rc_ty++; }  // advance row-major
    if (tx < 0 || ty < 0 || tx >= ntiles || ty >= ntiles) continue;  // outside coverage -> leave bg
    if (_rc_strip_only) {          // only the freshly-exposed strip needs redrawing (interior is valid)
      int cx = (int)((int64_t)tx * 256 - _rc_ox);
      int cy = (int)((int64_t)ty * 256 - _rc_oy);
      bool in_x_strip = (_rc_sx > 0 && cx < _rc_sx) || (_rc_sx < 0 && cx + 256 > _cw + _rc_sx);
      bool in_y_strip = (_rc_sy > 0 && cy < _rc_sy) || (_rc_sy < 0 && cy + 256 > _ch + _rc_sy);
      if (!in_x_strip && !in_y_strip) continue;   // interior preserved by shiftCanvas
    }
    max_tiles--;
    drawTile(_rc_z, tx, ty, _rc_ox, _rc_oy);
  }
  lv_obj_invalidate(_canvas);   // progressive: show what's been drawn so far (LVGL clips to the viewport)
  return done;                  // true when the whole pass (viewport + margin) is complete
}

void MapView::drawTile(int z, int tx, int ty, int64_t ox, int64_t oy) {
  int dx = (int)((int64_t)tx * 256 - ox), dy = (int)((int64_t)ty * 256 - oy);
  if (cacheBlit(z, tx, ty, dx, dy)) { _rc_drawn++; return; }   // hit (maybe prefetched) -> blit only
  cacheEnsure(z, tx, ty);                                       // miss -> decode (SD+PNG, off-lock)
  if (cacheBlit(z, tx, ty, dx, dy)) _rc_drawn++;               // now blit; still miss -> tile absent on SD
}

void MapView::cacheLock()   { if (_cache_mtx) xSemaphoreTake((SemaphoreHandle_t)_cache_mtx, portMAX_DELAY); }
void MapView::cacheUnlock() { if (_cache_mtx) xSemaphoreGive((SemaphoreHandle_t)_cache_mtx); }

// Blit a cached tile into the canvas, HELD under the cache lock so a concurrent evict (prefetch task)
// can't free the buffer mid-copy. Returns false (no blit) if the tile isn't in the cache.
bool MapView::cacheBlit(int z, int tx, int ty, int dst_x, int dst_y) {
  cacheLock();
  for (auto& e : _tcache) {
    if (e.px && e.z == z && e.tx == tx && e.ty == ty) {
      e.lru = ++_lru_clock;
      blit565(e.px, e.w, e.h, dst_x, dst_y);
      cacheUnlock();
      return true;
    }
  }
  cacheUnlock();
  return false;
}

// Decode (z,tx,ty) into the cache if absent. The slow SD read + PNG decode + RGB565 conversion run
// OUTSIDE the lock (on their own buffer); only the dedup checks + slot store are locked. Called by both
// the UI core (drawTile fallback) and the background prefetch task -> the lock makes that safe.
void MapView::cacheEnsure(int z, int tx, int ty) {
  cacheLock();                                  // already cached? (e.g. the prefetch task beat us)
  for (auto& e : _tcache) if (e.px && e.z == z && e.tx == tx && e.ty == ty) { cacheUnlock(); return; }
  cacheUnlock();

  char path[48];
  snprintf(path, sizeof(path), "/tiles/%d/%d/%d.png", z, tx, ty);
  uint8_t* rgba = nullptr; uint32_t iw = 0, ih = 0;
  if (!decodeTileRGBA(path, &rgba, &iw, &ih)) return;          // missing/!ok -> leave it uncached
  lv_color_t* px = (lv_color_t*)heap_caps_malloc((size_t)iw * ih * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (px) for (uint32_t i = 0; i < iw * ih; i++) px[i] = lv_color_make(rgba[i*4], rgba[i*4+1], rgba[i*4+2]);
  heap_caps_free(rgba);
  if (!px) return;

  cacheLock();
  for (auto& e : _tcache) if (e.px && e.z == z && e.tx == tx && e.ty == ty) { cacheUnlock(); heap_caps_free(px); return; }  // raced -> drop ours
  TileCacheEntry* free_e = nullptr; TileCacheEntry* lru_e = nullptr;
  for (auto& e : _tcache) {
    if (!e.px && !free_e) free_e = &e;
    if (e.px && (!lru_e || e.lru < lru_e->lru)) lru_e = &e;
  }
  TileCacheEntry* e = free_e ? free_e : lru_e;
  if (e) {
    if (e->px) heap_caps_free(e->px);
    e->px = px; e->z = z; e->tx = tx; e->ty = ty; e->w = (uint16_t)iw; e->h = (uint16_t)ih; e->lru = ++_lru_clock;
  } else heap_caps_free(px);
  cacheUnlock();
}

void MapView::tileCacheEvictAll() {
  cacheLock();
  for (auto& e : _tcache) { if (e.px) { heap_caps_free(e.px); e.px = nullptr; } e.z = -1; }
  cacheUnlock();
}

// --- background prefetch task ---------------------------------------------------
void MapView::prefetchTaskFn(void* arg) {
  MapView* self = (MapView*)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   // sleep until the UI core posts a request
    self->runPrefetch();
  }
}

// Queue a directional decode-ahead for the current origin/zoom (UI core, after a pan).
void MapView::prefetchPost(int dx, int dy) {
  if (!_prefetch_task) return;
  cacheLock();
  _pf_z = _z; _pf_ox = _origin_wx; _pf_oy = _origin_wy; _pf_dx = dx; _pf_dy = dy; _pf_gen++;
  cacheUnlock();
  xTaskNotifyGive((TaskHandle_t)_prefetch_task);
}

// Decode the band a continued pan in the (dx,dy) direction will reveal -- one ~half-viewport beyond the
// buffer edge on the DOMINANT axis, full perpendicular extent -- into the cache, so the next pan's
// exposed strip is a cache hit (fast refill between quick swipes). Bails on a newer request or on leave.
void MapView::runPrefetch() {
  cacheLock();
  int z = _pf_z, dx = _pf_dx, dy = _pf_dy; int64_t ox = _pf_ox, oy = _pf_oy; uint32_t gen = _pf_gen;
  cacheUnlock();
  if (!_pf_active || z <= 0 || (dx == 0 && dy == 0)) return;
  const int ntiles = 1 << z;
  int64_t x0 = ox, y0 = oy, x1 = ox + _cw - 1, y1 = oy + _ch - 1;
  if (abs(dx) >= abs(dy)) {                                   // horizontal: band past the left/right edge
    int depth = _vw / 2;
    if (dx > 0) { x0 = ox - depth; x1 = ox - 1; }             // content moved right -> next reveal is LEFT
    else        { x0 = ox + _cw;   x1 = ox + _cw + depth - 1; }
  } else {                                                    // vertical: band past the top/bottom edge
    int depth = _vh / 2;
    if (dy > 0) { y0 = oy - depth; y1 = oy - 1; }
    else        { y0 = oy + _ch;   y1 = oy + _ch + depth - 1; }
  }
  int tx0 = (int)floor((double)x0 / 256), ty0 = (int)floor((double)y0 / 256);
  int tx1 = (int)floor((double)x1 / 256), ty1 = (int)floor((double)y1 / 256);
  for (int ty = ty0; ty <= ty1; ty++)
    for (int tx = tx0; tx <= tx1; tx++) {
      if (gen != _pf_gen || !_pf_active) return;              // superseded / left the map -> stop
      if (tx < 0 || ty < 0 || tx >= ntiles || ty >= ntiles) continue;
      cacheEnsure(z, tx, ty);
    }
}

// RGB565 tile -> canvas, row/col-clipped (per-row memcpy of the visible span).
void MapView::blit565(const lv_color_t* src, uint32_t w, uint32_t h, int dst_x, int dst_y) {
  for (uint32_t row = 0; row < h; row++) {
    int cy = dst_y + (int)row;
    if (cy < 0 || cy >= _ch) continue;
    int cx = dst_x, col = 0, cnt = (int)w;
    if (cx < 0) { col = -cx; cnt -= col; cx = 0; }     // clip left
    if (cx + cnt > _cw) cnt = _cw - cx;                // clip right
    if (cnt <= 0) continue;
    memcpy(_cbuf + (size_t)cy * _cw + cx, src + (size_t)row * w + col, (size_t)cnt * sizeof(lv_color_t));
  }
}

// Scroll the whole buffer by (sx,sy) pixels (positive = right/down) in place, filling the
// vacated edges with background. Used on pan: the overlap is preserved (no re-decode), only
// the newly-exposed strip is redrawn afterward by a soft recompose.
void MapView::shiftCanvas(int sx, int sy) {
  if (!_cbuf || (sx == 0 && sy == 0)) return;
  lv_color_t bg = lv_color_hex(BG_HEX);
  const int W = _cw, H = _ch;
  if (sx <= -W || sx >= W || sy <= -H || sy >= H) {   // shifted entirely away -> just clear
    for (int i = 0; i < W * H; i++) _cbuf[i] = bg;
    return;
  }
  // Walk destination rows in an order that never reads an already-overwritten source row.
  int dstart = (sy >= 0) ? H - 1 : 0;
  int dend   = (sy >= 0) ? -1 : H;
  int dstep  = (sy >= 0) ? -1 : 1;
  for (int dy = dstart; dy != dend; dy += dstep) {
    lv_color_t* drow = _cbuf + (size_t)dy * W;
    int syr = dy - sy;
    if (syr < 0 || syr >= H) { for (int i = 0; i < W; i++) drow[i] = bg; continue; }   // new row
    lv_color_t* srow = _cbuf + (size_t)syr * W;
    if (sx >= 0) {
      memmove(drow + sx, srow, (size_t)(W - sx) * sizeof(lv_color_t));
      for (int i = 0; i < sx; i++) drow[i] = bg;
    } else {
      int a = -sx;
      memmove(drow, srow + a, (size_t)(W - a) * sizeof(lv_color_t));
      for (int i = W - a; i < W; i++) drow[i] = bg;
    }
  }
}

void MapView::recompose() {
  if (!_canvas) return;
  beginRecompose();
  while (!stepRecompose(8)) { /* drain fully (used by non-incremental callers) */ }
}

void MapView::repositionMarkers() {
  if (!_canvas) return;
  int sx, sy;

  // Self marker.
  int32_t slat = 0, slon = 0;
  mproxy::selfLatLon(slat, slon);
  if ((slat || slon) && markerScreenPos(slat / 1e6, slon / 1e6, sx, sy)) {
    lv_obj_set_pos(_self_marker, sx - 8, sy - 8);   // 16px dot centered on the fix
    lv_obj_clear_flag(_self_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_self_marker);
  } else {
    lv_obj_add_flag(_self_marker, LV_OBJ_FLAG_HIDDEN);
  }

  // Contact markers.
  int slot = 0;
  mproxy::beginUiRead();
  int n = mproxy::getNumContacts();
  for (int i = 0; i < n && slot < MARKER_MAX; i++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (c.gps_lat == 0 && c.gps_lon == 0) continue;
    if (!markerScreenPos(c.gps_lat / 1e6, c.gps_lon / 1e6, sx, sy)) continue;
    lv_obj_t* m = _markers[slot];
    lv_obj_set_pos(m, sx - 6, sy - 6);              // 12px dot centered
    memcpy(_marker_key[slot], c.id.pub_key, 32);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
    slot++;
  }
  mproxy::endUiRead();

  for (int i = slot; i < MARKER_MAX; i++) lv_obj_add_flag(_markers[i], LV_OBJ_FLAG_HIDDEN);
}

// --- lifecycle ------------------------------------------------------------------
// 1x1 placeholder the canvas points at whenever the real PSRAM buffer (_cbuf) isn't allocated --
// at build time (before the first show) and while the map is hidden. Keeps lv_canvas valid (never
// pointing at freed memory) so a stray render can't fault.
static lv_color_t s_map_stub[1];

void MapView::build(lv_obj_t* parent, uint16_t w, uint16_t h, MarkerTapCb cb, void* user) {
  if (_canvas) return;
  _root = parent; _tap_cb = cb; _tap_user = user;
  _vw = w; _vh = h;

  // Oversized buffer: viewport + a pre-rendered margin on every side, so a drag reveals real
  // content live; on release we scroll the buffer and only decode the freshly-exposed strip.
  // The margin is a fraction f of the viewport, BYTE-CAPPED so the buffer fits PSRAM. `vp` is one
  // viewport in BYTES; the buffer is (1+2f)^2 * vp, so (1+2f)^2 * vp <= CAP -> f = (sqrt(CAP/vp)-1)/2.
  // Clamp f to [0,1]: small screens keep the full (f=1) one-viewport-per-side margin (~1.9 MB at
  // 480x232), big 800x480 screens shrink it to fit (a full margin there would be ~5.6 MB).
  // The PSRAM buffer itself is allocated LAZILY in allocCanvasBuf() on onShow() and freed on onHide(),
  // so the map holds none of this ~2 MB while you're on another tab. Here we only fix the geometry.
  const size_t MAP_BUF_CAP = 2u * 1024 * 1024;
  double vp = (double)_vw * _vh * sizeof(lv_color_t);
  double f = (sqrt((double)MAP_BUF_CAP / vp) - 1.0) / 2.0;
  if (f > 1.0) f = 1.0; else if (f < 0.0) f = 0.0;
  _mx = (uint16_t)(f * _vw); _my = (uint16_t)(f * _vh);
  _cw = _vw + 2 * _mx; _ch = _vh + 2 * _my;

  // Cache mutex + background prefetch task (core 0, low priority): the task decodes look-ahead tiles
  // into the cache off the UI core so quick successive swipes don't outrun the renderer. It only
  // touches the (mutex-guarded) tile cache, never _cbuf, so it can't race the LVGL flush.
  _cache_mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(prefetchTaskFn, "mapPrefetch", 8192, this, 1, (TaskHandle_t*)&_prefetch_task, 0);

  // Layer holds the canvas + markers; its resting position is (-_mx,-_my) so the viewport
  // (the buffer's center region) aligns with the tab. A drag offsets it from that base.
  _map_layer = lv_obj_create(parent);
  lv_obj_set_size(_map_layer, _cw, _ch);
  lv_obj_set_pos(_map_layer, -_mx, -_my);
  lv_obj_set_style_bg_color(_map_layer, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_map_layer, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_map_layer, 0, 0);
  lv_obj_set_style_radius(_map_layer, 0, 0);
  lv_obj_set_style_pad_all(_map_layer, 0, 0);
  lv_obj_clear_flag(_map_layer, LV_OBJ_FLAG_SCROLLABLE);

  _canvas = lv_canvas_create(_map_layer);
  lv_canvas_set_buffer(_canvas, s_map_stub, 1, 1, LV_IMG_CF_TRUE_COLOR);   // real _cbuf bound in onShow()
  lv_obj_align(_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);     // so it receives press events for pan
  lv_obj_add_event_cb(_canvas, canvas_event_cb, LV_EVENT_ALL, this);

  // Overlay control buttons (top-right, stacked): zoom in / out / recenter-on-me.
  struct { lv_obj_t** slot; const char* sym; int yoff; } btns[] = {
    { &_btn_zin,  LV_SYMBOL_PLUS,  6 },
    { &_btn_zout, LV_SYMBOL_MINUS, 48 },
    { &_btn_self, LV_SYMBOL_GPS,   90 },
  };
  for (auto& bd : btns) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_set_size(b, 36, 36);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -6, bd.yoff);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, bd.sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, zoom_btn_cb, LV_EVENT_CLICKED, this);
    *bd.slot = b;
  }

  // Self marker (distinct accent dot, not a contact -> not tappable).
  _self_marker = lv_obj_create(_map_layer);
  lv_obj_set_size(_self_marker, 16, 16);
  lv_obj_set_style_radius(_self_marker, 8, 0);
  lv_obj_set_style_border_width(_self_marker, 3, 0);
  lv_obj_set_style_border_color(_self_marker, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(_self_marker, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_bg_opa(_self_marker, LV_OPA_COVER, 0);
  lv_obj_clear_flag(_self_marker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(_self_marker, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(_self_marker, LV_OBJ_FLAG_HIDDEN);

  // Empty-state hint, pinned to the bottom of the map (on _root, so it doesn't pan and
  // leaves the middle clear for markers). Shown when a recompose renders zero tiles.
  _empty_lbl = lv_label_create(parent);
  lv_label_set_text(_empty_lbl, "No map tiles on SD  -  /tiles/<z>/<x>/<y>.png");
  lv_obj_set_style_text_align(_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(_empty_lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(_empty_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(_empty_lbl, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(_empty_lbl, LV_OPA_60, 0);
  lv_obj_set_style_pad_all(_empty_lbl, 6, 0);
  lv_obj_set_style_radius(_empty_lbl, 6, 0);
  lv_obj_align(_empty_lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_add_flag(_empty_lbl, LV_OBJ_FLAG_HIDDEN);

  // Marker overlay pool (hidden dots; positioned when contacts are plotted).
  for (int i = 0; i < MARKER_MAX; i++) {
    lv_obj_t* m = lv_obj_create(_map_layer);
    lv_obj_set_size(m, 12, 12);
    lv_obj_set_style_radius(m, 6, 0);
    lv_obj_set_style_border_width(m, 2, 0);
    lv_obj_set_style_border_color(m, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(m, marker_event_cb, LV_EVENT_CLICKED, this);
    _markers[i] = m;
  }
}

// (Re)allocate the oversized PSRAM canvas buffer for the current geometry and bind it to the canvas.
// Called from onShow(); idempotent. On alloc failure, falls back to a no-margin (viewport-only) buffer.
bool MapView::allocCanvasBuf() {
  if (_cbuf) return true;
  size_t bytes = (size_t)_cw * _ch * sizeof(lv_color_t);
  _cbuf = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!_cbuf && (_mx || _my)) {              // retry smaller: drop the margin to a viewport-only buffer
    _mx = _my = 0; _cw = _vw; _ch = _vh;
    lv_obj_set_size(_map_layer, _cw, _ch);
    bytes = (size_t)_cw * _ch * sizeof(lv_color_t);
    _cbuf = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  }
  if (!_cbuf) { Serial.printf("[MAP] canvas alloc %u bytes FAILED\n", (unsigned)bytes); return false; }
  lv_canvas_set_buffer(_canvas, _cbuf, _cw, _ch, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(_canvas, lv_color_hex(BG_HEX), LV_OPA_COVER);
  lv_obj_set_pos(_map_layer, -_mx, -_my);    // resting position aligns the buffer center with the viewport
  _buf_complete = false;
  return true;
}

// Detach the canvas from _cbuf (point it at the 1x1 stub) and free the buffer. The big PSRAM block is
// returned whenever the map isn't on screen; allocCanvasBuf() re-creates it on the next onShow().
void MapView::freeCanvasBuf() {
  if (!_cbuf) return;
  if (_canvas) lv_canvas_set_buffer(_canvas, s_map_stub, 1, 1, LV_IMG_CF_TRUE_COLOR);
  heap_caps_free(_cbuf); _cbuf = nullptr;
  _buf_complete = false;
}

void MapView::destroy() {
  if (!_canvas) return;
  tileCacheEvictAll();
  lv_obj_del(_map_layer);   // cascades the canvas + self + marker pool
  _map_layer = nullptr; _canvas = nullptr; _self_marker = nullptr;
  for (int i = 0; i < MARKER_MAX; i++) _markers[i] = nullptr;
  if (_btn_zin)  { lv_obj_del(_btn_zin);  _btn_zin  = nullptr; }
  if (_btn_zout) { lv_obj_del(_btn_zout); _btn_zout = nullptr; }
  if (_btn_self) { lv_obj_del(_btn_self); _btn_self = nullptr; }
  if (_empty_lbl) { lv_obj_del(_empty_lbl); _empty_lbl = nullptr; }
  if (_cbuf) { heap_caps_free(_cbuf); _cbuf = nullptr; }
}

void MapView::onShow() {
  if (!_canvas) return;
  if (!allocCanvasBuf()) return;   // (re)allocate the big canvas buffer for this visit (freed on hide)
  // Tile decode wants ~256 KB PSRAM transients; reclaim the emoji glyph cache (up to
  // ~768 KB) if we're getting tight. The emoji cache repopulates lazily afterward.
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1024 * 1024) SdSvc::emojiBitmapCacheEvict();
  _pf_active = true;   // allow background prefetch while the map is on screen
  if (!_centered_once) { _centered_once = true; centerOnSelf(); }
  // Force a clean full render on (re)entry -- the buffer may be stale/partial from a prior visit.
  _recompose_pending = true;
  _rc_in_progress = false;
  _rc_no_fill = false;
  _rc_strip_only = false;
  _buf_complete = false;
  _markers_dirty = true;
}

void MapView::onHide() {
  _dragging = false;
  _pf_active = false;    // stop the prefetch task before we free the cache it writes into
  _pf_gen++;             // cancel any in-flight sweep
  tileCacheEvictAll();   // return the ~2 MB tile cache while the map isn't on screen
  freeCanvasBuf();       // ...and the ~2 MB canvas buffer too -- the map holds no big RAM while hidden
}

bool MapView::service(uint32_t now_ms) {
  if (!_canvas || !_cbuf) return false;   // no buffer bound (map hidden) -> nothing to render
  bool did = false;
  if (_recompose_pending && (now_ms - _last_recompose_ms) >= 40) {
    if (!_rc_in_progress) beginRecompose();         // fresh pass (origin/zoom snapshot)
    bool done = stepRecompose(2);                    // a couple tiles per pass -> short loop iterations
    _last_recompose_ms = now_ms;
    if (done) _recompose_pending = false;            // else keep pending; resume next pass
    did = true;
  }
  if (_markers_dirty) { _markers_dirty = false; repositionMarkers(); did = true; }
  return did;
}

void MapView::markContactsDirty() { _markers_dirty = true; }

// Pan by (dx,dy) screen pixels (dx>0 = content moves right, revealing the left). Scrolls the
// existing buffer pixels into place so what was on screen stays put, then queues a SOFT recompose
// that decodes only the freshly-exposed strip. Used for both touch-drag release and trackball pan.
void MapView::panBy(int dx, int dy) {
  if (!_canvas || !_cbuf) return;
  int64_t ox0 = _origin_wx, oy0 = _origin_wy;
  _origin_wx -= dx; _origin_wy -= dy;
  clampOrigin();
  int sx = (int)(ox0 - _origin_wx);   // actual content shift (after coverage clamp)
  int sy = (int)(oy0 - _origin_wy);
  if (sx == 0 && sy == 0) { lv_obj_set_pos(_map_layer, -_mx, -_my); return; }
  shiftCanvas(sx, sy);
  lv_obj_set_pos(_map_layer, -_mx, -_my);   // back to base; buffer now holds the shifted view (no snap)
  lv_obj_invalidate(_canvas);
  _rc_sx = sx; _rc_sy = sy;
  _rc_in_progress = false;
  _rc_no_fill = true;                  // keep the shifted pixels (never clear -> no flash)
  _rc_strip_only = _buf_complete;      // fast strip-fill ONLY if the buffer was fully valid; else redraw all to fix gaps
  _buf_complete = false;
  _recompose_pending = true;
  repositionMarkers();                 // markers to the new origin immediately (no 1-frame lag)
  prefetchPost(sx, sy);                // decode-ahead the next reveal in this motion direction (off the UI core)
}

// --- event trampolines ----------------------------------------------------------
void MapView::canvas_event_cb(lv_event_t* e) {
  MapView* s = (MapView*)lv_event_get_user_data(e);
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;

  if (code == LV_EVENT_PRESSED) {
    lv_indev_get_point(indev, &s->_drag_last);
    s->_dragging = true; s->_drag_dx = 0; s->_drag_dy = 0;

  } else if (code == LV_EVENT_PRESSING) {
    if (!s->_dragging) return;
    lv_point_t p; lv_indev_get_point(indev, &p);
    s->_drag_dx += p.x - s->_drag_last.x;
    s->_drag_dy += p.y - s->_drag_last.y;
    s->_drag_last = p;
    // Live pan: slide the layer off its margin base. Pre-rendered margin fills the gap in real time.
    lv_obj_set_pos(s->_map_layer, -s->_mx + s->_drag_dx, -s->_my + s->_drag_dy);

  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (!s->_dragging) return;
    s->_dragging = false;
    int dx = s->_drag_dx, dy = s->_drag_dy;

    if (abs(dx) < 6 && abs(dy) < 6) {      // barely moved -> a tap (maybe a double-tap)
      lv_obj_set_pos(s->_map_layer, -s->_mx, -s->_my);   // reset to base
      lv_point_t p; lv_indev_get_point(indev, &p);
      uint32_t now = lv_tick_get();
      if (now - s->_last_tap_ms < 350 &&
          abs(p.x - s->_last_tap_pt.x) < 24 && abs(p.y - s->_last_tap_pt.y) < 24) {
        s->_last_tap_ms = 0;
        s->zoomInAt(p.x, p.y);             // double-tap -> zoom in toward the tapped point
      } else {
        s->_last_tap_ms = now; s->_last_tap_pt = p;
      }
      return;
    }
    // Commit: scroll the buffer to the dropped position (content stays put) + fill the new strip.
    s->panBy(dx, dy);
  }
}

void MapView::zoom_btn_cb(lv_event_t* e) {
  MapView* s = (MapView*)lv_event_get_user_data(e);
  lv_obj_t* b = lv_event_get_target(e);
  if (b == s->_btn_zin)       s->setZoomKeepCenter(s->_z + 1);
  else if (b == s->_btn_zout) s->setZoomKeepCenter(s->_z - 1);
  else if (b == s->_btn_self) s->centerOnSelf();
  s->_markers_dirty = true;
}

void MapView::marker_event_cb(lv_event_t* e) {
  MapView* s = (MapView*)lv_event_get_user_data(e);
  lv_obj_t* m = lv_event_get_target(e);
  for (int i = 0; i < MARKER_MAX; i++) {
    if (s->_markers[i] == m) {
      if (s->_tap_cb) s->_tap_cb(s->_marker_key[i], s->_tap_user);
      return;
    }
  }
}

// --- spike / diagnostic ---------------------------------------------------------
void MapView::selfTest() {
  // A few real tiles from the user's set (z14, US Pacific NW). 1-bit, dense, etc.
  static const char* paths[] = {
    "/tiles/14/2749/5849.png",
    "/tiles/14/2562/5833.png",
    "/tiles/1/0/0.png",
  };
  Serial.printf("[MAP] selfTest: SD ready=%d freePSRAM=%u\n",
                (int)SdSvc::ready(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  for (auto path : paths) {
    uint32_t t0 = millis();
    uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
    bool ok = decodeTileRGBA(path, &rgba, &w, &h);
    uint32_t dt = millis() - t0;
    Serial.printf("[MAP]   %-26s ok=%d %ux%u %ums freePSRAM=%u\n",
                  path, (int)ok, (unsigned)w, (unsigned)h, (unsigned)dt,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (rgba) heap_caps_free(rgba);
  }
}

// ================================ MapThumb ======================================
// A small static z14 basemap square centered on a fixed location.
void MapThumb::create(lv_obj_t* parent, int side) {
  if (_canvas || side <= 0) return;
  _side = side;
  _buf = (lv_color_t*)heap_caps_malloc((size_t)side * side * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!_buf) return;

  // Full-width wrapper so the square centers under the hero in the flex column.
  _wrap = lv_obj_create(parent);
  lv_obj_remove_style_all(_wrap);
  lv_obj_set_width(_wrap, LV_PCT(100));
  lv_obj_set_height(_wrap, side);
  lv_obj_set_flex_flow(_wrap, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(_wrap, LV_OBJ_FLAG_SCROLLABLE);

  _canvas = lv_canvas_create(_wrap);
  lv_canvas_set_buffer(_canvas, _buf, side, side, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(_canvas, lv_color_hex(BG_HEX), LV_OPA_COVER);
  lv_obj_set_style_radius(_canvas, 8, 0);
  lv_obj_set_style_clip_corner(_canvas, true, 0);
  lv_obj_set_style_border_width(_canvas, 1, 0);
  lv_obj_set_style_border_color(_canvas, lv_color_hex(UI_DIM), 0);

  lv_obj_add_flag(_wrap, LV_OBJ_FLAG_HIDDEN);   // shown by render() once a location is known
}

void MapThumb::render(double lat, double lon) {
  if (!_buf || !_canvas) return;
  if (_shown && fabs(lat - _lat) < 1e-7 && fabs(lon - _lon) < 1e-7) return;   // unchanged
  _lat = lat; _lon = lon;

  const int Z = 14;
  lv_color_t bg = lv_color_hex(BG_HEX);
  for (int i = 0; i < _side * _side; i++) _buf[i] = bg;

  int64_t cx = (int64_t)llround(lonToWorldX(lon, Z));
  int64_t cy = (int64_t)llround(latToWorldY(lat, Z));
  int64_t ox = cx - _side / 2, oy = cy - _side / 2;      // buffer top-left in world px
  int ntiles = 1 << Z;
  int tx0 = (int)floor((double)ox / 256.0), ty0 = (int)floor((double)oy / 256.0);
  int tx1 = (int)floor((double)(ox + _side - 1) / 256.0), ty1 = (int)floor((double)(oy + _side - 1) / 256.0);
  for (int ty = ty0; ty <= ty1; ty++) {
    for (int tx = tx0; tx <= tx1; tx++) {
      if (tx < 0 || ty < 0 || tx >= ntiles || ty >= ntiles) continue;
      char path[48];
      snprintf(path, sizeof(path), "/tiles/%d/%d/%d.png", Z, tx, ty);
      uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
      if (!MapView::decodeTileRGBA(path, &rgba, &w, &h)) continue;
      int dx = (int)((int64_t)tx * 256 - ox), dy = (int)((int64_t)ty * 256 - oy);
      for (uint32_t r = 0; r < h; r++) {
        int yy = dy + (int)r; if (yy < 0 || yy >= _side) continue;
        const uint8_t* s = rgba + (size_t)r * w * 4;
        lv_color_t* d = _buf + (size_t)yy * _side;
        for (uint32_t cc = 0; cc < w; cc++) {
          int xx = dx + (int)cc; if (xx < 0 || xx >= _side) continue;
          d[xx] = lv_color_make(s[cc * 4], s[cc * 4 + 1], s[cc * 4 + 2]);
        }
      }
      heap_caps_free(rgba);
    }
  }

  // Center marker (the location is always at the buffer center): white-ringed accent dot.
  int mc = _side / 2;
  lv_color_t accent = lv_color_hex(UI_PRIMARY), ring = lv_color_hex(0xFFFFFF);
  for (int dy = -6; dy <= 6; dy++) {
    for (int dx = -6; dx <= 6; dx++) {
      int d2 = dx * dx + dy * dy; if (d2 > 36) continue;
      int xx = mc + dx, yy = mc + dy;
      if (xx < 0 || xx >= _side || yy < 0 || yy >= _side) continue;
      _buf[(size_t)yy * _side + xx] = (d2 >= 16) ? ring : accent;
    }
  }

  lv_obj_invalidate(_canvas);
  if (!_shown) { lv_obj_clear_flag(_wrap, LV_OBJ_FLAG_HIDDEN); _shown = true; }
}

void MapThumb::hide() {
  if (_wrap) lv_obj_add_flag(_wrap, LV_OBJ_FLAG_HIDDEN);
  _shown = false; _lat = 1e9; _lon = 1e9;   // force a redraw next time it's shown
}
