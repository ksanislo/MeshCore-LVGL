#pragma once

#include <stdint.h>
#include <string.h>

// On-device chat history. Abstract interface so the RAM ring buffer used today
// can be swapped for an SD-card-backed store later (see project design goals)
// without touching the UI. Messages are keyed by peer *name* for now (that's
// what MyMesh::newMsg hands the UI); a future SD backend can re-key by pubkey.

#ifndef CHAT_MSG_TEXT_MAX
  // Buffer size (INCLUDES the null) for a message body. 161 = MAX_TEXT_LEN (160 =
  // 10*CIPHER_BLOCK_SIZE, the wire cap) + 1, so we can hold the longest legal
  // message in full and never byte-truncate (which could slice a UTF-8 codepoint)
  // at ingest. Also the fixed-record text column width (RC_TXT_W) -- widening it is
  // backward-compatible since text is the last column and records are space-filled.
  #define CHAT_MSG_TEXT_MAX 161
#endif
#ifndef CHAT_PEER_NAME_MAX
  #define CHAT_PEER_NAME_MAX 32
#endif

// Delivery status for outgoing direct messages (incoming/channel msgs = NONE).
enum { MSG_STATUS_NONE = 0, MSG_STATUS_SENDING = 1, MSG_STATUS_DELIVERED = 2, MSG_STATUS_FAILED = 3 };

struct ChatMessage {
  char     peer[CHAT_PEER_NAME_MAX];   // conversation key (contact or channel name)
  char     sender[CHAT_PEER_NAME_MAX]; // who sent it (for channel display / grouping)
  char     text[CHAT_MSG_TEXT_MAX];
  uint32_t timestamp;  // RTC seconds at store time
  bool     outgoing;   // true = sent by us
  uint8_t  status;     // MSG_STATUS_* (delivery state for outgoing direct msgs)
  uint32_t ack;        // expected ACK value (to match a delivery confirmation)
  uint32_t expiry_ms;  // millis() deadline after which a SENDING msg is FAILED (0 = none)
  uint32_t cli;        // client send-token: correlates an optimistic outgoing bubble
                       // with its async EV_SEND_RESULT (0 = none). RAM-only, not persisted.
  uint8_t  hops;       // incoming flood path length (0xFF = direct/unknown). RAM-only.
  uint16_t bytes;      // incoming message payload size, for the diagnostic footer. RAM-only.
};

// Disk-only radio/diagnostic metadata for the 512-byte SD record's radio half. Passed by
// pointer to append(); the RAM ring ignores it (keeps ChatMessage lean). Captured at packet
// receive and threaded backend->UI; fields not captured stay at their zero defaults (written
// blank). RX_PATH_MAX mirrors src MAX_PATH_SIZE (64) without pulling in the heavy header.
#ifndef RX_PATH_MAX
  #define RX_PATH_MAX 64
#endif
struct RxMeta {
  bool     outgoing = false;   // mirrors the append() arg (selects which columns to write)
  bool     has_rx = false;     // incoming radio fields valid
  int8_t   snr = 0;            // SNR*4
  int8_t   rssi = 0;           // dBm
  int16_t  noise = 0;
  uint8_t  header = 0;
  uint8_t  pkt_hash[8] = {0};
  uint8_t  path[RX_PATH_MAX] = {0};
  uint8_t  path_len = 0;       // raw byte count in path[]
  uint32_t sender_ts = 0;
  int32_t  our_lat = 0, our_lon = 0;       // deg * 1e6 (0,0 = unknown -> written blank)
  int32_t  remote_lat = 0, remote_lon = 0;
  uint32_t out_ack = 0;        // outgoing only
};

class MessageStore {
public:
  virtual ~MessageStore() {}

  // Backing-store lifecycle/health. Default impls suit a pure-RAM store (always
  // ready, nothing to mount, never fails); a persistent store overrides them.
  virtual bool begin() { return true; }
  virtual bool ready() const { return true; }
  virtual bool takeWriteError() { return false; }

