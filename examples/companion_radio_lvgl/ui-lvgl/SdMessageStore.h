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
  bool _loaded_has_older = false;    // the loaded window started mid-file -> older history exists

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

  // Copy src into dst (bounded), backslash-escaping the structural chars so the line/TSV
  // format stays unambiguous while preserving the original text byte-for-byte (a tab or a
  // multi-line message survives a reload). \t -> "\t", \n -> "\n", \r -> "\r", \ -> "\\".
  static int copyEscaped(char* dst, int cap, const char* src) {
    int o = 0;
    for (const char* p = src ? src : ""; *p && o < cap - 2; p++) {
      char c = *p, e = 0;
      if      (c == '\\') e = '\\';
      else if (c == '\t') e = 't';
      else if (c == '\n') e = 'n';
      else if (c == '\r') e = 'r';
      if (e) { dst[o++] = '\\'; dst[o++] = e; }
      else   { dst[o++] = c; }
    }
    return o;
  }

  // Reverse copyEscaped in place (result is always <= input length). Unknown escapes pass
  // the escaped char through literally. Records written before escaping have no backslashes,
  // so this is a no-op on them (backward compatible).
  static void unescape(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) {
      if (*r == '\\' && r[1]) {
        r++;
        *w++ = (*r == 't') ? '\t' : (*r == 'n') ? '\n' : (*r == 'r') ? '\r' : *r;
      } else *w++ = *r;
    }
    *w = 0;
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

  // ---- Fixed-width record log format -------------------------------------------------------
  // /chat/<key>.log is an array of fixed 256-byte blocks of printable text. Block 0 is a header
  // ("#MCHAT ..."); block N>=1 is record N-1. So offset = 256*N always lands on the header or a
  // record start, count = (size-256)/256, and any message is random-accessible in O(1) -- no
  // delimiters, no escaping, no line scanning. Fields live at FIXED byte columns; the message
  // text is stored RAW (newlines/tabs and all) in its column and right-trimmed of pad spaces on
  // read. Each block ends with '\n' so it still opens cleanly as a text file. Reserved bytes
  // [219,255) leave room for future fixed-position fields without changing offsets. (The text
  // column was widened 150->161 to match the wire cap; old 150-wide records still read correctly
  // since the extra bytes were space pad and text is the last column.)
  static const int REC = 256;
  static const int RC_TS_OFF=0,  RC_TS_W=10;   // decimal unix ts (10 digits = uint32 max)
  static const int RC_DIR_OFF=11;              // '0'/'1' outgoing
  static const int RC_ST_OFF=13;               // status digit
  static const int RC_HOPS_OFF=15, RC_HOPS_W=3;
  static const int RC_BYTES_OFF=19, RC_BYTES_W=5;
  static const int RC_SND_OFF=25, RC_SND_W=CHAT_PEER_NAME_MAX;       // 32
  static const int RC_TXT_OFF=58, RC_TXT_W=CHAT_MSG_TEXT_MAX;        // 161  (-> 219, then reserved)
  static constexpr const char* RC_MAGIC = "#MCHAT";                  // first 6 bytes of block 0

  static void rcPutNum(char* rec, int off, int w, uint32_t v) {
    for (int i = w - 1; i >= 0; i--) { rec[off + i] = (char)('0' + (v % 10)); v /= 10; }
  }
  static uint32_t rcGetNum(const char* rec, int off, int w) {
    uint32_t v = 0;
    for (int i = 0; i < w; i++) { char c = rec[off + i]; if (c >= '0' && c <= '9') v = v * 10 + (uint32_t)(c - '0'); }
    return v;
  }
  static void rcPutStr(char* rec, int off, int w, const char* s) {   // left-justified, space-padded, raw
    int i = 0;
    for (; s && s[i] && i < w; i++) rec[off + i] = s[i];
    for (; i < w; i++) rec[off + i] = ' ';
  }
  static void rcGetStr(const char* rec, int off, int w, char* dst, int dcap) {  // rstrip pad spaces
    int end = w; while (end > 0 && rec[off + end - 1] == ' ') end--;
    int n = (end < dcap - 1) ? end : dcap - 1;
    memcpy(dst, rec + off, (size_t)n); dst[n] = 0;
  }
  // Build one 256-byte record block from fields.
  static void rcFormat(char* rec, bool outgoing, uint8_t status, uint8_t hops, uint16_t bytes,
                       uint32_t ts, const char* sender, const char* text) {
    memset(rec, ' ', REC);
    rcPutNum(rec, RC_TS_OFF, RC_TS_W, ts);
    rec[RC_DIR_OFF] = outgoing ? '1' : '0';
    rec[RC_ST_OFF]  = (char)('0' + (status % 10));
    rcPutNum(rec, RC_HOPS_OFF, RC_HOPS_W, hops);
    rcPutNum(rec, RC_BYTES_OFF, RC_BYTES_W, bytes);
    rcPutStr(rec, RC_SND_OFF, RC_SND_W, sender);
    rcPutStr(rec, RC_TXT_OFF, RC_TXT_W, text);
    rec[REC - 1] = '\n';
  }
  // Parse one 256-byte record block into the RAM ring (status SENDING -> NONE: not in-flight).
  void rcParseInto(const char* rec) {
    uint32_t ts = rcGetNum(rec, RC_TS_OFF, RC_TS_W);
    bool outgoing = rec[RC_DIR_OFF] == '1';
    char sc = rec[RC_ST_OFF];
    uint8_t status = (sc >= '0' && sc <= '9') ? (uint8_t)(sc - '0') : MSG_STATUS_NONE;
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;
    uint8_t  hops  = (uint8_t)rcGetNum(rec, RC_HOPS_OFF, RC_HOPS_W);
    uint16_t bytes = (uint16_t)rcGetNum(rec, RC_BYTES_OFF, RC_BYTES_W);
    char sender[CHAT_PEER_NAME_MAX]; rcGetStr(rec, RC_SND_OFF, RC_SND_W, sender, sizeof(sender));
    char text[CHAT_MSG_TEXT_MAX];    rcGetStr(rec, RC_TXT_OFF, RC_TXT_W, text, sizeof(text));
    pushBuf(outgoing, sender, text, ts, status, 0, 0, 0, hops, bytes);
  }
  // Parse one record block straight into a caller's ChatMessage (for random-access paging reads --
  // does NOT touch the live _buf). RAM-only fields (ack/expiry/cli) default off; a persisted record
  // is historical, so SENDING collapses to NONE just like rcParseInto.
  static void rcToMsg(const char* rec, const char* peerkey, ChatMessage& m) {
    char sc = rec[RC_ST_OFF];
    uint8_t status = (sc >= '0' && sc <= '9') ? (uint8_t)(sc - '0') : MSG_STATUS_NONE;
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;
    m.outgoing  = rec[RC_DIR_OFF] == '1';
    m.timestamp = rcGetNum(rec, RC_TS_OFF, RC_TS_W);
    m.status    = status;
    m.ack = 0; m.expiry_ms = 0; m.cli = 0;
    m.hops  = (uint8_t)rcGetNum(rec, RC_HOPS_OFF, RC_HOPS_W);
    m.bytes = (uint16_t)rcGetNum(rec, RC_BYTES_OFF, RC_BYTES_W);
    copyBounded(m.peer, peerkey, CHAT_PEER_NAME_MAX);
    rcGetStr(rec, RC_SND_OFF, RC_SND_W, m.sender, CHAT_PEER_NAME_MAX);
    rcGetStr(rec, RC_TXT_OFF, RC_TXT_W, m.text, CHAT_MSG_TEXT_MAX);
  }
  static bool rcFileIsNew(FsFile& f) {   // header magic present?
    char m[6]; f.seek(0);
    return f.read((uint8_t*)m, 6) == 6 && memcmp(m, RC_MAGIC, 6) == 0;
  }
  static void rcMakeHeader(char* hdr) {
    memset(hdr, ' ', REC);
    memcpy(hdr, RC_MAGIC, 6);
    rcPutStr(hdr, 7, 4, "v1");
    rcPutStr(hdr, 12, 4, "256");
    hdr[REC - 1] = '\n';
  }


  void loadConversation(const char* key) {
    _count = 0;
    copyBounded(_loaded, key, CHAT_PEER_NAME_MAX);
    _loaded_has_older = false;
    if (!SdSvc::ready()) return;
    char path[64];
    keyPath(key, path, sizeof(path));
    SdSvc::Lock lk;
    FsFile f = sd.open(path, O_RDONLY);
    if (f.isOpen()) {
      uint32_t sz = (uint32_t)f.size();
      if (sz >= (uint32_t)(2 * REC)) {           // header block + >=1 record
        int total = (int)((sz - REC) / REC);     // record count = (size - header)/recsize
        int want  = (total < CAP) ? total : CAP; // newest CAP records
        int first = total - want;
        _loaded_has_older = (first > 0);         // older records exist before the window
        static char block[REC];
        f.seek((uint32_t)REC * (1 + first));     // skip header (block 0) + the `first` older records
        for (int i = 0; i < want; i++) {
          if (f.read((uint8_t*)block, REC) != REC) break;
          rcParseInto(block);
        }
      }
      f.close();
    }
  }

  // Split the post-sender remainder "text[\thops\tbytes]" -> text + optional metadata.
  // The stored text is escaped (no raw tabs), so the first raw tab cleanly ends it; older
  // records (no metadata fields) leave hops/bytes at their unset defaults. In-place: nulls
  // `rest`. The caller unescapes the returned text.
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
    unescape(f_snd); unescape(f_txt);        // restore any tabs/newlines in the original
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
  //
  // WARNING: NOT CALLED at boot anymore. On the CrowPanel's octal PSRAM @ 80MHz, running
  // this over a large mixed set of logs could leave the memory bus such that a later pool
  // read stalls -> watchdog -> boot loop (a marginal HW timing quirk; see the repo's
  // psram-80mhz-stall-repro/). Do NOT re-enable on 80MHz hardware without a fix (e.g. 40MHz
  // PSRAM). Kept here intentionally for that future path; it is not instantiated while
  // unreferenced, so it costs nothing in the binary.
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
    char line[2 * CHAT_MSG_TEXT_MAX + CHAT_PEER_NAME_MAX + 48];  // 2x: escaping can double text
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
        unescape(f_snd); unescape(f_txt);         // restore any tabs/newlines in the original
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
      static char rec[REC];
      rcFormat(rec, outgoing, status, hops, bytes, ts, sender, text);  // one fixed 256B block
      char path[64];
      keyPath(peer, path, sizeof(path));
      bool opened = false, wrote = false;
      {
        SdSvc::Lock lk;
        FsFile f = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
        opened = f.isOpen();
        if (opened) {
          // A brand-new (or just-created) file needs the 256B header block first so offsets line up.
          if (f.size() == 0) { char hdr[REC]; rcMakeHeader(hdr); f.write((const uint8_t*)hdr, REC); }
          size_t n = f.write((const uint8_t*)rec, REC);   // append the record block; verify full write
          f.close();
          wrote = ((int)n == REC);
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

  // Total persisted record count for a conversation (for virtualized paging). From the file when
  // the card is up; else the RAM buffer count for the loaded conversation (no older history then).
  int recordCount(const char* peer) override {
    ensure();
    if (SdSvc::ready()) {
      char path[64]; keyPath(peer, path, sizeof(path));
      SdSvc::Lock lk;
      FsFile f = sd.open(path, O_RDONLY);
      if (f.isOpen()) {
        uint32_t sz = (uint32_t)f.size(); f.close();
        return (sz >= (uint32_t)(2 * REC)) ? (int)((sz - REC) / REC) : 0;
      }
    }
    if (_loaded[0] && strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0) return _count;
    return 0;
  }

  // Random-access read of records [first, first+max) (file order, oldest-first) into `out` as COPIES
  // -- O(1) seek, does NOT disturb the live _buf. Returns the count read. Used by the UI's virtualized
  // scrollback to page older history beyond the resident _buf window. (Persisted status only; the live
  // tail's up-to-date status comes from messagesFor.)
  int readRecords(const char* peer, int first, int max, ChatMessage* out) override {
    if (first < 0 || max <= 0) return 0;
    ensure();
    if (SdSvc::ready()) {
      char path[64]; keyPath(peer, path, sizeof(path));
      SdSvc::Lock lk;
      FsFile f = sd.open(path, O_RDONLY);
      if (f.isOpen()) {
        uint32_t sz = (uint32_t)f.size();
        int total = (sz >= (uint32_t)(2 * REC)) ? (int)((sz - REC) / REC) : 0;
        static char block[REC];
        int got = 0;
        f.seek((uint32_t)REC * (1 + first));     // header is block 0
        for (int i = 0; i < max && (first + i) < total; i++) {
          if (f.read((uint8_t*)block, REC) != REC) break;
          rcToMsg(block, peer, out[got++]);
        }
        f.close();
        return got;
      }
    }
    if (_loaded[0] && strncmp(peer, _loaded, CHAT_PEER_NAME_MAX) == 0) {
      int got = 0;
      for (int i = 0; i < max && (first + i) < _count; i++) out[got++] = _buf[first + i];
      return got;
    }
    return 0;
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
      if (sz >= (uint32_t)(2 * REC) && rcFileIsNew(f)) {
        // New fixed-record format: the last record block is at size-REC -- O(1), no migration.
        static char block[REC];
        f.seek(sz - REC);
        if (f.read((uint8_t*)block, REC) == REC) ts = rcGetNum(block, RC_TS_OFF, RC_TS_W);
      } else if (sz > 0) {
        // Old escaped-text format (not yet migrated): last line's leading ts field.
        uint32_t start = sz > 256 ? sz - 256 : 0;
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
