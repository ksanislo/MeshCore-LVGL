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
 * Topic convention:
 *   <prefix>/rx       - firmware publishes RX-flavor packets here (heard from
 *                       RF, pre-forward) when mqtt.publish_rx = on
 *   <prefix>/tx       - firmware publishes TX-flavor packets here (post-forward
 *                       with this bridge's hash appended) when mqtt.publish_tx
 *                       = on
 *   <prefix>/rf       - default subscribe target: any packet published here is
 *                       injected onto our local RF as if heard from the air
 *   <prefix>/status   - retained "online"/"offline" via LWT
 *
 * The subscribe target is configurable via mqtt.subscribe (empty = default
 * <prefix>/rf). Setting it to e.g. "meshcore/some-peer/tx" turns this bridge
 * into a passive listener that re-broadcasts whatever the peer transmits --
 * no daemon required for simple symmetric pairing.
 *
 * Placeholder substitution: mqtt.client_id, mqtt.topic_prefix, and
 * mqtt.subscribe all accept these tokens, expanded at connect time:
 *   {client_id}  -> the resolved client_id (mqtt.client_id, or its
 *                   auto-derived default "meshcore-<8 hex of pubkey>").
 *                   Only valid in topic_prefix and subscribe -- inside
 *                   mqtt.client_id itself it's passed through literally
 *                   to avoid self-referential recursion.
 *   {pubkey}     -> the first 8 hex chars of this device's pubkey.
 *                   Valid in all three fields.
 * Example: mqtt.client_id = "rover-{pubkey}" resolves to
 * "rover-10db83e6"; mqtt.topic_prefix = "meshedup/{client_id}" then
 * resolves to "meshedup/rover-10db83e6".
 * The empty default for mqtt.topic_prefix is equivalent to
 * "meshcore/{client_id}".
 *
 * Loop suppression is delegated to the underlying mesh layer's own
 * SimpleMeshTables. MeshCore is designed to handle constant RF echoes; an
 * MQTT-arrived packet that the mesh has already seen (because we originated
 * it or forwarded it ourselves) is dropped by the mesh's normal hasSeen()
 * check. No firmware-level dedup needed -- the bandwidth cost of publishing
 * extra copies on /rx is small and the daemon can dedup if it wants.
 *
 * Topic-level self-filter: messages whose topic matches our own /rx or /tx
 * are silently dropped before parsing. Defends against wildcard subscribe
 * patterns like "meshcore/+/tx" pulling our own publishes back to us.
 *
 * /rx and /tx publish payloads share a fixed header:
 *   [1]  header version (currently 1)
 *   [4]  uptime millis (little-endian) when the event was logged
 *   [1]  RSSI (signed dBm; meaningful for /rx, stale on /tx)
 *   [1]  SNR  (signed, x4 - same convention as KISS RxMeta)
 *   [N]  raw mesh packet bytes (variable)
 *
 * Inbound subscribe payload: if topic ends in "/rx" or "/tx", the header is
 * stripped before injection. Otherwise (e.g. /rf), bytes are treated as raw
 * mesh packet bytes.
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
  // sendPacket() routes by direction: rx-flavor publishes go to /rx, tx-flavor
  // publishes go to /tx. The two are independent (gated by mqtt.publish_rx and
  // mqtt.publish_tx prefs).
  void sendPacket(mesh::Packet *packet) override;
  void publishRx(mesh::Packet *packet);
  void publishTx(mesh::Packet *packet);
  void onPacketReceived(mesh::Packet *packet) override;

  // Record signal stats for the next publishRx() call (called from logRx)
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

  // Expand {client_id} and {pubkey} placeholders in user-supplied strings
  // (mqtt.topic_prefix, mqtt.subscribe, mqtt.client_id). Unknown placeholders
  // pass through literally. Output is always null-terminated.
  //
  // When resolving mqtt.client_id itself, pass allow_client_id=false to
  // prevent self-referential recursion ({client_id} inside the client_id
  // value would otherwise call back into resolvedClientId).
  void expandPlaceholders(const char *src, char *dest, size_t dest_sz,
                          bool allow_client_id = true);

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
  char      _topic_rx[80];        // <prefix>/rx
  char      _topic_tx[80];        // <prefix>/tx
  char      _topic_status[80];    // <prefix>/status
  char      _topic_subscribe[96]; // mqtt.subscribe or "<prefix>/rf"

  // Local node pubkey hex (first 8 hex chars) used as fallback client id
  char      _pubkey_short_hex[9];
};

#endif // WITH_MQTT_BRIDGE
