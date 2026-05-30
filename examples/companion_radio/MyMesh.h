#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include "AbstractUITask.h"

/*------------ Frame Protocol --------------*/
#define FIRMWARE_VER_CODE 11

#ifndef FIRMWARE_BUILD_DATE
#define FIRMWARE_BUILD_DATE "19 Apr 2026"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v1.15.0"
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(ESP32)
#include <SPIFFS.h>
#endif

#include "DataStore.h"
#include "NodePrefs.h"
#include "NodeStats.h"

#include <RTClib.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <target.h>

/* ---------------------------------- CONFIGURATION ------------------------------------- */

#ifndef LORA_FREQ
#define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
#define LORA_BW 250
#endif
#ifndef LORA_SF
#define LORA_SF 10
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 20
#endif
#ifndef MAX_LORA_TX_POWER
#define MAX_LORA_TX_POWER LORA_TX_POWER
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 100
#endif

#ifndef OFFLINE_QUEUE_SIZE
#define OFFLINE_QUEUE_SIZE 16
#endif

#ifndef BLE_NAME_PREFIX
#define BLE_NAME_PREFIX "MeshCore-"
#endif

#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

/* -------------------------------------------------------------------------------------- */

#define REQ_TYPE_GET_STATUS             0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE             0x02
#define REQ_TYPE_GET_TELEMETRY_DATA     0x03

struct AdvertPath {
  uint8_t pubkey_prefix[7];
  uint8_t path_len;
  char    name[32];
  uint32_t recv_timestamp;
  uint8_t path[MAX_PATH_SIZE];
};

#ifdef ENABLE_LOGIN_STORE
  #ifndef MAX_LOGINS
  #define MAX_LOGINS 32
  #endif
  // A saved repeater/room-server credential (on-device standalone use); persisted
  // to /logins. pubkey is the 6-byte prefix; auto=1 means auto-login on chat open.
  struct LoginCred { uint8_t pubkey[6]; char password[16]; uint8_t autolog; };
#endif

class MyMesh : public BaseChatMesh, public DataStoreHost {
public:
  MyMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore& store, AbstractUITask* ui=NULL);

  void begin(bool has_display);
  void startInterface(BaseSerialInterface &serial);

  const char *getNodeName();
  NodePrefs *getNodePrefs();
  uint32_t getBLEPin();

  void loop();
  void handleCmdFrame(size_t len);
  bool advert();
  void enterCLIRescue();

  int  getRecentlyHeard(AdvertPath dest[], int max_num);

protected:
  float getAirtimeBudgetFactor() const override;
  int getInterferenceThreshold() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet *packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet *packet) override;
  uint8_t getExtraAckTransmitCount() const override;
  bool filterRecvFloodPacket(mesh::Packet* packet) override;
  bool allowPacketForward(const mesh::Packet* packet) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis);
  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  bool isAutoAddEnabled() const override;
  bool shouldAutoAddContactType(uint8_t type) const override;
  bool shouldOverwriteWhenFull() const override;
  uint8_t getAutoAddMaxHops() const override;
  void onContactsFull() override;
  void onContactOverwrite(const uint8_t* pub_key) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onContactPathUpdated(const ContactInfo &contact) override;
  ContactInfo* processAck(const uint8_t *data) override;
  void queueMessage(const ContactInfo &from, uint8_t txt_type, mesh::Packet *pkt, uint32_t sender_timestamp,
                    const uint8_t *extra, int extra_len, const char *text);

  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                     const char *text) override;
  void onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                         const char *text) override;
  void onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                           const uint8_t *sender_prefix, const char *text) override;
  void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp,
                            const char *text) override;
  void onChannelDataRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint16_t data_type,
                         const uint8_t *data, size_t data_len) override;

  uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data,
                           uint8_t len, uint8_t *reply) override;
  void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onControlDataRecv(mesh::Packet *packet) override;
  void onRawDataRecv(mesh::Packet *packet) override;
  void onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) override;

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;
  void onSendTimeout() override;

  // DataStoreHost methods
  bool onContactLoaded(const ContactInfo& contact) override { return addContact(contact); }
  bool getContactForSave(uint32_t idx, ContactInfo& contact) override { return getContactByIdx(idx, contact); }
  bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) override { return setChannel(channel_idx, ch); }
  bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) override { return getChannel(channel_idx, ch); }

  void clearPendingReqs() {
    pending_login = pending_status = pending_telemetry = pending_discovery = pending_req = 0;
  }

