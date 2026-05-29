#pragma once

#include "MessageStore.h"
#include <Arduino.h>
#include <SdFat.h>

// Shared SD filesystem + bus guard, provided by the variant (the SD card shares
// the LoRa HSPI bus, re-pinned per access). See variants/.../target.cpp.
extern SdFs sd;
extern void sd_bus_to_sd();
extern void sd_bus_to_lora();
extern bool sd_card_begin();

// Persistent message store on the SD card.
//
// Each conversation is its own append-only log at /chat/<key>.log, where <key>
// is a stable id chosen by the caller (contact pubkey prefix, or channel secret
// prefix) -- never a display name, so renames/nicknames don't orphan history.
//
// Records are one line each, tab-separated:  <ts>\t<dir>\t<status>\t<sender>\t<text>
// (tabs/newlines in sender/text are replaced with spaces on write).
//
// A RAM buffer holds the *active* conversation (the one last asked for via
// messagesFor) so display + live delivery-status work without re-reading SD, and
// so the existing pointer-returning interface still holds. Delivery status is
// ephemeral: persisted SENDING records load back as NONE (can't be in-flight
// across a reboot).
template <int CAP>
class SdMessageStore : public MessageStore {
  ChatMessage _buf[CAP];
  int  _count;
  char _loaded[CHAT_PEER_NAME_MAX];  // conversation key currently in _buf
  bool _ok;                          // SD mounted
  bool _write_err;                   // a persist write failed (e.g. card full)
  uint32_t _retry_ms;                // last (re)mount attempt time

  // Mount if needed. No card-detect pin on this board, so we (re)mount lazily on
  // access -- recovering a re-inserted card -- but throttle attempts so a missing
  // card doesn't thrash SD.begin() (and the shared LoRa bus) every message.
  bool ensureMounted() {
    if (_ok) return true;
    uint32_t now = millis();
    if (_retry_ms != 0 && (uint32_t)(now - _retry_ms) < 3000) return false;
    _retry_ms = now ? now : 1;
    _ok = sd_card_begin();
    if (_ok) {
      sd_bus_to_sd();
      if (!sd.exists("/chat")) sd.mkdir("/chat");
      sd_bus_to_lora();
      _loaded[0] = 0;   // force reload of the active conversation from the card
    }
    return _ok;
  }

  static void keyPath(const char* key, char* out, size_t cap) {
    int o = snprintf(out, cap, "/chat/");
    for (const char* p = key; *p && o < (int)cap - 5; p++) {
      char c = *p;
      bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
      out[o++] = safe ? c : '_';
    }
    snprintf(out + o, cap - o, ".log");
  }

  // Copy src into dst (bounded), turning tabs/newlines into spaces (field-safe).
  static int copySanitized(char* dst, int cap, const char* src) {
    int o = 0;
    for (const char* p = src ? src : ""; *p && o < cap - 1; p++)
      dst[o++] = (*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p;
    return o;
  }

  // Append into the linear RAM buffer (drops the oldest when full).
  void pushBuf(bool outgoing, const char* sender, const char* text, uint32_t ts,
               uint8_t status, uint32_t ack, uint32_t expiry_ms) {
    if (_count == CAP) {
      for (int i = 1; i < CAP; i++) _buf[i - 1] = _buf[i];
      _count--;
    }
    ChatMessage& m = _buf[_count++];
    m.outgoing = outgoing;
    m.timestamp = ts;
    m.status = status;
    m.ack = ack;
    m.expiry_ms = expiry_ms;
    copyBounded(m.peer, _loaded, CHAT_PEER_NAME_MAX);
    copyBounded(m.sender, sender, CHAT_PEER_NAME_MAX);
    copyBounded(m.text, text, CHAT_MSG_TEXT_MAX);
  }

  static void copyBounded(char* dst, const char* src, size_t cap) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
  }

  void loadConversation(const char* key) {
    _count = 0;
    copyBounded(_loaded, key, CHAT_PEER_NAME_MAX);
    if (!_ok) return;
    char path[64];
    keyPath(key, path, sizeof(path));
    sd_bus_to_sd();
    FsFile f = sd.open(path, O_RDONLY);
    if (f.isOpen()) {
      char line[CHAT_MSG_TEXT_MAX + CHAT_PEER_NAME_MAX + 32];
      while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len <= 0) continue;
        line[len] = 0;
        parseInto(line);
      }
      f.close();
    }
    sd_bus_to_lora();
  }

  // Parse one record line into the RAM buffer.
  void parseInto(char* line) {
    char* save = nullptr;
    char* f_ts  = strtok_r(line, "\t", &save);
    char* f_dir = strtok_r(nullptr, "\t", &save);
    char* f_st  = strtok_r(nullptr, "\t", &save);
    char* f_snd = strtok_r(nullptr, "\t", &save);
    char* f_txt = save ? save : (char*)"";   // remainder = text (may contain nothing)
    if (!f_ts || !f_dir || !f_st || !f_snd) return;
    uint8_t status = (uint8_t)atoi(f_st);
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;  // not in-flight across reboot
    pushBuf(atoi(f_dir) != 0, f_snd, f_txt, (uint32_t)strtoul(f_ts, nullptr, 10), status, 0, 0);
  }

