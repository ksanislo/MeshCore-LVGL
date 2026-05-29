#pragma once

// MeshProxy — the UI's proxy view of the mesh backend, for the dual-core split.
//
// Shared-nothing boundary between the LVGL UI (core 1) and the mesh/radio
// backend (core 0). The UI NEVER calls `the_mesh` directly; instead it:
//   * reads a published, immutable SNAPSHOT of contacts/channels/self/prefs,
//   * posts COMMANDS (sends, edits, settings) the backend executes, and
//   * receives EVENTS (the 5 mesh→UI callbacks) the backend cooks and enqueues.
// The only lock is the HSPI bus arbiter (variant target.cpp); the contact DB is
// owned by the backend and reached only through these mailboxes.
//
// (Named MeshProxy, not "bridge", to avoid colliding with the unrelated
// mqtt-bridge project living in this tree.)

#include <stdint.h>
#include <stddef.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include "../../companion_radio/NodePrefs.h"
#include "MessageStore.h"          // CHAT_MSG_TEXT_MAX / CHAT_PEER_NAME_MAX

#ifndef MAX_CONTACTS
  #define MAX_CONTACTS 350
#endif
#ifndef MAX_GROUP_CHANNELS
  #define MAX_GROUP_CHANNELS 40
#endif

class MyMesh;   // backend type; only MeshProxy.cpp needs its full definition

namespace mproxy {

// ---- Snapshot (backend → UI read-model) ------------------------------------
// Reuses the firmware's own structs so UI field access is unchanged. A contact's
// local nickname override is resolved into the snapshot so displayName() needs
// no backend call.
struct ContactSnap {
  ContactInfo c;
  char        name_override[32];   // "" when none
};

struct MeshSnapshot {
  uint32_t       version;          // bumped on every publish; UI rebuilds on change
  uint8_t        self_pub_key[PUB_KEY_SIZE];
  bool           has_connection;
  NodePrefs      prefs;
  uint16_t       contact_count;
  ContactSnap    contacts[MAX_CONTACTS];
  ChannelDetails channels[MAX_GROUP_CHANNELS];   // unused slots have name[0]==0
};

// ---- Commands (UI → backend) -----------------------------------------------
enum class CmdKind : uint8_t {
  Send, SaveGps, ToggleFav, SetNameOvr, ResetPath, SetPath,
  AddContact, ReqTelem, ShareZhop, Advert, UpdatePrefs, ApplyRadio,
  AddChannel,   // reuses name = channel name, text = base64 PSK (validated UI-side)
};

struct MeshCmd {
  CmdKind  kind;
  uint8_t  pubkey[PUB_KEY_SIZE];               // target contact (where applicable)
  // Send
  uint32_t token;
  bool     is_channel;
  int      channel_idx;
  char     text[CHAT_MSG_TEXT_MAX + 32];
  // SaveGps
  int32_t  gps_lat, gps_lon;
  // SetNameOvr
  char     name[32];
  // SetPath
  uint8_t  path[MAX_PATH_SIZE];
  uint8_t  path_len;
  // AddContact (full record handed over from a received contact card)
  ContactInfo contact;
  // UpdatePrefs / ApplyRadio
  NodePrefs prefs;
};

// ---- Events (backend → UI) -------------------------------------------------
enum class EvKind : uint8_t { Msg, SendResult, Delivered, Telem, MsgCount };

struct UiEvent {
  EvKind   kind;
  // Msg
  bool     outgoing;
  char     conv_key[CHAT_PEER_NAME_MAX];
  char     sender[CHAT_PEER_NAME_MAX];
  char     text[CHAT_MSG_TEXT_MAX];
  uint32_t ts;
  // SendResult (+ Delivered reuses `ack`)
  uint32_t token;
  bool     ok;
  uint32_t ack;
  uint32_t timeout;
  // Telem
  uint8_t  pubkey[6];
  char     telem_text[160];
  // MsgCount
  int      msgcount;
};

// ---- Lifecycle -------------------------------------------------------------
bool init();                       // alloc snapshot (PSRAM) + queues; call once in setup()

// ---- Backend side (runs where `the_mesh` lives) ----------------------------
void drainCommands(MyMesh& mesh);      // execute all queued commands
bool publishIfChanged(MyMesh& mesh);   // rebuild + publish snapshot if anything changed; returns true if published
void pushEvent(const UiEvent& ev);     // enqueue a cooked callback result

// ---- UI side ---------------------------------------------------------------
void beginUiRead();                // pin the published snapshot for this UI pass (no-op single-core)
void endUiRead();
uint32_t snapshotVersion();
bool     hasConnection();
const uint8_t* selfPubKey();
int  getNumContacts();
bool getContactByIdx(int idx, ContactInfo& out);
const ContactInfo* lookupContactByPubKey(const uint8_t* key, int prefix_len);
bool getChannel(int idx, ChannelDetails& out);
bool getNameOverride(const uint8_t* pubkey, char* out, size_t cap);
const NodePrefs* prefsSnap();      // current published prefs (UI seeds its working copy from this)
uint32_t rtcSeconds();             // live device clock (ESP32 internal RTC; safe cross-core)

// Valid ONLY inside a mesh callback (backend thread): the conversation key of the
// message currently handed to the UI. The 5 UITask callbacks run on the backend,
// so they read these to cook their event before enqueuing it.
const uint8_t* hookKey();
bool           hookIsChannel();

bool postCommand(const MeshCmd& cmd);   // returns false if the queue is full
bool pollEvent(UiEvent& ev);            // false if no event pending

}  // namespace mproxy
