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
#include "../../companion_radio/NodeStats.h"
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
  AddChannel,   // reuses name = channel name, path/path_len = raw secret bytes (16/32)
  RenameChannel,// rename a channel: path = its 32-byte secret (identity), name = new name
  ServerLogin,  // login to a repeater/room server: pubkey + password (+ save_login)
  SendCommand,  // send a CLI command (TXT_TYPE_CLI_DATA) to a repeater: pubkey + text
  AutoLogin,    // pubkey: log in using a saved credential if one exists (silent)
  SetRadio,     // apply prefs.radio_off: sleep the radio (off) or re-enable it (on) + persist
  SetMute,      // mute/unmute a conversation: name = conv-key, muted = on/off; persists
  RemoveContact,// delete a contact: pubkey
  RemoveChannel,// delete a channel: path = its 32-byte secret (identity)
  AdvertFlood,  // self-advert, flooded (Advert is zero-hop)
  ImportKey,    // replace our identity: path = the new 64-byte private key; reboots on success
  ApplyWifi,    // copy prefs (wifi_*) + save + (re)connect WiFi (+ re-arm NTP)
  ApplyMqtt,    // copy prefs (mqtt_*) + save + (re)start the MQTT bridge
  SyncNtp,      // kick an immediate NTP clock sync
  UpdatePresets,// fetch the official radio presets over HTTPS -> internal flash
  OtaUpdate,    // download prefs.ota_url firmware -> OTA partition -> reboot
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
  // SetNameOvr  /  AddChannel (channel name)
  char     name[32];
  // ServerLogin
  char     password[16];
  bool     save_login;     // ServerLogin: persist the credential
  bool     auto_login;     // ServerLogin: mark the saved credential for auto-login
  bool     muted;          // SetMute: true=mute, false=unmute (conv-key in `name`)
  // SetPath
  uint8_t  path[MAX_PATH_SIZE];
  uint8_t  path_len;
  // AddContact (full record handed over from a received contact card)
  ContactInfo contact;
  // UpdatePrefs / ApplyRadio
  NodePrefs prefs;
};

// ---- Events (backend → UI) -------------------------------------------------
enum class EvKind : uint8_t { Msg, SendResult, Delivered, Telem, MsgCount, LoginResult };

struct UiEvent {
  EvKind   kind;
  // Msg
  bool     outgoing;
  char     conv_key[CHAT_PEER_NAME_MAX];
  char     sender[CHAT_PEER_NAME_MAX];
  char     text[CHAT_MSG_TEXT_MAX];
  uint32_t ts;
  uint8_t  hops;       // incoming flood path length (0xFF = direct/unknown)
  uint16_t bytes;      // incoming message payload size (diagnostic footer)
  // SendResult (+ Delivered reuses `ack`)
  uint32_t token;
  bool     ok;
  uint32_t ack;
  uint32_t timeout;
  // Telem  /  LoginResult (pubkey of the server)
  uint8_t  pubkey[6];
  char     telem_text[160];
  // MsgCount
  int      msgcount;
  // LoginResult (ok reuses `ok`; pubkey reuses `pubkey`)
  uint8_t  is_admin;
  uint16_t keep_alive;
};

// ---- Lifecycle -------------------------------------------------------------
bool init();                       // alloc snapshot (PSRAM) + queues; call once in setup()

// ---- Backend side (runs where `the_mesh` lives) ----------------------------
void setBackend(MyMesh& mesh);         // capture the backend for read-only accessors; call BEFORE the UI seeds (begin)
void drainCommands(MyMesh& mesh);      // execute all queued commands
bool publishIfChanged(MyMesh& mesh);   // rebuild + publish snapshot if anything changed; returns true if published
void updateStats(MyMesh& mesh);        // refresh the live node-stats buffer (call each backend loop)
void pushEvent(const UiEvent& ev);     // enqueue a cooked callback result

// ---- UI side ---------------------------------------------------------------
void beginUiRead();                // pin the published snapshot for this UI pass (no-op single-core)
void endUiRead();
uint32_t snapshotVersion();
bool     hasConnection();
const uint8_t* selfPubKey();
const char* firmwareVersion();     // upstream MeshCore FIRMWARE_VERSION string
int  getNumContacts();
bool getContactByIdx(int idx, ContactInfo& out);
const ContactInfo* lookupContactByPubKey(const uint8_t* key, int prefix_len);
bool getChannel(int idx, ChannelDetails& out);
void getStats(NodeStats& out);     // latest node/radio counters (display-only, lock-protected copy)
bool getNameOverride(const uint8_t* pubkey, char* out, size_t cap);
const NodePrefs* prefsSnap();      // current published prefs (UI seeds its working copy from this)
bool exportPrivKey(uint8_t out[64]);   // copy our 64-byte private key out (UI key export)
void wifiStatus(char* out, size_t cap);   // human-readable WiFi status for the UI
void mqttStatus(char* out, size_t cap);   // human-readable MQTT status for the UI
void wifiIpInfo(char* ip, char* mask, char* gw, char* dns, size_t cap);  // live addressing strings
void ntpStatus(char* out, size_t cap);   // NTP clock-sync status for the UI
void presetStatus(char* out, size_t cap);   // radio-preset update status for the UI
void otaStatus(char* out, size_t cap);       // OTA firmware-update status for the UI
int  copyMutedKeys(char out[][CHAT_PEER_NAME_MAX], int max);    // seed the UI's explicit-muted set at begin()
int  copyUnmutedKeys(char out[][CHAT_PEER_NAME_MAX], int max);  // seed the UI's explicit-unmuted set at begin()
uint32_t rtcSeconds();             // live device clock (ESP32 internal RTC; safe cross-core)

// Signal-strength meter: a peak-hold-with-decay envelope over heard-packet SNR. The
// backend feeds samples from onRecvPacket (noteRxSnr); the UI reads the current decayed
// level (signalLevelDb) at 1 Hz and quantizes it to bars. hold/tau come from NodePrefs.
// Two-scalar state, written backend / read UI -- a torn read at worst skews one frame.
void  noteRxSnr(float snr_db, uint32_t now_ms, uint32_t hold_ms, uint32_t tau_ms);
float signalLevelDb(uint32_t now_ms, uint32_t hold_ms, uint32_t tau_ms);  // decayed dB; -127 if no data yet
bool  signalHasData();             // false until the first real sample (cold start = empty bars)

// Valid ONLY inside a mesh callback (backend thread): the conversation key of the
// message currently handed to the UI. The 5 UITask callbacks run on the backend,
// so they read these to cook their event before enqueuing it.
const uint8_t* hookKey();
bool           hookIsChannel();

bool postCommand(const MeshCmd& cmd);   // returns false if the queue is full
bool pollEvent(UiEvent& ev);            // false if no event pending

}  // namespace mproxy