public:
  SdMessageStore() : _count(0), _ok(false), _write_err(false), _retry_ms(0) { _loaded[0] = 0; }

  // Returns (and clears) whether a persist write has failed since last checked.
  bool takeWriteError() { bool e = _write_err; _write_err = false; return e; }

  // Mount the card + ensure /chat exists. Call once after radio_init.
  bool begin() { _ok = false; _retry_ms = 0; return ensureMounted(); }
  bool ready() const { return _ok; }

  void append(bool outgoing, const char* peer, const char* sender,
              const char* text, uint32_t ts,
              uint8_t status = MSG_STATUS_NONE, uint32_t ack = 0, uint32_t expiry_ms = 0) override {
    if (ensureMounted()) {
      // Build the whole record, then write it in one go and verify every byte
      // landed -- a full card silently short-writes, so check the count.
      char rec[CHAT_MSG_TEXT_MAX + CHAT_PEER_NAME_MAX + 48];
      int o = snprintf(rec, sizeof(rec), "%lu\t%d\t%u\t",
                       (unsigned long)ts, outgoing ? 1 : 0, (unsigned)status);
      o += copySanitized(rec + o, sizeof(rec) - o, sender);
      if (o < (int)sizeof(rec) - 1) rec[o++] = '\t';
      o += copySanitized(rec + o, sizeof(rec) - o, text);
      if (o < (int)sizeof(rec) - 1) rec[o++] = '\n';

      char path[64];
      keyPath(peer, path, sizeof(path));
      sd_bus_to_sd();
      FsFile f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
      bool opened = f.isOpen();
      bool wrote = false;
      if (opened) {
        size_t n = f.write((const uint8_t*)rec, o);
        f.close();
        wrote = ((int)n == o);
      }
      sd_bus_to_lora();
      if (!wrote) {
        _write_err = true;            // surfaced to the UI
        if (!opened) _ok = false;     // couldn't open at all -> card gone; remount next access
      }
    }
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0)
      pushBuf(outgoing, sender, text, ts, status, ack, expiry_ms);
  }

  int messagesFor(const char* peer, const ChatMessage** out, int max) override {
    ensureMounted();  // recover a re-inserted card before loading
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) != 0) loadConversation(peer);
    int n = (_count < max) ? _count : max;
    for (int i = 0; i < n; i++) out[i] = &_buf[_count - n + i];
    return n;
  }

  void clearPeer(const char* peer) override {
    if (_ok) {
      char path[64];
      keyPath(peer, path, sizeof(path));
      sd_bus_to_sd();
      if (sd.exists(path)) sd.remove(path);
      sd_bus_to_lora();
    }
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0) _count = 0;
  }

  bool setStatusByAck(uint32_t ack, uint8_t status) override {
    if (!ack) return false;
    for (int k = 0; k < _count; k++) {
      int i = _count - 1 - k;
      if (_buf[i].ack == ack && _buf[i].status == MSG_STATUS_SENDING) {
        _buf[i].status = status;
        return true;
      }
    }
    return false;
  }

  bool expireSending(uint32_t now_ms, uint8_t fail_status) override {
    bool changed = false;
    for (int i = 0; i < _count; i++)
      if (_buf[i].status == MSG_STATUS_SENDING && _buf[i].expiry_ms &&
          (int32_t)(now_ms - _buf[i].expiry_ms) >= 0) {
        _buf[i].status = fail_status;
        changed = true;
      }
    return changed;
  }

  uint32_t latestTimestampFor(const char* peer) override {
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0)
      return _count > 0 ? _buf[_count - 1].timestamp : 0;
    if (!_ok) return 0;
    char path[64];
    keyPath(peer, path, sizeof(path));
    uint32_t ts = 0;
    sd_bus_to_sd();
    FsFile f = sd.open(path, O_RDONLY);
    if (f.isOpen()) {
      uint32_t sz = (uint32_t)f.size();
      uint32_t start = sz > 256 ? sz - 256 : 0;   // last record only -- cheap
      f.seek(start);
      char buf[260];
      int n = f.read((uint8_t*)buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = 0;
        char* p = buf + n - 1;
        while (p > buf && (*p == '\n' || *p == '\r')) *p-- = 0;  // strip trailing EOL
        char* nl = strrchr(buf, '\n');
        ts = (uint32_t)strtoul(nl ? nl + 1 : buf, nullptr, 10);
      }
      f.close();
    }
    sd_bus_to_lora();
    return ts;
  }

  bool requeue(const ChatMessage* m, uint32_t ack, uint32_t expiry_ms) override {
    for (int i = 0; i < _count; i++)
      if (&_buf[i] == m) {
        _buf[i].status = MSG_STATUS_SENDING;
        _buf[i].ack = ack;
        _buf[i].expiry_ms = expiry_ms;
        return true;
      }
    return false;
  }
};