public:
  void savePrefs() { _store->savePrefs(_prefs, sensors.node_lat, sensors.node_lon); }
  // Request telemetry from a contact AND record the tag so onContactResponse()
  // matches the reply (the on-device UI calls this; the BLE path sets the tag
  // in its own CMD handler). Returns false if the send failed.
  // Register an expected ACK so processAck() can confirm delivery of a message
  // sent directly by the on-device UI (the BLE CMD path registers its own).
  void addExpectedAck(uint32_t ack, ContactInfo* c) {
    if (!ack) return;
    expected_ack_table[next_ack_idx].msg_sent = _ms->getMillis();
    expected_ack_table[next_ack_idx].ack = ack;
    expected_ack_table[next_ack_idx].contact = c;
    next_ack_idx = (next_ack_idx + 1) % (int)(sizeof(expected_ack_table) / sizeof(expected_ack_table[0]));
  }
  bool requestTelemetry(const ContactInfo& c) {
    uint32_t tag = 0, est = 0;
    if (sendRequest(c, REQ_TYPE_GET_TELEMETRY_DATA, tag, est) == MSG_SEND_FAILED) return false;
    pending_telemetry = tag;
    return true;
  }
  // On-device repeater/room console: log in (sets pending_login so onContactResponse
  // matches the reply -> _ui->loginResult) and send a CLI command (reply arrives via
  // onCommandDataRecv as a CLI_DATA message). Mirror the BLE CMD_SEND_LOGIN path.
  bool loginToServer(const ContactInfo& c, const char* password) {
    uint32_t est = 0;
    if (sendLogin(c, password, est) == MSG_SEND_FAILED) return false;
    memcpy(&pending_login, c.id.pub_key, 4);
    return true;
  }
  bool sendCliCommand(const ContactInfo& c, const char* text) {
    uint32_t est = 0;
    return sendCommandData(c, getRTCClock()->getCurrentTimeUnique(), 0, text, est) != MSG_SEND_FAILED;
  }
#ifdef ENABLE_LOGIN_STORE
  const LoginCred* findLoginCred(const uint8_t* pubkey); // saved cred (6-byte key) or NULL
  void        saveLogin(const uint8_t* pubkey, const char* password, bool autolog);
  void        loadLoginCreds();                          // from /logins at boot
#endif
  void saveContacts() { _store->saveContacts(this); }  // public: on-device UI edits contacts
  void saveChannels() { _store->saveChannels(this); }  // public: on-device UI adds channels
  // Per-conversation mute: a set of UI-defined conversation keys (pubkey/secret hex,
  // <=15 chars). The backend treats them as opaque strings and persists to /mutes.
  void   loadMutes();                      // from /mutes at boot
  bool   isMuted(const char* key) const;
  void   setMute(const char* key, bool on);
  int    numMutes() const { return _num_mutes; }
  const char* muteKey(int i) const { return (i >= 0 && i < _num_mutes) ? _mutes[i] : ""; }
  void getNodeStats(NodeStats& s);                     // public: on-device node-info screen
  // Persist a single edited contact in place (crash-safe vs the full rewrite).
  // Use for field edits only (add/remove change indices -> use saveContacts()).
  bool saveContact(const ContactInfo& c) {
    for (uint32_t i = 0; i < (uint32_t)getNumContacts(); i++) {
      ContactInfo tmp;
      if (getContactByIdx(i, tmp) && memcmp(tmp.id.pub_key, c.id.pub_key, PUB_KEY_SIZE) == 0) {
        if (_store->updateContact(i, c)) return true;
        break;  // file short/missing -> fall back below
      }
    }
    saveContacts();  // fallback resyncs the whole file
    return false;
  }

  // Stable conversation key of the message currently being handed to the UI
  // (set right before each newMsg/sentMsg call): the contact pubkey prefix for a
  // direct message, or the channel secret prefix for a channel message. Lets the
  // on-device UI key chat history by identity instead of a mutable display name.
  const uint8_t* hookKey() const { return _hook_key; }
  bool hookIsChannel() const { return _hook_is_channel; }

  // Local display-name overrides, stored OUTSIDE the contact record (a small
  // RAM table persisted to /disp_names) so they survive advert name overwrites.
  // Presentation-only: identity, routing and chat keys still use the advert name.
  // Kept in RAM so the on-device UI can look them up per-contact without flash.
  void loadNameOverrides() {
    _name_ov_count = 0;
    File f = _store->openRead("/disp_names");
    if (!f) return;
    while (_name_ov_count < MAX_NAME_OVERRIDES) {
      NameOverride o;
      if (f.read(o.pubkey, 6) != 6) break;
      if (f.read((uint8_t*)o.name, 32) != 32) break;
      o.name[31] = 0;
      _name_ov[_name_ov_count++] = o;
    }
    f.close();
  }
  bool getNameOverride(const uint8_t* pubkey, char* out, size_t cap) {
    for (int i = 0; i < _name_ov_count; i++) {
      if (memcmp(_name_ov[i].pubkey, pubkey, 6) == 0) {
        strncpy(out, _name_ov[i].name, cap - 1);
        out[cap - 1] = 0;
        return out[0] != 0;
      }
    }
    if (cap) out[0] = 0;
    return false;
  }
  void setNameOverride(const uint8_t* pubkey, const char* name) {
    int idx = -1;
    for (int i = 0; i < _name_ov_count; i++)
      if (memcmp(_name_ov[i].pubkey, pubkey, 6) == 0) { idx = i; break; }
    if (!name || !name[0]) {                       // clear
      if (idx >= 0) _name_ov[idx] = _name_ov[--_name_ov_count];
    } else {
      if (idx < 0) {
        if (_name_ov_count >= MAX_NAME_OVERRIDES) return;
        idx = _name_ov_count++;
        memcpy(_name_ov[idx].pubkey, pubkey, 6);
      }
      strncpy(_name_ov[idx].name, name, sizeof(_name_ov[idx].name) - 1);
      _name_ov[idx].name[sizeof(_name_ov[idx].name) - 1] = 0;
    }
    saveNameOverrides();
  }
  void saveNameOverrides() {
    File f = _store->createFile("/disp_names");
    if (!f) return;
    for (int i = 0; i < _name_ov_count; i++) {
      f.write(_name_ov[i].pubkey, 6);
      f.write((uint8_t*)_name_ov[i].name, 32);
    }
    f.close();
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled ? "1" : "0");
    if (_prefs.gps_interval > 0) {
      char interval_str[12];  // Max: 24 hours = 86400 seconds (5 digits + null)
      sprintf(interval_str, "%u", _prefs.gps_interval);
      sensors.setSettingValue("gps_interval", interval_str);
    }
  }
