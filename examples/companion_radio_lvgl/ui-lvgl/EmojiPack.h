#pragma once
#include <stddef.h>

// On-device emoji-pack downloader. Pulls the per-size gzip bundles for the running firmware's emoji
// format (EMOJI_PACK_FMT) from the dedicated GitHub "emoji-<fmt>" release, gunzips each on the fly
// (ROM tinfl streaming), and writes them to /emoji/<size>.bin on the SD card -- so a fresh/blank card
// can be provisioned without a PC. Runs on its own core-1 task (like the firmware OTA); the HTTPS
// phase raises fetching() so the UI frees TLS RAM (draw-buffer shrink), same as the OTA path.
namespace EmojiPack {
  void start();                        // spawn the download task (no-op if already running)
  void cancel();                       // request abort between bundles
  bool busy();                         // a download task is running
  bool fetching();                     // true during the run (UI shrinks the LVGL draw buffers for TLS)
  int  progress();                     // bundles completed, 0..EMOJI_SIZE_COUNT
  void status(char* out, size_t cap);  // human-readable status for the Update screen
}