  virtual void append(bool outgoing, const char* peer, const char* sender,
                      const char* text, uint32_t ts,
                      uint8_t status = MSG_STATUS_NONE, uint32_t ack = 0, uint32_t expiry_ms = 0,
                      uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0,
                      const RxMeta* meta = nullptr) = 0;

  // Mark the (most recent) SENDING message with this ack as `status`. Returns true if found.
  virtual bool setStatusByAck(uint32_t ack, uint8_t status) = 0;
  // Resolve an optimistic outgoing bubble by its client token (set the real ack +
  // expiry once the backend confirms the send, or FAILED if it didn't). Finds the
  // most-recent SENDING message with this `cli`. Returns true if found.
  virtual bool setByClientToken(uint32_t cli, uint8_t status, uint32_t ack, uint32_t expiry_ms) = 0;
  // Move any SENDING message past its expiry deadline to `fail_status`. Returns true if any changed.
  virtual bool expireSending(uint32_t now_ms, uint8_t fail_status) = 0;

  // Re-arm an existing (e.g. failed) message in place for a resend: set it back to
  // SENDING with a new ack/expiry/token, no new entry. Returns false if `m` isn't current.
  virtual bool requeue(const ChatMessage* m, uint32_t ack, uint32_t expiry_ms, uint32_t cli = 0) = 0;

  // Fill `out` with pointers to this peer's messages, oldest-first, up to `max`.
  // Returns the number written. Pointers stay valid until the next append.
  virtual int messagesFor(const char* peer, const ChatMessage** out, int max) = 0;

  // Virtualized paging (persistent stores). recordCount = total persisted records for a peer;
  // readRecords copies file records [first, first+max) (oldest-first) into `out`. Default 0 means
  // "no paging available" -- the UI then just uses the messagesFor() window (a RAM ring has no
  // history beyond what it holds).
  virtual int recordCount(const char* /*peer*/) { return 0; }
  virtual int readRecords(const char* /*peer*/, int /*first*/, int /*max*/, ChatMessage* /*out*/) { return 0; }

  // Drop all messages for a peer (clear that conversation's history).
  virtual void clearPeer(const char* peer) = 0;

  // Newest message timestamp for a peer, or 0 if none (for "latest message" sort).
  virtual uint32_t latestTimestampFor(const char* peer) = 0;
};

// Fixed-capacity ring buffer. Oldest message is overwritten when full.
template <int CAP>
class RamMessageStore : public MessageStore {
  ChatMessage _buf[CAP];
  int _count = 0;  // number of valid entries (<= CAP)
  int _head  = 0;  // index of next write

  static void copyBounded(char* dst, const char* src, size_t cap) {
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
  }

public:
  void append(bool outgoing, const char* peer, const char* sender,
              const char* text, uint32_t ts,
              uint8_t status = MSG_STATUS_NONE, uint32_t ack = 0, uint32_t expiry_ms = 0,
              uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0,
              const RxMeta* meta = nullptr) override {
    (void)meta;   // RAM ring is lean -- radio metadata is disk-only
    ChatMessage& m = _buf[_head];
    m.outgoing = outgoing;
    m.timestamp = ts;
    m.status = status;
    m.ack = ack;
    m.expiry_ms = expiry_ms;
    m.cli = cli;
    m.hops = hops;
    m.bytes = bytes;
    copyBounded(m.peer, peer, CHAT_PEER_NAME_MAX);
    copyBounded(m.sender, sender, CHAT_PEER_NAME_MAX);
    copyBounded(m.text, text, CHAT_MSG_TEXT_MAX);
    _head = (_head + 1) % CAP;
    if (_count < CAP) _count++;
  }

