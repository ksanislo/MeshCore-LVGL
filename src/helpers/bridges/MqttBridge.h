#pragma once

#include "MeshCore.h"
#include "helpers/bridges/BridgeBase.h"

#ifdef WITH_MQTT_BRIDGE

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

/**
 * @brief Bridge implementation that publishes received mesh packets to an MQTT
 * broker on a "down" topic, and injects packets received on an "up" topic back
 * into the local mesh.
 *
 * Topic convention (from the daemon's perspective):
 *   <prefix>/up    - daemon publishes packets here for the firmware to TX on RF
 *   <prefix>/down  - firmware publishes every packet it heard from RF here
 *
 * Note: "up" / "down" mean "toward the air" / "from the air". This is
 * inverted from the LoRaWAN convention where uplink = device->cloud.
 *
 * The "down" publish payload uses a small fixed header:
 *   [1]  header version (currently 1)
 *   [4]  uptime millis (little-endian) when received
 *   [1]  RSSI (signed dBm)
 *   [1]  SNR  (signed, x4 - same convention as KISS RxMeta)
 *   [N]  raw mesh packet bytes (variable)
 *
 * "up" expects only raw mesh packet bytes (no header).
 */
class MqttBridge : public BridgeBase {
public:
  static const size_t MAX_MESH_PACKET   = 256;
  static const size_t DOWN_HEADER_LEN   = 7;
  static const size_t MQTT_BUFFER_SIZE  = 384;  // header + max packet + slack
  static const uint8_t DOWN_HEADER_VER  = 1;

  MqttBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet *packet) override;
  void onPacketReceived(mesh::Packet *packet) override;

  // Called by main when a TX has happened (only used if mqtt_publish_tx is on)
  void onPacketTransmitted(mesh::Packet *packet);

  // Record signal stats for the next sendPacket() call (called from logRx)
  void setLastSignal(int8_t rssi_dbm, int8_t snr_x4) {
    _last_rssi = rssi_dbm;
    _last_snr_x4 = snr_x4;
  }

  // Set the local pubkey, used for the default client_id when prefs are empty
  void setLocalPubkey(const uint8_t *pubkey, size_t len);

  // Populate `reply` with a brief status string
  void describeStatus(char *reply);

private:
  static MqttBridge *_instance;
  static void mqtt_cb(char *topic, uint8_t *payload, unsigned int length);
  void onMqttMessage(const char *topic, const uint8_t *payload, unsigned int length);
  bool tryConnect();
  void publishStatus(const char *state);
  void buildTopic(char *dest, size_t dest_sz, const char *suffix);
  void resolvedClientId(char *dest, size_t dest_sz);
  void resolvedTopicPrefix(char *dest, size_t dest_sz);

  WiFiClient       _plain_client;
  WiFiClientSecure _tls_client;
  PubSubClient     _client;

  bool      _running;
  uint32_t  _next_reconnect_ms;
  uint32_t  _reconnect_backoff_ms;
  uint32_t  _msgs_pub;
  uint32_t  _msgs_sub;
  uint32_t  _last_err_ms;
  int8_t    _last_rcd_state;       // PubSubClient::state() at last check

  // Captured by main from logRx so the next publish carries it
  int8_t    _last_rssi;
  int8_t    _last_snr_x4;

  // Cached resolved values; recomputed on applyMqttConfig()
  char      _resolved_client_id[24];
  char      _resolved_topic_prefix[64];
  char      _topic_up[80];
  char      _topic_down[80];
  char      _topic_status[80];

  // Local node pubkey hex (first 8 hex chars) used as fallback client id
  char      _pubkey_short_hex[9];
};

#endif // WITH_MQTT_BRIDGE
