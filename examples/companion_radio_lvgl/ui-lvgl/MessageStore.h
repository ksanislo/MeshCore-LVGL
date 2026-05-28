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

struct ChatMessage {
  char     peer[CHAT_PEER_NAME_MAX];   // conversation key (contact or channel name)
  char     sender[CHAT_PEER_NAME_MAX]; // who sent it (for channel display / grouping)
  char     text[CHAT_MSG_TEXT_MAX];
  uint32_t timestamp;  // RTC seconds at store time
  bool     outgoing;   // true = sent by us
};

class MessageStore {
public:
  virtual ~MessageStore() {}

  virtual void append(bool outgoing, const char* peer, const char* sender,
                      const char* text, uint32_t ts) = 0;

  // Fill `out` with pointers to this peer's messages, oldest-first, up to `max`.
  // Returns the number written. Pointers stay valid until the next append.
  virtual int messagesFor(const char* peer, const ChatMessage** out, int max) = 0;
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
              const char* text, uint32_t ts) override {
    ChatMessage& m = _buf[_head];
    m.outgoing = outgoing;
    m.timestamp = ts;
    copyBounded(m.peer, peer, CHAT_PEER_NAME_MAX);
    copyBounded(m.sender, sender, CHAT_PEER_NAME_MAX);
    copyBounded(m.text, text, CHAT_MSG_TEXT_MAX);
    _head = (_head + 1) % CAP;
    if (_count < CAP) _count++;
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
