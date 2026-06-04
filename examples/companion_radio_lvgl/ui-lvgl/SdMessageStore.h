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

  // ---- Fixed-width record log format (v1, 512-byte blocks) ---------------------------------
  // /chat/<key>.log is an array of fixed 512-byte blocks of printable text. Block 0 is a header
  // ("#MCHAT v1 512 ..."); block N>=1 is record N-1. So offset = 512*N always lands on the header
  // or a record start, count = (size-512)/512, any message is O(1) random-access -- no delimiters,
  // no escaping, no line scanning. Fields live at FIXED byte columns. Each record has ONE '\n'
  // (the terminator at 511) so it is exactly one line for awk/grep/sed even when the message is
  // multi-line: newlines in the text column are stored as a 0x1F sentinel and restored on read.
  //
  // The 512B record is two meaningful 256B halves:
  //   Half 1 (0..255)   CHAT     -- everything to render a bubble; chat scrollback reads only this.
  //   Half 2 (256..510) RADIO    -- reception/diagnostic fields (rssi/snr/path/positions/hashes...),
  //                                 reserved + space-filled for now; populated when reception
  //                                 logging is wired. byte 255 is a space (no mid-record newline).
  static const int REC = 512;
  // Half 1 (chat)
  static const int RC_TS_OFF=0,   RC_TS_W=10;   // decimal unix ts
  static const int RC_DIR_OFF=11;               // '0'/'1' outgoing
  static const int RC_ST_OFF=13;                // delivery status digit
  static const int RC_SEEN_OFF=15;              // '0'/'1' seen (reserved; unread feature wires it)
  static const int RC_RSSI_OFF=17,  RC_RSSI_W=4;   // last RSSI dBm (signed decimal), incoming
  static const int RC_SNR_OFF=22,   RC_SNR_W=4;    // SNR*4 (signed decimal), incoming
  static const int RC_PMETA_OFF=27, RC_PMETA_W=2;  // path_len byte as 2 hex (count|size); blank=direct
  static const int RC_SND_OFF=30,   RC_SND_W=CHAT_PEER_NAME_MAX;   // 32  -> 30..61
  static const int RC_TXT_OFF=63,   RC_TXT_W=192;                  // 63..254 (192 col; buffer caps at CHAT_MSG_TEXT_MAX)
  // Half 2 (radio/diagnostic) -- offsets MUST match the offline converter (convert_chatlog.py).
  static const int RC_HDR_OFF=256,  RC_HDR_W=2;     // pkt header byte, 2 hex
  static const int RC_HASH_OFF=259, RC_HASH_W=16;   // packet hash, 16 hex = 8 bytes
  static const int RC_ACK_OFF=276,  RC_ACK_W=8;     // outgoing expected-ack, 8 hex = 4 bytes
  static const int RC_NOISE_OFF=285,RC_NOISE_W=4;   // noise floor (signed decimal)
  static const int RC_SCORE_OFF=290,RC_SCORE_W=6;   // reserved (blank for now)
  static const int RC_FERR_OFF=297, RC_FERR_W=6;    // reserved (blank for now)
  static const int RC_HPATH_OFF=304,RC_HPATH_W=128; // path hashes, 128 hex = 64 bytes
  static const int RC_OLAT_OFF=433, RC_OLAT_W=11;   // our lat  "-122.123456"
  static const int RC_OLON_OFF=445, RC_OLON_W=11;   // our lon
  static const int RC_RTS_OFF=457,  RC_RTS_W=10;    // remote claimed ts (decimal)
  static const int RC_RLAT_OFF=468, RC_RLAT_W=11;   // remote lat
  static const int RC_RLON_OFF=480, RC_RLON_W=11;   // remote lon
  static const int RC_RESV_OFF=492, RC_RESV_W=19;   // reserved (492..510), '\n' at 511
  static const char RC_NL = 0x1F;               // newline sentinel (US) stored for '\n'; restored on read
  static constexpr const char* RC_MAGIC = "#MCHAT";

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
  // Text column: store newline-safe (0x0A -> 0x1F) so each record stays one line; restore on read.
  static void rcPutText(char* rec, const char* s) {
    int i = 0;
    for (; s && s[i] && i < RC_TXT_W; i++) { char c = s[i]; rec[RC_TXT_OFF + i] = (c == '\n') ? RC_NL : c; }
    for (; i < RC_TXT_W; i++) rec[RC_TXT_OFF + i] = ' ';
  }
  static void rcGetText(const char* rec, char* dst, int dcap) {
    int end = RC_TXT_W; while (end > 0 && rec[RC_TXT_OFF + end - 1] == ' ') end--;
    int n = (end < dcap - 1) ? end : dcap - 1;
    for (int i = 0; i < n; i++) { char c = rec[RC_TXT_OFF + i]; dst[i] = (c == RC_NL) ? '\n' : c; }
    dst[n] = 0;
  }
  static int rcHexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }
  // path_meta: the packet's path_len byte (low 6 bits = hop count, high 2 bits = hash_size-1),
  // stored as 2 hex. Direct/unknown (hops 0xFF) -> blank. bytes is the per-hop hash size (1..4).
  static void rcPutPath(char* rec, uint8_t hops, uint16_t bytes) {
    if (hops == 0xFF) { rec[RC_PMETA_OFF] = ' '; rec[RC_PMETA_OFF + 1] = ' '; return; }
    uint8_t sz = (bytes >= 1 && bytes <= 4) ? (uint8_t)bytes : 1;
    uint8_t plen = (uint8_t)((((sz - 1) & 0x3) << 6) | (hops & 0x3F));
    static const char* H = "0123456789ABCDEF";
    rec[RC_PMETA_OFF]     = H[(plen >> 4) & 0xF];
    rec[RC_PMETA_OFF + 1] = H[plen & 0xF];
  }
  static void rcGetPath(const char* rec, uint8_t* hops, uint16_t* bytes) {
    int hi = rcHexNibble(rec[RC_PMETA_OFF]), lo = rcHexNibble(rec[RC_PMETA_OFF + 1]);
    if (hi < 0 || lo < 0) { *hops = 0xFF; *bytes = 0; return; }   // blank/invalid -> direct
    uint8_t plen = (uint8_t)((hi << 4) | lo);
    *hops  = (uint8_t)(plen & 0x3F);
    *bytes = (uint16_t)((plen >> 6) + 1);
  }
  // --- radio-half field writers ---
  static void rcPutHexBytes(char* rec, int off, const uint8_t* b, int nbytes) {
    static const char* H = "0123456789ABCDEF";
    for (int i = 0; i < nbytes; i++) { rec[off + 2*i] = H[(b[i] >> 4) & 0xF]; rec[off + 2*i + 1] = H[b[i] & 0xF]; }
  }
  static void rcPutSigned(char* rec, int off, int w, int32_t v) {   // signed decimal, right-justified
    char tmp[16]; int n = snprintf(tmp, sizeof(tmp), "%ld", (long)v);
    if (n > w) n = w;
    int pad = w - n;
    for (int i = 0; i < pad; i++) rec[off + i] = ' ';
    for (int i = 0; i < n; i++)   rec[off + pad + i] = tmp[i];
  }
  static void rcPutLatLon(char* rec, int off, int w, int32_t e6) {   // "-122.123456"; 0 -> blank
    if (e6 == 0) { for (int i = 0; i < w; i++) rec[off + i] = ' '; return; }
    int32_t a = e6 < 0 ? -e6 : e6;
    char tmp[20];
    snprintf(tmp, sizeof(tmp), "%s%ld.%06ld", e6 < 0 ? "-" : "", (long)(a / 1000000), (long)(a % 1000000));
    rcPutStr(rec, off, w, tmp);   // left-justify, space-pad, truncate to w
  }
  // Write the radio/diagnostic half from captured meta (everything not set stays the memset space).
  static void rcFormatRadio(char* rec, const RxMeta& m) {
    rcPutLatLon(rec, RC_OLAT_OFF, RC_OLAT_W, m.our_lat);   // our position, both directions
    rcPutLatLon(rec, RC_OLON_OFF, RC_OLON_W, m.our_lon);
    if (m.outgoing) {
      if (m.out_ack) {
        uint8_t ab[4] = { (uint8_t)(m.out_ack >> 24), (uint8_t)(m.out_ack >> 16),
                          (uint8_t)(m.out_ack >> 8),  (uint8_t)m.out_ack };
        rcPutHexBytes(rec, RC_ACK_OFF, ab, 4);
      }
    } else if (m.has_rx) {
      rcPutSigned(rec, RC_RSSI_OFF, RC_RSSI_W, (int32_t)m.rssi);   // chat-half signal slots
      rcPutSigned(rec, RC_SNR_OFF,  RC_SNR_W,  (int32_t)m.snr);
      rcPutHexBytes(rec, RC_HDR_OFF,  &m.header, 1);
      rcPutHexBytes(rec, RC_HASH_OFF, m.pkt_hash, 8);
      rcPutSigned(rec, RC_NOISE_OFF, RC_NOISE_W, (int32_t)m.noise);
      if (m.path_len) rcPutHexBytes(rec, RC_HPATH_OFF, m.path, m.path_len <= 64 ? m.path_len : 64);
      rcPutNum(rec, RC_RTS_OFF, RC_RTS_W, m.sender_ts);
      rcPutLatLon(rec, RC_RLAT_OFF, RC_RLAT_W, m.remote_lat);
      rcPutLatLon(rec, RC_RLON_OFF, RC_RLON_W, m.remote_lon);
    }
  }
  // Build one 512-byte record block. The radio half is space-filled unless `meta` is supplied.
  static void rcFormat(char* rec, bool outgoing, uint8_t status, uint8_t hops, uint16_t bytes,
                       uint32_t ts, const char* sender, const char* text, const RxMeta* meta = nullptr) {
    memset(rec, ' ', REC);
    rcPutNum(rec, RC_TS_OFF, RC_TS_W, ts);
    rec[RC_DIR_OFF]  = outgoing ? '1' : '0';
    rec[RC_ST_OFF]   = (char)('0' + (status % 10));
    rec[RC_SEEN_OFF] = '0';                 // new record: unseen (reserved column)
    rcPutPath(rec, hops, bytes);
    rcPutStr(rec, RC_SND_OFF, RC_SND_W, sender);
    rcPutText(rec, text);
    if (meta) rcFormatRadio(rec, *meta);
    rec[REC - 1] = '\n';
  }
  // Parse one 512-byte record block into the RAM ring (status SENDING -> NONE: not in-flight).
  void rcParseInto(const char* rec) {
    uint32_t ts = rcGetNum(rec, RC_TS_OFF, RC_TS_W);
    bool outgoing = rec[RC_DIR_OFF] == '1';
    char sc = rec[RC_ST_OFF];
    uint8_t status = (sc >= '0' && sc <= '9') ? (uint8_t)(sc - '0') : MSG_STATUS_NONE;
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;
    uint8_t hops; uint16_t bytes; rcGetPath(rec, &hops, &bytes);
    char sender[CHAT_PEER_NAME_MAX]; rcGetStr(rec, RC_SND_OFF, RC_SND_W, sender, sizeof(sender));
    char text[CHAT_MSG_TEXT_MAX];    rcGetText(rec, text, sizeof(text));
    pushBuf(outgoing, sender, text, ts, status, 0, 0, 0, hops, bytes);
  }
  // Parse one record block straight into a caller's ChatMessage (random-access paging; does NOT
  // touch the live _buf). RAM-only fields default off; persisted SENDING collapses to NONE.
  static void rcToMsg(const char* rec, const char* peerkey, ChatMessage& m) {
    char sc = rec[RC_ST_OFF];
    uint8_t status = (sc >= '0' && sc <= '9') ? (uint8_t)(sc - '0') : MSG_STATUS_NONE;
    if (status == MSG_STATUS_SENDING) status = MSG_STATUS_NONE;
    m.outgoing  = rec[RC_DIR_OFF] == '1';
    m.timestamp = rcGetNum(rec, RC_TS_OFF, RC_TS_W);
    m.status    = status;
    m.ack = 0; m.expiry_ms = 0; m.cli = 0;
    rcGetPath(rec, &m.hops, &m.bytes);
    copyBounded(m.peer, peerkey, CHAT_PEER_NAME_MAX);
    rcGetStr(rec, RC_SND_OFF, RC_SND_W, m.sender, CHAT_PEER_NAME_MAX);
    rcGetText(rec, m.text, CHAT_MSG_TEXT_MAX);
  }
  static bool rcFileIsNew(FsFile& f) {   // header magic present?
    char m[6]; f.seek(0);
    return f.read((uint8_t*)m, 6) == 6 && memcmp(m, RC_MAGIC, 6) == 0;
  }
  static void rcMakeHeader(char* hdr) {
    memset(hdr, ' ', REC);
    memcpy(hdr, RC_MAGIC, 6);
    rcPutStr(hdr, 7, 4, "v1");           // version label stays v1; the "512" distinguishes the size
    rcPutStr(hdr, 12, 4, "512");
    // header free space [16..] reserved for seen_floor / unread_count (wired by the unread feature)
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
              uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0,
              const RxMeta* meta = nullptr) override {
    if (ensure()) {
      static char rec[REC];
      rcFormat(rec, outgoing, status, hops, bytes, ts, sender, text, meta);  // one fixed 512B block
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
