#pragma once

#include <stdint.h>
#include <string.h>

// On-device chat history. Abstract interface so the RAM ring buffer used today
// can be swapped for an SD-card-backed store later (see project design goals)
// without touching the UI. Messages are keyed by peer *name* for now (that's
// what MyMesh::newMsg hands the UI); a future SD backend can re-key by pubkey.

#ifndef CHAT_MSG_TEXT_MAX
  #define CHAT_MSG_TEXT_MAX 150
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
                      uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0) = 0;

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
              uint32_t cli = 0, uint8_t hops = 0xFF, uint16_t bytes = 0) override {
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
        dst->append(_buf[i].outgoing, _buf[i].peer, _buf[i].sender, _buf[i].text, _buf[i].timestamp);
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
};
