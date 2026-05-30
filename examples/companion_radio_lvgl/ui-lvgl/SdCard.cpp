#include "SdCard.h"
#include <Arduino.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"

// Low-level bus + mount primitives provided by the variant (target.cpp): they
// know the SD pins and the LoRa SPI to share. sd_card_begin() brackets its own
// bus access and (re)mounts the shared `sd` handle.
extern void sd_bus_to_sd();
extern void sd_bus_to_lora();
extern bool sd_card_begin();
// Shared HSPI mutex (LoRa + SD), defined in the variant's target.cpp. Held
// across the whole SD bus span so a re-pin never races the radio on the other
// core. See target.cpp.
extern void hspi_lock();
extern void hspi_unlock();

namespace SdSvc {

static bool     s_mounted = false;
static uint32_t s_retry_ms = 0;   // last (re)mount attempt
static int      s_fail_count = 0;
static bool     s_gave_up = false;  // no card: stop hammering the bus (each failed
                                    // mount stalls the shared HSPI while it times out)

Lock::Lock()  { hspi_lock(); sd_bus_to_sd(); }
Lock::~Lock() { sd_bus_to_lora(); hspi_unlock(); }

bool ready() { return s_mounted; }

// Re-enable mount attempts (after giving up): call when a card may have been
// inserted, or to force a re-check.
void rescan() { s_gave_up = false; s_fail_count = 0; s_retry_ms = 0; }

bool ensureMounted() {
  if (s_mounted) return true;
  if (s_gave_up) return false;     // missing/dead card: don't stall the bus every few s
  uint32_t now = millis();
  if (s_retry_ms != 0 && (uint32_t)(now - s_retry_ms) < 3000) return false;  // throttle
  s_retry_ms = now ? now : 1;
  s_mounted = sd_card_begin();   // variant re-pins, mounts, restores the bus
  if (!s_mounted && ++s_fail_count >= 3) s_gave_up = true;  // give up -> graceful no-SD
  return s_mounted;
}

// ---- lv_fs driver (drive 'S:') so LVGL can read SD files (emoji images, etc.) ----
// Read-only. Holds the bus for the open..close span (single-threaded, so a short
// image read completes before the mesh touches the radio again).
//
// Emoji are served from per-size *bundles* /emoji/<size>.bin (magic 'EMJ1',
// count, sorted {cp,off,len} index, then concatenated LVGL images). The imgfont
// asks for "/emoji/<size>/<hexcp>.bin"; we map that to a bounded view into the
// bundle via a RAM index (binary search) -- one small dir lookup + one seek,
// instead of scanning a 1400-entry directory per glyph (which was crippling).

struct EmojiEntry { uint32_t cp, off, len; };
struct EmojiIndex { int size; uint32_t count; EmojiEntry* entries; };
static EmojiIndex s_eidx[8];
static int        s_eidx_count = 0;

// A handle is a (possibly bounded) view into an open file. len==FULL means the
// whole file (normal open); otherwise reads/seeks are clamped to [base,base+len).
static const uint32_t FULL = 0xFFFFFFFFu;
struct SdFsHandle { FsFile f; uint32_t base, len, pos; };

// Parse "/emoji/<size>/<hexcp>.bin" -> size, cp. Rejects "/emoji/<size>.bin"
// (the bundle itself) and any non-emoji path.
static bool parseEmojiPath(const char* p, int* size, uint32_t* cp) {
  if (strncmp(p, "/emoji/", 7) != 0) return false;
  p += 7;
  if (!isdigit((unsigned char)*p)) return false;
  int sz = 0;
  while (isdigit((unsigned char)*p)) sz = sz * 10 + (*p++ - '0');
  if (*p != '/') return false;
  p++;
  if (!isxdigit((unsigned char)*p)) return false;
  uint32_t c = 0;
  while (isxdigit((unsigned char)*p)) {
    char ch = *p++;
    c = c * 16 + (uint32_t)((ch <= '9') ? ch - '0' : (tolower(ch) - 'a' + 10));
  }
  if (strcmp(p, ".bin") != 0) return false;
  *size = sz; *cp = c;
  return true;
}

// Return a cached index for a size (RAM only). *known=true if we've already
// tried this size (entries==nullptr means a cached miss -- don't retry SD).
static EmojiIndex* findCached(int size, bool* known) {
  for (int i = 0; i < s_eidx_count; i++)
    if (s_eidx[i].size == size) { *known = true; return s_eidx[i].entries ? &s_eidx[i] : nullptr; }
  *known = false;
  return nullptr;
}
// Read a size's index from an already-open bundle (positioned anywhere) and
// cache it. Caches a miss (entries=nullptr) on any failure so we don't retry.
static EmojiIndex* readIndexInto(int size, FsFile& bundle) {
  if (s_eidx_count >= (int)(sizeof(s_eidx) / sizeof(s_eidx[0]))) return nullptr;
  EmojiIndex& slot = s_eidx[s_eidx_count++];
  slot = {size, 0, nullptr};
  char magic[4];
  uint32_t count = 0;
  bundle.seekSet(0);
  if (bundle.read(magic, 4) != 4 || memcmp(magic, "EMJ1", 4) != 0) return nullptr;
  if (bundle.read(&count, 4) != 4 || count == 0 || count > 5000) return nullptr;
  size_t bytes = (size_t)count * sizeof(EmojiEntry);
  EmojiEntry* e = (EmojiEntry*)ps_malloc(bytes);
  if (!e) e = (EmojiEntry*)malloc(bytes);
  if (!e) return nullptr;
  if (bundle.read((uint8_t*)e, bytes) != (int)bytes) { free(e); return nullptr; }
  slot.count = count;
  slot.entries = e;
  return &slot;
}
// For callers already holding the bus with the bundle open (fs_open).
static EmojiIndex* ensureIndex(int size, FsFile& bundle) {
  bool known;
  EmojiIndex* c = findCached(size, &known);
  if (known) return c;
  return readIndexInto(size, bundle);
}
// For callers NOT holding the bus (the get_info path). Loads the index once per
// size (acquiring the bus itself), then every later call is a pure RAM hit -- so
// per-frame glyph measurement never touches SD.
static EmojiIndex* getIndex(int size) {
  bool known;
  EmojiIndex* c = findCached(size, &known);
  if (known) return c;
  if (!ensureMounted()) return nullptr;
  Lock lk;
  char bpath[24];
  snprintf(bpath, sizeof(bpath), "/emoji/%d.bin", size);
  FsFile bf = sd.open(bpath, O_RDONLY);
  if (!bf.isOpen()) {
    if (s_eidx_count < (int)(sizeof(s_eidx) / sizeof(s_eidx[0])))
      s_eidx[s_eidx_count++] = {size, 0, nullptr};   // cache the miss
    return nullptr;
  }
  EmojiIndex* r = readIndexInto(size, bf);
  bf.close();
  return r;
}

static const EmojiEntry* findEntry(const EmojiIndex* idx, uint32_t cp) {
  int lo = 0, hi = (int)idx->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint32_t v = idx->entries[mid].cp;
    if (v == cp) return &idx->entries[mid];
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

static void* fs_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
  (void)drv; (void)mode;
  if (!ensureMounted()) return nullptr;
  // Hold the HSPI mutex for the whole open..close span (released in fs_close, or
  // on each failure path below). LVGL keeps the handle open across read/seek, so
  // we can't use the RAII Lock here -- the lock must outlive this function.
  hspi_lock();
  sd_bus_to_sd();

  int size; uint32_t cp;
  if (parseEmojiPath(path, &size, &cp)) {
    char bpath[24];
    snprintf(bpath, sizeof(bpath), "/emoji/%d.bin", size);
    SdFsHandle* h = new SdFsHandle();
    h->f = sd.open(bpath, O_RDONLY);
    if (!h->f.isOpen()) { delete h; sd_bus_to_lora(); hspi_unlock(); return nullptr; }
    EmojiIndex* idx = ensureIndex(size, h->f);
    const EmojiEntry* e = idx ? findEntry(idx, cp) : nullptr;
    if (!e) { h->f.close(); delete h; sd_bus_to_lora(); hspi_unlock(); return nullptr; }
    h->f.seekSet(e->off);
    h->base = e->off; h->len = e->len; h->pos = 0;
    return h;
  }

  SdFsHandle* h = new SdFsHandle();
  h->f = sd.open(path, O_RDONLY);   // path is "/..." (drive letter already stripped)
  if (!h->f.isOpen()) { delete h; sd_bus_to_lora(); hspi_unlock(); return nullptr; }
  h->base = 0; h->len = FULL; h->pos = 0;
  return h;
}
static lv_fs_res_t fs_close(lv_fs_drv_t* drv, void* fp) {
  (void)drv;
  SdFsHandle* h = (SdFsHandle*)fp;
  h->f.close();
  delete h;
  sd_bus_to_lora();
  hspi_unlock();   // balances the hspi_lock() in fs_open
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_read(lv_fs_drv_t* drv, void* fp, void* buf, uint32_t btr, uint32_t* br) {
  (void)drv;
  SdFsHandle* h = (SdFsHandle*)fp;
  uint32_t want = btr;
  if (h->len != FULL) {
    uint32_t rem = (h->pos < h->len) ? h->len - h->pos : 0;
    if (want > rem) want = rem;
  }
  int n = want ? h->f.read((uint8_t*)buf, want) : 0;
  if (n < 0) n = 0;
  h->pos += (uint32_t)n;
  *br = (uint32_t)n;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_seek(lv_fs_drv_t* drv, void* fp, uint32_t pos, lv_fs_whence_t whence) {
  (void)drv;
  SdFsHandle* h = (SdFsHandle*)fp;
  uint32_t end = (h->len != FULL) ? h->len : (uint32_t)h->f.size();
  uint32_t np = pos;
  if (whence == LV_FS_SEEK_CUR)      np = h->pos + pos;
  else if (whence == LV_FS_SEEK_END) np = end + pos;
  h->pos = np;
  h->f.seekSet((uint64_t)h->base + np);
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_tell(lv_fs_drv_t* drv, void* fp, uint32_t* pos_p) {
  (void)drv;
  *pos_p = ((SdFsHandle*)fp)->pos;
  return LV_FS_RES_OK;
}
// ---- emoji image decoder (preload to RAM) ----
// LVGL's built-in decoder, for a file-backed true-color image, keeps NO pixels
// in RAM -- it re-reads each line from the file on every draw. With the image
// cache on that meant a fresh SD read per emoji per frame while scrolling. This
// decoder claims our "/emoji/..." sources, reads the whole glyph into a RAM
// buffer once at open, and points img_data at it; the image cache then keeps
// that buffer live, so scrolling redraws are pure RAM blits (and cached emoji
// even survive the SD being pulled).
static bool isEmojiSrc(const void* src) {
  if (lv_img_src_get_type(src) != LV_IMG_SRC_FILE) return false;
  const char* p = (const char*)src;
  return strstr(p, "/emoji/") != nullptr;
}
static lv_res_t emoji_dec_info(lv_img_decoder_t* d, const void* src, lv_img_header_t* header) {
  (void)d;
  if (!isEmojiSrc(src)) return LV_RES_INV;
  const char* p = strstr((const char*)src, "/emoji/");   // skip the "S:" drive letter
  int size; uint32_t cp;
  if (!p || !parseEmojiPath(p, &size, &cp)) return LV_RES_INV;
  // Measure from the RAM index -- imgfont calls this per glyph per frame, so it
  // must NOT touch SD (the index is loaded once per size; dims are deterministic).
  EmojiIndex* idx = getIndex(size);
  if (!idx || !findEntry(idx, cp)) return LV_RES_INV;    // not in the pack -> mono fallback
  uint32_t pad = (size * 8 + 50) / 100;                  // == round(size*0.08), matches the converter
  if (pad < 1) pad = 1;
  header->always_zero = 0;
  header->reserved    = 0;
  header->cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  header->w  = size + 2 * pad;
  header->h  = size;
  return LV_RES_OK;
}
static lv_res_t emoji_dec_open(lv_img_decoder_t* d, lv_img_decoder_dsc_t* dsc) {
  (void)d;
  if (!isEmojiSrc(dsc->src)) return LV_RES_INV;
  lv_fs_file_t f;
  if (lv_fs_open(&f, (const char*)dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) return LV_RES_INV;
  uint32_t h = 0, rn = 0;
  lv_fs_read(&f, &h, 4, &rn);                 // consume the 4-byte lv_img header
  uint32_t w = (h >> 10) & 0x7FF, ht = (h >> 21) & 0x7FF;
  uint32_t bytes = w * ht * LV_IMG_PX_SIZE_ALPHA_BYTE;
  if (rn != 4 || bytes == 0) { lv_fs_close(&f); return LV_RES_INV; }
  uint8_t* buf = (uint8_t*)ps_malloc(bytes);
  if (!buf) buf = (uint8_t*)malloc(bytes);
  if (!buf) { lv_fs_close(&f); return LV_RES_INV; }
  uint32_t off = 0;
  while (off < bytes) {
    uint32_t got = 0;
    if (lv_fs_read(&f, buf + off, bytes - off, &got) != LV_FS_RES_OK || got == 0) break;
    off += got;
  }
  lv_fs_close(&f);
  if (off != bytes) { free(buf); return LV_RES_INV; }
  dsc->img_data = buf;
  dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  dsc->header.w  = w;
  dsc->header.h  = ht;
  return LV_RES_OK;
}
static void emoji_dec_close(lv_img_decoder_t* d, lv_img_decoder_dsc_t* dsc) {
  (void)d;
  if (dsc->img_data) { free((void*)dsc->img_data); dsc->img_data = nullptr; }
}

void registerFs() {
  static bool done = false;
  if (done) return;
  static lv_fs_drv_t drv;
  lv_fs_drv_init(&drv);
  drv.letter   = 'S';
  drv.open_cb  = fs_open;
  drv.close_cb = fs_close;
  drv.read_cb  = fs_read;
  drv.seek_cb  = fs_seek;
  drv.tell_cb  = fs_tell;
  lv_fs_drv_register(&drv);

  lv_img_decoder_t* dec = lv_img_decoder_create();  // newest decoder is tried first
  lv_img_decoder_set_info_cb(dec, emoji_dec_info);
  lv_img_decoder_set_open_cb(dec, emoji_dec_open);
  lv_img_decoder_set_close_cb(dec, emoji_dec_close);
  done = true;
}

bool begin() {
  s_mounted = false;
  rescan();                 // fresh start: allow mount attempts again
  return ensureMounted();   // NOTE: registerFs() is separate -- it needs lv_init() first
}

void end() {
  if (s_mounted) { Lock lk; sd.end(); }
  s_mounted = false;
}

}  // namespace SdSvc
