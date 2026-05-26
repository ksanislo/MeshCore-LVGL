#pragma once

#include "Mesh.h"
#include <helpers/IdentityStore.h>
#include <helpers/SensorManager.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>

#if defined(WITH_RS232_BRIDGE) || defined(WITH_ESPNOW_BRIDGE)
#define WITH_BRIDGE
#endif

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1
#define ADVERT_LOC_PREFS      2

#define LOOP_DETECT_OFF       0
#define LOOP_DETECT_MINIMAL   1
#define LOOP_DETECT_MODERATE  2
#define LOOP_DETECT_STRICT    3

// Repeat / forwarding mode for NodePrefs.disable_fwd. Backward compatible
// with the old boolean meaning: 0 = forward (repeater), 1 = don't forward.
// A new value 2 = "bridge mode": forward only packets that arrived via a
// Bridge (MQTT/RS232/ESPNow), not packets we heard on RF. Lets a node be
// a quiet RF observer while still injecting cross-bridge traffic.
#define REPEAT_ON     0   // forward everything (full repeater; legacy "on")
#define REPEAT_OFF    1   // forward nothing (legacy "off")
#define REPEAT_BRIDGE 2   // forward only bridge-injected packets

struct NodePrefs { // persisted to file
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  char password[16];
  float freq;
  int8_t tx_power_dbm;
  uint8_t disable_fwd;
  uint8_t advert_interval;       // minutes / 2
  uint8_t flood_advert_interval; // hours
  float rx_delay_base;
  float tx_delay_factor;
  char guest_password[16];
  float direct_tx_delay_factor;
  uint32_t guard;
  uint8_t sf;
  uint8_t cr;
  uint8_t allow_read_only;
  uint8_t multi_acks;
  float bw;
  uint8_t flood_max;
  uint8_t interference_threshold;
  uint8_t agc_reset_interval; // secs / 4
  // Bridge settings
  uint8_t bridge_enabled; // boolean
  uint16_t bridge_delay;  // milliseconds (default 500 ms)
  uint8_t bridge_pkt_src; // 0 = logTx, 1 = logRx (default logTx)
  uint32_t bridge_baud;   // 9600, 19200, 38400, 57600, 115200 (default 115200)
  uint8_t bridge_channel; // 1-14 (ESP-NOW only)
  char bridge_secret[16]; // for XOR encryption of bridge packets (ESP-NOW only)
  // Power setting
  uint8_t powersaving_enabled; // boolean
  // Gps settings
  uint8_t gps_enabled;
  uint32_t gps_interval; // in seconds
  uint8_t advert_loc_policy;
  uint32_t discovery_mod_timestamp;
  float adc_multiplier;
  char owner_info[120];
  uint8_t rx_boosted_gain; // power settings
  uint8_t path_hash_mode;   // which path mode to use when sending
  uint8_t loop_detect;
  // WiFi settings (ESP32 only; persisted across all builds for layout stability)
  uint8_t  wifi_enabled;    // boolean
  char     wifi_ssid[33];   // 32-char SSID + null
  char     wifi_password[64]; // up to 63-char WPA2 PSK + null
  uint32_t wifi_ip;         // 0 = DHCP, else static IPv4 in network byte order (a|b<<8|c<<16|d<<24)
  uint32_t wifi_netmask;
  uint32_t wifi_gateway;
  uint32_t wifi_dns;
  // MQTT bridge settings
  uint8_t  mqtt_enabled;       // boolean
  char     mqtt_host[64];      // broker hostname or IP
  uint16_t mqtt_port;          // typically 1883 (plain) or 8883 (TLS)
  char     mqtt_user[32];      // optional, empty = anonymous
  char     mqtt_password[64];  // optional
  char     mqtt_client_id[24]; // empty = auto-derive from pubkey
  char     mqtt_topic_prefix[48]; // empty = "meshcore/<client_id>"
  uint8_t  mqtt_tls;           // boolean; insecure-mode TLS for v1
  uint8_t  mqtt_publish_tx;    // boolean; publish post-forward TX bytes to <prefix>/tx
  uint8_t  mqtt_publish_rx;    // boolean; publish heard RX bytes to <prefix>/rx
  char     mqtt_subscribe[80]; // subscribe topic pattern; empty = "<prefix>/rf"
};

