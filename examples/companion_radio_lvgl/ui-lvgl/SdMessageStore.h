#pragma once

#include "MessageStore.h"
#include <Arduino.h>
#include "SdCard.h"   // shared mount + RAII bus lock (pulls in <SdFat.h> + `sd`)

// Persistent message store on the SD card. One consumer of the shared SdCard
// facility (mount + bus lock); others (emoji/map/sound caches) use it too.
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
  bool _write_err;                   // a persist write failed (e.g. card full)
  bool _dir_ok;                      // /chat exists on the currently-mounted card

  // Ensure the card is mounted (via the shared service, which throttles retries
  // and recovers a re-inserted card) and that our /chat dir exists. Resets the
  // loaded conversation after a fresh (re)mount so it reloads from the card.
  bool ensure() {
    if (!SdSvc::ensureMounted()) { _dir_ok = false; return false; }
    if (!_dir_ok) {
      SdSvc::Lock lk;
      if (!sd.exists("/chat")) sd.mkdir("/chat");
      _dir_ok = true;
      _loaded[0] = 0;
    }
    return true;
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
               uint8_t status, uint32_t ack, uint32_t expiry_ms, uint32_t cli = 0,
               uint8_t hops = 0xFF, uint16_t bytes = 0) {
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
    m.cli = cli;
    m.hops = hops;
    m.bytes = bytes;
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
    if (!SdSvc::ready()) return;
    char path[64];
    keyPath(key, path, sizeof(path));
    SdSvc::Lock lk;
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
  }

  // Split the post-sender remainder "text[\thops\tbytes]" -> text + optional metadata.
  // Text is tab-free (copySanitized), so the first tab cleanly ends it; older records
  // (no metadata fields) leave hops/bytes at their unset defaults. In-place: nulls `rest`.
  static void splitTail(char* rest, char** out_txt, uint8_t* out_hops, uint16_t* out_bytes) {
    *out_txt = rest ? rest : (char*)"";
    *out_hops = 0xFF; *out_bytes = 0;
    if (!rest) return;
    char* tab = strchr(rest, '\t');
    if (!tab) return;                         // old record: all remainder is text
    *tab = 0;
    char* hp = tab + 1;
    char* tab2 = strchr(hp, '\t');
    if (tab2) { *tab2 = 0; *out_bytes = (uint16_t)atoi(tab2 + 1); }
    *out_hops = (uint8_t)atoi(hp);
  }

  // Parse one record line into the RAM buffer.
  void parseInto(char* line) {
    char* save = nullptr;
    char* f_ts  = strtok_r(line, "\t", &save);
    char* f_dir = strtok_r(nullptr, "\t", &save);
    char* f_st  = strtok_r(nullptr, "\t", &save);
    char* f_snd = strtok_r(nullptr, "\t", &save);
    if (!f_ts || !f_dir || !f_st || !f_snd) return;
    char* f_txt; uint8_t hops; uint16_t bytes;
    splitTail(save, &f_txt, &hops, &bytes);  // remainder = text (+ optional hops/bytes)
    uint8_t status = (uint8_t)atoi(f_st);
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;  // not in-flight across reboot
    pushBuf(atoi(f_dir) != 0, f_snd, f_txt, (uint32_t)strtoul(f_ts, nullptr, 10), status, 0, 0, 0, hops, bytes);
  }