  // Replay buffered messages (oldest-first) with timestamp >= since_ts into dst.
  // Used to backfill the SD store with history that arrived while saving was off.
  void replayInto(MessageStore* dst, uint32_t since_ts) const {
    if (!dst) return;
    for (int k = 0; k < _count; k++) {
      int i = (_head - _count + k + CAP) % CAP;
      if (_buf[i].timestamp >= since_ts)
        dst->append(_buf[i].outgoing, _buf[i].peer, _buf[i].sender, _buf[i].text, _buf[i].timestamp,
                    MSG_STATUS_NONE, 0, 0, 0, _buf[i].hops, _buf[i].bytes);
    }
  }

  bool setStatusByAck(uint32_t ack, uint8_t status) override {
    if (!ack) return false;
    // Newest-first so a recycled ack value updates the most recent send.
    for (int k = 0; k < _count; k++) {
      int i = (_head - 1 - k + CAP) % CAP;
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
      int i = (_head - 1 - k + CAP) % CAP;
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
    for (int i = 0; i < _count; i++) {
      if (_buf[i].status == MSG_STATUS_SENDING && _buf[i].expiry_ms &&
          (int32_t)(now_ms - _buf[i].expiry_ms) >= 0) {
        _buf[i].status = fail_status;
        changed = true;
      }
    }
    return changed;
  }

  bool requeue(const ChatMessage* m, uint32_t ack, uint32_t expiry_ms, uint32_t cli = 0) override {
    for (int i = 0; i < _count; i++) {
      if (&_buf[i] == m) {  // still the same live slot
        _buf[i].status = MSG_STATUS_SENDING;
        _buf[i].ack = ack;
        _buf[i].expiry_ms = expiry_ms;
        _buf[i].cli = cli;
        return true;
      }
    }
    return false;
  }

  void clearPeer(const char* peer) override {
    if (!peer) return;
    for (int i = 0; i < _count; i++) {
      ChatMessage& m = _buf[i];
      if (strncmp(m.peer, peer, CHAT_PEER_NAME_MAX) == 0) m.peer[0] = 0;  // tombstone
    }
  }

  uint32_t latestTimestampFor(const char* peer) override {
    if (!peer) return 0;
    uint32_t latest = 0;
    for (int i = 0; i < _count; i++) {
      const ChatMessage& m = _buf[i];
      if (m.peer[0] && strncmp(m.peer, peer, CHAT_PEER_NAME_MAX) == 0 && m.timestamp > latest)
        latest = m.timestamp;
    }
    return latest;
  }

  int messagesFor(const char* peer, const ChatMessage** out, int max) override {
    if (!peer || max <= 0) return 0;
    // Walk oldest-first. Oldest entry is at (_head - _count) mod CAP.
    int start = (_head - _count + CAP) % CAP;
    int n = 0;
    for (int i = 0; i < _count && n < max; i++) {
      const ChatMessage& m = _buf[(start + i) % CAP];
      if (strncmp(m.peer, peer, CHAT_PEER_NAME_MAX) == 0) out[n++] = &m;
    }
    return n;
  }

  // Virtualized-read support so the chat view works with a RAM-only store (no SD). The ring IS the
  // whole history, so recordCount = the per-peer ring count and readRecords slices it oldest-first.
  int recordCount(const char* peer) override {
    if (!peer) return 0;
    int start = (_head - _count + CAP) % CAP;
    int n = 0;
    for (int i = 0; i < _count; i++)
      if (strncmp(_buf[(start + i) % CAP].peer, peer, CHAT_PEER_NAME_MAX) == 0) n++;
    return n;
  }
  int readRecords(const char* peer, int first, int max, ChatMessage* out) override {
    if (!peer || max <= 0 || first < 0 || !out) return 0;
    int start = (_head - _count + CAP) % CAP;
    int matched = 0, n = 0;
    for (int i = 0; i < _count && n < max; i++) {
      const ChatMessage& m = _buf[(start + i) % CAP];
      if (strncmp(m.peer, peer, CHAT_PEER_NAME_MAX) != 0) continue;
      if (matched >= first) out[n++] = m;
      matched++;
    }
    return n;
  }
};