class CommonCLICallbacks {
public:
  virtual void savePrefs() = 0;
  virtual const char* getFirmwareVer() = 0;
  virtual const char* getBuildDate() = 0;
  virtual const char* getRole() = 0;
  virtual bool formatFileSystem() = 0;
  virtual void sendSelfAdvertisement(int delay_millis, bool flood) = 0;
  virtual void updateAdvertTimer() = 0;
  virtual void updateFloodAdvertTimer() = 0;
  virtual void setLoggingOn(bool enable) = 0;
  virtual void eraseLogFile() = 0;
  virtual void dumpLogFile() = 0;
  virtual void setTxPower(int8_t power_dbm) = 0;
  virtual void formatNeighborsReply(char *reply) = 0;
  virtual void removeNeighbor(const uint8_t* pubkey, int key_len) {
    // no op by default
  };
  virtual void formatStatsReply(char *reply) = 0;
  virtual void formatRadioStatsReply(char *reply) = 0;
  virtual void formatPacketStatsReply(char *reply) = 0;
  virtual mesh::LocalIdentity& getSelfId() = 0;
  virtual void saveIdentity(const mesh::LocalIdentity& new_id) = 0;
  virtual void clearStats() = 0;
  virtual void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) = 0;

  virtual void startRegionsLoad() {
    // no op by default
  }
  virtual bool saveRegions() {
    return false;
  }
  virtual void onDefaultRegionChanged(const RegionEntry* r) {
    // no op by default
  }

  virtual void setBridgeState(bool enable) {
    // no op by default
  };

  virtual void restartBridge() {
    // no op by default
  };

  virtual void setRxBoostedGain(bool enable) {
    // no op by default
  };

  virtual void applyWifiConfig() {
    // no op by default
  };

  // Fill `reply` with a human-readable WiFi status string ("disconnected", "connected ip=... rssi=...", etc).
  // Default writes "not supported" so non-WiFi builds give a sensible answer.
  virtual void getWifiStatus(char* reply) {
    strcpy(reply, "not supported");
  };

  virtual void applyMqttConfig() {
    // no op by default
  };

  // Fill `reply` with a human-readable MQTT status ("disabled", "connected msgs_tx=N msgs_rx=N", etc).
  virtual void getMqttStatus(char* reply) {
    strcpy(reply, "not supported");
  };
};

class CommonCLI {
  mesh::RTCClock* _rtc;
  NodePrefs* _prefs;
  CommonCLICallbacks* _callbacks;
  mesh::MainBoard* _board;
  SensorManager* _sensors;
  RegionMap* _region_map;
  ClientACL* _acl;
  char tmp[PRV_KEY_SIZE*2 + 4];

  mesh::RTCClock* getRTCClock() { return _rtc; }
  void savePrefs();
  void loadPrefsInt(FILESYSTEM* _fs, const char* filename);

  void handleRegionCmd(char* command, char* reply);
  void handleGetCmd(uint32_t sender_timestamp, char* command, char* reply);
  void handleSetCmd(uint32_t sender_timestamp, char* command, char* reply);

public:
  CommonCLI(mesh::MainBoard& board, mesh::RTCClock& rtc, SensorManager& sensors, RegionMap& region_map, ClientACL& acl, NodePrefs* prefs, CommonCLICallbacks* callbacks)
      : _board(&board), _rtc(&rtc), _sensors(&sensors), _region_map(&region_map), _acl(&acl), _prefs(prefs), _callbacks(callbacks) { }

  void loadPrefs(FILESYSTEM* _fs);
  void savePrefs(FILESYSTEM* _fs);
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  uint8_t buildAdvertData(uint8_t node_type, uint8_t* app_data);
};