public:
  SdMessageStore() : _count(0), _write_err(false), _dir_ok(false) { _loaded[0] = 0; }

  // Returns (and clears) whether a persist write has failed since last checked.
  bool takeWriteError() { bool e = _write_err; _write_err = false; return e; }

  // Mount the card + ensure /chat exists. Call once after radio_init.
  bool begin() { SdSvc::begin(); _dir_ok = false; return ensure(); }
  bool ready() const { return SdSvc::ready(); }

  // Preload the newest ~cap messages across all conversations into `dst` (the RAM
  // ring), so disabling/ejecting the card keeps recent history on screen. Reads
  // only each file's tail. Appends oldest-first so the ring keeps the newest set.
  void preloadRecent(MessageStore* dst, int cap) {
    if (!SdSvc::ready() || !dst || cap <= 0) return;
    if (cap > CAP) cap = CAP;
    ChatMessage* recent = (ChatMessage*)malloc(sizeof(ChatMessage) * cap);
    if (!recent) return;
    int rn = 0;  // recent[0..rn) sorted ascending by timestamp

    auto consider = [&](const ChatMessage& m) {
      if (rn == cap) {
        if (m.timestamp <= recent[0].timestamp) return;  // not newer than what we keep
        for (int i = 1; i < cap; i++) recent[i - 1] = recent[i];
        rn = cap - 1;
      }
      int i = rn;
      while (i > 0 && recent[i - 1].timestamp > m.timestamp) { recent[i] = recent[i - 1]; i--; }
      recent[i] = m;
      rn++;
    };

    {
    SdSvc::Lock lk;
    FsFile dir = sd.open("/chat", O_RDONLY);
    FsFile f;
    char name[80];
    char line[CHAT_MSG_TEXT_MAX + CHAT_PEER_NAME_MAX + 32];
    while (dir.isOpen() && f.openNext(&dir, O_RDONLY)) {
      if (f.isDir()) { f.close(); continue; }
      f.getName(name, sizeof(name));
      char key[CHAT_PEER_NAME_MAX];
      copyBounded(key, name, CHAT_PEER_NAME_MAX);
      char* dot = strrchr(key, '.');
      if (dot) *dot = 0;                       // strip ".log" -> conversation key
      uint32_t sz = (uint32_t)f.size();
      uint32_t tail = (uint32_t)cap * 96;      // enough bytes for ~cap records
      if (sz > tail) { f.seek(sz - tail); f.readBytesUntil('\n', line, sizeof(line) - 1); }  // skip partial
      while (f.available()) {
        int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
        if (len <= 0) continue;
        line[len] = 0;
        char* save = nullptr;
        char* f_ts  = strtok_r(line, "\t", &save);
        char* f_dir = strtok_r(nullptr, "\t", &save);
        char* f_st  = strtok_r(nullptr, "\t", &save);
        char* f_snd = strtok_r(nullptr, "\t", &save);
        if (!f_ts || !f_dir || !f_st || !f_snd) continue;
        char* f_txt; uint8_t hops; uint16_t bytes;
        splitTail(save, &f_txt, &hops, &bytes);   // text (+ optional hops/bytes)
        uint32_t ts = (uint32_t)strtoul(f_ts, nullptr, 10);
        if (!ts) continue;
        if (rn == cap && ts <= recent[0].timestamp) continue;
        ChatMessage m;
        m.timestamp = ts;
        m.outgoing = (atoi(f_dir) != 0);
        m.status = MSG_STATUS_NONE;
        m.ack = 0;
        m.expiry_ms = 0;
        m.cli = 0;
        m.hops = hops; m.bytes = bytes;
        copyBounded(m.peer, key, CHAT_PEER_NAME_MAX);
        copyBounded(m.sender, f_snd, CHAT_PEER_NAME_MAX);
        copyBounded(m.text, f_txt, CHAT_MSG_TEXT_MAX);
        consider(m);
      }
      f.close();
    }
    if (dir.isOpen()) dir.close();
    }  // release the bus before appending into the RAM ring

    for (int i = 0; i < rn; i++)
      dst->append(recent[i].outgoing, recent[i].peer, recent[i].sender, recent[i].text, recent[i].timestamp,
                  MSG_STATUS_NONE, 0, 0, 0, recent[i].hops, recent[i].bytes);
    free(recent);
  }

  // Unmount the shared card (e.g. when the user disables history).
  void end() {
    SdSvc::end();
    _dir_ok = false;
    _loaded[0] = 0;
  }

  void append(bool outgoing, const char* peer, const char* sender,
              const char* text, uint32_t ts,
              uint8_t status = MSG_STATUS_NONE, uint32_t ack = 0, uint32_t expiry_ms = 0,
              uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0) override {
    if (ensure()) {
      // Build the whole record, then write it in one go and verify every byte
      // landed -- a full card silently short-writes, so check the count.
      char rec[CHAT_MSG_TEXT_MAX + CHAT_PEER_NAME_MAX + 64];
      int o = snprintf(rec, sizeof(rec), "%lu\t%d\t%u\t",
                       (unsigned long)ts, outgoing ? 1 : 0, (unsigned)status);
      o += copySanitized(rec + o, sizeof(rec) - o, sender);
      if (o < (int)sizeof(rec) - 1) rec[o++] = '\t';
      o += copySanitized(rec + o, sizeof(rec) - o, text);
      // Trailing hops/bytes so the chat metadata footer survives a reload (text is tab-free,
      // so these stay cleanly separable; older records without them default to unset on read).
      o += snprintf(rec + o, sizeof(rec) - o, "\t%u\t%u\n", (unsigned)hops, (unsigned)bytes);

      char path[64];
      keyPath(peer, path, sizeof(path));
      bool opened = false, wrote = false;
      {
        SdSvc::Lock lk;
        FsFile f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
        opened = f.isOpen();
        if (opened) {
          size_t n = f.write((const uint8_t*)rec, o);
          f.close();
          wrote = ((int)n == o);
        }
      }
      if (!wrote) {
        _write_err = true;            // surfaced to the UI
        if (!opened) end();           // couldn't open at all -> card gone; remount next access
      }
    }
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0)
      pushBuf(outgoing, sender, text, ts, status, ack, expiry_ms, cli, hops, bytes);
  }

  int messagesFor(const char* peer, const ChatMessage** out, int max) override {
    ensure();  // recover a re-inserted card before loading
    if (strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) != 0) loadConversation(peer);
    int n = (_count < max) ? _count : max;
    for (int i = 0; i < n; i++) out[i] = &_buf[_count - n + i];
    return n;
  }

  void clearPeer(const char* peer) override {
    if (SdSvc::ready()) {
      char path[64];
      keyPath(peer, path, sizeof(path));
      SdSvc::Lock lk;
      if (sd.exists(path)) sd.remove(path);
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

  bool setByClientToken(uint32_t cli, uint8_t status, uint32_t ack, uint32_t expiry_ms) override {
    if (!cli) return false;
    for (int k = 0; k < _count; k++) {
      int i = _count - 1 - k;
      if (_buf[i].cli == cli && _buf[i].status == MSG_STATUS_SENDING) {
        _buf[i].status = status;
        _buf[i].ack = ack;
        _buf[i].expiry_ms = expiry_ms;
        _buf[i].cli = 0;   // resolved
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
    if (!SdSvc::ready()) return 0;
    char path[64];
    keyPath(peer, path, sizeof(path));
    uint32_t ts = 0;
    SdSvc::Lock lk;
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
    return ts;
  }

  bool requeue(const ChatMessage* m, uint32_t ack, uint32_t expiry_ms, uint32_t cli = 0) override {
    for (int i = 0; i < _count; i++)
      if (&_buf[i] == m) {
        _buf[i].status = MSG_STATUS_SENDING;
        _buf[i].ack = ack;
        _buf[i].expiry_ms = expiry_ms;
        _buf[i].cli = cli;
        return true;
      }
    return false;
  }
};