#endif

private:
  struct NameOverride { uint8_t pubkey[6]; char name[32]; };
  static const int MAX_NAME_OVERRIDES = 32;
  NameOverride _name_ov[MAX_NAME_OVERRIDES];
  int _name_ov_count = 0;

#ifdef ENABLE_LOGIN_STORE
  LoginCred _logins[MAX_LOGINS];
  int _num_logins = 0;
#endif

  static const int MAX_MUTES = 64;
  char _mutes[MAX_MUTES][20];    // muted conversation keys (UI-defined, <=15 chars + NUL)
  int  _num_mutes = 0;

  uint8_t _hook_key[6] = {0};   // identity of the msg currently handed to the UI
  bool _hook_is_channel = false;
  void setHookKey(const uint8_t* k6, bool is_channel) { memcpy(_hook_key, k6, 6); _hook_is_channel = is_channel; }

  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeDisabledFrame();
  void writeContactRespFrame(uint8_t code, const ContactInfo &contact);
  void updateContactFromFrame(ContactInfo &contact, uint32_t& last_mod, const uint8_t *frame, int len);
  void addToOfflineQueue(const uint8_t frame[], int len);
  int getFromOfflineQueue(uint8_t frame[]);
  int getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) override { 
    return _store->getBlobByKey(key, key_len, dest_buf);
  }
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], int len) override {
    return _store->putBlobByKey(key, key_len, src_buf, len);
  }

  void checkCLIRescueCmd();
  void checkSerialInterface();
  bool isValidClientRepeatFreq(uint32_t f) const;

  DataStore* _store;
  NodePrefs _prefs;
  uint32_t pending_login;
  uint32_t pending_status;
  uint32_t pending_telemetry, pending_discovery;   // pending _TELEMETRY_REQ
  uint32_t pending_req;   // pending _BINARY_REQ
  BaseSerialInterface *_serial;
  AbstractUITask* _ui;

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  uint32_t _active_ble_pin;
  bool _iter_started;
  bool _cli_rescue;
  char cli_command[80];
  uint8_t app_target_ver;
  uint8_t *sign_data;
  uint32_t sign_data_len;
  unsigned long dirty_contacts_expiry;

  TransportKey send_scope;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];
  CayenneLPP telemetry;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];

    bool isChannelMsg() const;
  };
  int offline_queue_len;
#ifdef ENABLE_PSRAM_OFFLINE_QUEUE
  // Opt-in: the offline store-and-forward queue (large at big OFFLINE_QUEUE_SIZE,
  // touched only on queue/dequeue -- not the per-packet path) lives in PSRAM,
  // allocated once in begin(). Index-based access, so source-compatible with the
  // static array. See helpers/MeshPSRAM.h.
  Frame* offline_queue;
#else
  Frame offline_queue[OFFLINE_QUEUE_SIZE];
#endif

  struct AckTableEntry {
    unsigned long msg_sent;
    uint32_t ack;
    ContactInfo* contact;
  };
  #define EXPECTED_ACK_TABLE_SIZE 8
  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE]; // circular table
  int next_ack_idx;

  #define ADVERT_PATH_TABLE_SIZE   16
  AdvertPath advert_paths[ADVERT_PATH_TABLE_SIZE]; // circular table
};

extern MyMesh the_mesh;
