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
  uint32_t       active_ble_pin;   // the live BLE pairing passkey (random per-boot when has_display); 0 if BLE off
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
  UpdateReleases,// fetch the GitHub releases list (tags + our asset URLs) -> backend cache
  OtaUpdate,    // download firmware -> OTA partition -> reboot. ota_mode/ota_release_idx pick the source
  OtaCancel,    // abort an in-flight OTA download (safe: only the inactive slot is touched)
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
  // OtaUpdate: pick the firmware source. The backend resolves the URL itself (keeps the long GitHub
  // asset URL out of the queued command). 0 = GitHub release at _ota_releases[ota_release_idx];
  // 1 = custom URL (prefs.ota_url).
  uint8_t  ota_mode;
  int8_t   ota_release_idx;
};

// ---- Events (backend → UI) -------------------------------------------------
enum class EvKind : uint8_t { Msg, SendResult, Delivered, Telem, MsgCount, LoginResult, OtaFailed, OtaRebooting };

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
  bool     is_cli;     // reply came as TXT_TYPE_CLI_DATA (admin/console reply, not a chat post)
  // Radio/diagnostic meta (written disk-only). Copied from the MyMesh hook in newMsg/sentMsg
  // (backend core); drainEvents reads ONLY these ev.* fields (never a hook accessor).
  bool     has_radio_meta;       // incoming rx fields valid
  int8_t   snr;        // SNR*4
  int8_t   rssi;       // dBm
  int16_t  noise;      // noise floor
  uint8_t  rx_header;  // pkt->header
  uint8_t  pkt_hash[8];
  uint8_t  rx_path[MAX_PATH_SIZE];
  uint8_t  rx_path_len;
  uint32_t sender_ts;  // sender's claimed timestamp
  int32_t  our_lat, our_lon;        // deg*1e6 at send/receive (both directions)
  int32_t  remote_lat, remote_lon;  // sender's last-advert position (incoming DM)
  uint32_t out_ack;    // outgoing companion-app send expected-ack
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

// Radio quiesce handshake (for a runtime SD (re)mount). The SD card shares the radio's HSPI bus;
// at boot a mount is safe because meshTask is still parked on s_ui_ready, but at runtime meshTask's
// the_mesh.loop() interleaves radio ops with our long bus-hold and wedges the SX1262 into a no-yield
// busy-spin (TASK_WDT on core-0 idle). The UI requests a pause, waits for meshTask to ack idle, does
// the mount with the radio quiescent (the boot condition), then releases. Safe cross-core (volatile).
void requestRadioPause(bool on);       // UI: ask meshTask to stop touching the radio
bool radioPauseRequested();            // meshTask: is a pause requested?
void setRadioIdle(bool idle);          // meshTask: ack that it is not in the radio path
bool radioIdle();                      // UI: has meshTask acked idle?

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
uint32_t activeBlePin();           // live BLE pairing passkey to show on screen (0 = BLE not active / no pin)
bool exportPrivKey(uint8_t out[64]);   // copy our 64-byte private key out (UI key export)
void wifiStatus(char* out, size_t cap);   // human-readable WiFi status for the UI
void mqttStatus(char* out, size_t cap);   // human-readable MQTT status for the UI
void wifiIpInfo(char* ip, char* mask, char* gw, char* dns, size_t cap);  // live addressing strings
void ntpStatus(char* out, size_t cap);   // NTP clock-sync status for the UI
void presetStatus(char* out, size_t cap);   // radio-preset update status for the UI
void otaStatus(char* out, size_t cap);       // OTA firmware-update status for the UI
bool otaBusy();                              // true while an OTA download task is running
// GitHub-release OTA: trigger a fetch, then read the cached list (newest-first, our-asset releases only).
void updateReleases();                       // post UpdateReleases (backend re-fetches the list)
int  otaReleaseCount();                      // cached releases available to choose from
bool otaRelease(int idx, char* tag, size_t tag_cap, bool* prerelease, char* url, size_t url_cap);
void otaReleaseStatus(char* out, size_t cap);// last release-list fetch result for the UI
bool releasesFetching();                     // true while the backend HTTPS release fetch runs
void uiLowMemReady(bool ready);              // UI -> backend ack: draw buf shrunk (TLS RAM free) / restored
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
bool           hookIsCli();
// Per-message radio meta accessors (read on the backend thread inside newMsg/sentMsg only).
bool           hookRxValid();
int8_t         hookRxSnr();
int8_t         hookRxRssi();
int16_t        hookRxNoise();
uint8_t        hookRxHeader();
const uint8_t* hookRxHash();      // 8 bytes
const uint8_t* hookRxPath();      // MAX_PATH_SIZE bytes
uint8_t        hookRxPathLen();
uint32_t       hookRxSenderTs();
int32_t        hookOurLat();
int32_t        hookOurLon();
int32_t        hookRxRemoteLat();
int32_t        hookRxRemoteLon();
uint32_t       hookOutAck();
void           selfLatLon(int32_t& lat_e6, int32_t& lon_e6);   // our node position (UI-core safe; near-static)

// A saved server password (login store) for pre-filling a login dialog. Copies the password
// into out and returns true if one is stored for this 6-byte pubkey, else false. Read-only.
bool           savedLogin(const uint8_t* pubkey, char* out, size_t cap);

bool postCommand(const MeshCmd& cmd);   // returns false if the queue is full
bool pollEvent(UiEvent& ev);            // false if no event pending

}  // namespace mproxy
