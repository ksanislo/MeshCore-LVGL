#include "EmojiPack.h"
#include "SdCard.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "rom/miniz.h"      // ESP32-S3 ROM tinfl (malloc-free streaming inflate)

// Repo comes from the OTA build flags (shared with the firmware updater). Default so any build compiles.
#ifndef OTA_GH_OWNER
  #define OTA_GH_OWNER "ksanislo"
#endif
#ifndef OTA_GH_REPO
  #define OTA_GH_REPO "MeshCore-LVGL"
#endif

namespace EmojiPack {

static volatile bool s_busy = false, s_cancel = false, s_fetching = false;
static volatile int  s_progress = 0;
static char s_status[48] = "";

bool busy()     { return s_busy; }
bool fetching() { return s_fetching; }
int  progress() { return s_progress; }
void cancel()   { if (s_busy) s_cancel = true; }
void status(char* out, size_t cap) {
  if (!out || cap == 0) return;
  strncpy(out, s_status[0] ? s_status : "tap to download", cap - 1); out[cap - 1] = 0;
}
static void setStatus(const char* m) { strncpy(s_status, m, sizeof(s_status) - 1); s_status[sizeof(s_status) - 1] = 0; }
// Progress line: "<verb> <n>/N <pct>%" (e.g. "downloading 5/5 72%").
static void setProg(const char* verb, int idx, int pct) {
  char b[40]; snprintf(b, sizeof(b), "%s %d/%d %d%%", verb, idx + 1, EMOJI_SIZE_COUNT, pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  setStatus(b);
}

// Download emoji-<fmt>-<size>.bin.gz and stream-gunzip it to /emoji/<size>.bin. idx = bundle index
// (for the n/N progress line). Returns true on success.
static bool fetchOne(int idx, int size) {
  char url[208];
  snprintf(url, sizeof(url), "https://github.com/%s/%s/releases/download/emoji-%s/emoji-%s-%d.bin.gz",
           OTA_GH_OWNER, OTA_GH_REPO, EMOJI_PACK_FMT, EMOJI_PACK_FMT, size);

  IPAddress rip;                              // bounded DNS pre-resolve (matches the OTA path)
  if (!WiFi.hostByName("github.com", rip)) { setStatus("can't resolve host"); return false; }

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000); http.setConnectTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // github.com -> release-assets CDN 302
  if (!http.begin(client, url)) { setStatus("connect failed"); return false; }
  http.addHeader("User-Agent", "MeshCore-LVGL-OTA");
  int code = http.GET();
  if (code != 200) { http.end(); char b[40]; snprintf(b, sizeof(b), "HTTP %d (sz %d)", code, size); setStatus(b); return false; }

  // Buffer the whole .gz in PSRAM (each <= ~1.5 MB). content-length is provided for asset downloads.
  int len = http.getSize();
  size_t cap = (len > 0) ? (size_t)len + 16 : (2u * 1024 * 1024);
  uint8_t* gz = (uint8_t*)ps_malloc(cap);
  if (!gz) { http.end(); setStatus("no PSRAM"); return false; }
  WiFiClient* st = http.getStreamPtr();
  size_t got = 0; uint32_t last = millis(); int lastdp = -1;
  setProg("downloading", idx, 0);
  while (got < cap - 1) {
    if (s_cancel) break;
    size_t avail = st->available();
    if (avail) {
      int r = st->readBytes(gz + got, avail > (cap - 1 - got) ? (cap - 1 - got) : avail);
      if (r > 0) { got += r; last = millis();
        if (len > 0) { int pct = (int)((uint64_t)got * 100 / (uint32_t)len); if (pct != lastdp) { lastdp = pct; setProg("downloading", idx, pct); } }
      }
    }
    else if (!http.connected()) break;
    else if (millis() - last > 15000) break;
    else delay(2);
  }
  http.end();
  if (s_cancel)                       { free(gz); setStatus("cancelled"); return false; }
  if (len > 0 && got != (size_t)len)  { free(gz); setStatus("short download"); return false; }
  if (got < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 0x08) { free(gz); setStatus("not gzip"); return false; }

  // Skip the gzip header -> raw DEFLATE. (10-byte fixed header + optional fields per FLG.)
  uint8_t flg = gz[3]; size_t p = 10;
  if (flg & 0x04) { if (p + 2 > got) { free(gz); setStatus("bad gz"); return false; } p += 2 + (gz[p] | (gz[p + 1] << 8)); }  // FEXTRA
  if (flg & 0x08) { while (p < got && gz[p]) p++; p++; }   // FNAME
  if (flg & 0x10) { while (p < got && gz[p]) p++; p++; }   // FCOMMENT
  if (flg & 0x02) p += 2;                                  // FHCRC
  if (p >= got)   { free(gz); setStatus("bad gz"); return false; }

  // Uncompressed size from the gzip trailer (ISIZE = original size mod 2^32; our bundles are < 4 MB
  // so it's exact) -- drives the "saving" progress percent.
  uint32_t isize = (uint32_t)gz[got - 4] | ((uint32_t)gz[got - 3] << 8) |
                   ((uint32_t)gz[got - 2] << 16) | ((uint32_t)gz[got - 1] << 24);

  // tinfl streaming state + a 32 KB output dictionary (both PSRAM; ROM tinfl is malloc-free).
  tinfl_decompressor* dec = (tinfl_decompressor*)ps_malloc(sizeof(tinfl_decompressor));
  uint8_t* dict = (uint8_t*)ps_malloc(TINFL_LZ_DICT_SIZE);
  if (!dec || !dict) { free(gz); if (dec) free(dec); if (dict) free(dict); setStatus("no PSRAM"); return false; }
  tinfl_init(dec);

  char path[24]; snprintf(path, sizeof(path), "/emoji/%d.bin", size);
  bool ok = false;
  FsFile f;
  { SdSvc::Lock lk; sd.mkdir("/emoji"); f = sd.open(path, O_WRONLY | O_CREAT | O_TRUNC); }
  if (f.isOpen()) {
    // Inflate in PSRAM and write in ~256 KB batches, RELEASING the HSPI bus lock between batches so the
    // LoRa radio (core 0, sharing this bus) gets it back. Holding one lock across a whole multi-MB
    // inflate starved core 0's idle task past the 5 s task watchdog -> reset (the rc6 crash; only the
    // largest bundle crossed the line). The file stays open across the brief locks -- same discipline
    // as the lv_fs image reader. vTaskDelay yields core 1 (LVGL loop + idle) between batches.
    const uint8_t* in = gz + p; size_t in_avail = got - p; size_t dict_ofs = 0;
    bool fail = false, done = false; uint32_t total_out = 0; int lastwp = -1;
    setProg("saving", idx, 0);
    while (!done && !fail) {
      uint32_t batch = 0;
      {
        SdSvc::Lock lk;                        // bounded hold (~256 KB write, well under the 5 s WDT)
        while (batch < (256u * 1024)) {
          size_t in_bytes = in_avail, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
          tinfl_status s = tinfl_decompress(dec, in, &in_bytes, dict, dict + dict_ofs, &out_bytes,
                                            in_avail ? TINFL_FLAG_HAS_MORE_INPUT : 0);
          in += in_bytes; in_avail -= in_bytes;
          if (out_bytes) {
            if ((size_t)f.write(dict + dict_ofs, out_bytes) != out_bytes) { fail = true; break; }
            total_out += out_bytes; batch += out_bytes;
          }
          dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
          if (s == TINFL_STATUS_DONE) { done = true; break; }
          if (s < TINFL_STATUS_DONE)  { fail = true; break; }   // negative = error
        }
      }                                        // lock released -> radio gets the bus
      if (isize) { int pct = (int)((uint64_t)total_out * 100 / isize); if (pct != lastwp) { lastwp = pct; setProg("saving", idx, pct); } }
      if (!done && !fail) vTaskDelay(1);        // breathe between batches
    }
    { SdSvc::Lock lk; f.close(); }
    ok = !fail;
  }
  free(dec); free(dict); free(gz);
  if (!ok) { setStatus("SD write failed"); return false; }
  return true;
}

static void taskTramp(void*) {
  if (WiFi.status() != WL_CONNECTED) { setStatus("no WiFi"); s_busy = false; vTaskDelete(nullptr); return; }
  if (!SdSvc::ready())               { setStatus("insert + mount SD"); s_busy = false; vTaskDelete(nullptr); return; }
  s_fetching = true;                          // UI shrinks the draw buffers for TLS RAM
  SdSvc::emojiBitmapCacheEvict();             // free PSRAM + drop stale glyphs
  vTaskDelay(pdMS_TO_TICKS(150));             // let the UI loop free the draw buffer before the first TLS
  bool all = true;
  for (int i = 0; i < EMOJI_SIZE_COUNT; i++) {
    if (s_cancel) { all = false; break; }
    if (!fetchOne(i, EMOJI_SIZES[i])) { all = false; break; }
    s_progress = i + 1;
  }
  s_fetching = false;
  SdSvc::emojiBitmapCacheEvict();             // reload glyphs from the new bundles on demand
  if (all)           setStatus("emoji updated");
  else if (s_cancel) setStatus("cancelled");
  s_busy = false;
  vTaskDelete(nullptr);
}

void start() {
  if (s_busy) return;
  s_busy = true; s_cancel = false; s_progress = 0;
  setStatus("starting...");
  if (xTaskCreatePinnedToCore(taskTramp, "emoji", 16384, nullptr, 1, nullptr, 1) != pdPASS) {
    s_busy = false; setStatus("task spawn failed");
  }
}

}  // namespace EmojiPack
