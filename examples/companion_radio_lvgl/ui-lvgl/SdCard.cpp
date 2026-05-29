#include "SdCard.h"
#include <Arduino.h>
#include "lvgl.h"

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

// ---- lv_fs driver (drive 'S:') so LVGL can read SD files (emoji images, etc.) ----
// Read-only. Holds the bus for the open..close span (single-threaded, so a short
// image read completes before the mesh touches the radio again).
static void* fs_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
  (void)drv; (void)mode;
  if (!ensureMounted()) return nullptr;
  sd_bus_to_sd();
  FsFile* f = new FsFile();
  *f = sd.open(path, O_RDONLY);   // path is "/emoji/..." (letter already stripped)
  if (!f->isOpen()) { delete f; sd_bus_to_lora(); return nullptr; }
  return f;
}
static lv_fs_res_t fs_close(lv_fs_drv_t* drv, void* fp) {
  (void)drv;
  FsFile* f = (FsFile*)fp;
  f->close();
  delete f;
  sd_bus_to_lora();
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_read(lv_fs_drv_t* drv, void* fp, void* buf, uint32_t btr, uint32_t* br) {
  (void)drv;
  int n = ((FsFile*)fp)->read((uint8_t*)buf, btr);
  *br = (n < 0) ? 0 : (uint32_t)n;
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_seek(lv_fs_drv_t* drv, void* fp, uint32_t pos, lv_fs_whence_t whence) {
  (void)drv;
  FsFile* f = (FsFile*)fp;
  uint64_t base = 0;
  if (whence == LV_FS_SEEK_CUR) base = f->curPosition();
  else if (whence == LV_FS_SEEK_END) base = f->size();
  f->seekSet(base + pos);
  return LV_FS_RES_OK;
}
static lv_fs_res_t fs_tell(lv_fs_drv_t* drv, void* fp, uint32_t* pos_p) {
  (void)drv;
  *pos_p = (uint32_t)((FsFile*)fp)->curPosition();
  return LV_FS_RES_OK;
}
void registerFs() {
  static lv_fs_drv_t drv;
  static bool done = false;
  if (done) return;
  lv_fs_drv_init(&drv);
  drv.letter   = 'S';
  drv.open_cb  = fs_open;
  drv.close_cb = fs_close;
  drv.read_cb  = fs_read;
  drv.seek_cb  = fs_seek;
  drv.tell_cb  = fs_tell;
  lv_fs_drv_register(&drv);
  done = true;
}

bool begin() {
  s_mounted = false;
  s_retry_ms = 0;
  return ensureMounted();   // NOTE: registerFs() is separate -- it needs lv_init() first
}

void end() {
  if (s_mounted) { Lock lk; sd.end(); }
  s_mounted = false;
}

}  // namespace SdSvc
