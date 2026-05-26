#include "MqttBridge.h"

#ifdef WITH_MQTT_BRIDGE

#include <Arduino.h>

MqttBridge *MqttBridge::_instance = nullptr;

void MqttBridge::mqtt_cb(char *topic, uint8_t *payload, unsigned int length) {
  if (_instance) {
    _instance->onMqttMessage(topic, payload, length);
  }
}

MqttBridge::MqttBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc),
      _client(_plain_client),
      _running(false),
      _next_reconnect_ms(0),
      _reconnect_backoff_ms(1000),
      _msgs_pub(0),
      _msgs_sub(0),
      _last_err_ms(0),
      _last_rcd_state(0),
      _last_rssi(0),
      _last_snr_x4(0) {
  _instance = this;
  _resolved_client_id[0] = 0;
  _resolved_topic_prefix[0] = 0;
  _topic_rx[0] = 0;
  _topic_tx[0] = 0;
  _topic_status[0] = 0;
  _topic_subscribe[0] = 0;
  _pubkey_short_hex[0] = 0;
  _client.setCallback(mqtt_cb);
  _client.setBufferSize(MQTT_BUFFER_SIZE);
}

void MqttBridge::setLocalPubkey(const uint8_t *pubkey, size_t len) {
  size_t to_copy = (len < 4) ? len : 4;
  for (size_t i = 0; i < to_copy; i++) {
    sprintf(&_pubkey_short_hex[i * 2], "%02x", pubkey[i]);
  }
  _pubkey_short_hex[to_copy * 2] = 0;
}

void MqttBridge::resolvedClientId(char *dest, size_t dest_sz) {
  if (_prefs->mqtt_client_id[0]) {
    // User-supplied value -- expand {pubkey} but not {client_id} (would
    // be self-referential).
    expandPlaceholders(_prefs->mqtt_client_id, dest, dest_sz,
                       /*allow_client_id=*/false);
  } else if (_pubkey_short_hex[0]) {
    snprintf(dest, dest_sz, "meshcore-%s", _pubkey_short_hex);
  } else {
    strncpy(dest, "meshcore-bridge", dest_sz - 1);
    dest[dest_sz - 1] = 0;
  }
}

void MqttBridge::resolvedTopicPrefix(char *dest, size_t dest_sz) {
  // Empty value resolves to the implicit default "meshcore/{client_id}".
  // Either way, run the result through expandPlaceholders so user-supplied
  // strings and the default both follow the same substitution rules.
  const char *src = _prefs->mqtt_topic_prefix[0]
                      ? _prefs->mqtt_topic_prefix
                      : "meshcore/{client_id}";
  expandPlaceholders(src, dest, dest_sz);
}

void MqttBridge::expandPlaceholders(const char *src, char *dest, size_t dest_sz,
                                    bool allow_client_id) {
  if (dest_sz == 0) return;
  size_t out = 0;
  while (*src && out + 1 < dest_sz) {
    if (*src == '{') {
      const char *end = strchr(src + 1, '}');
      if (end) {
        size_t name_len = (size_t)(end - src - 1);
        const char *replacement = nullptr;
        char cid_buf[24];
        if (allow_client_id && name_len == 9 &&
            memcmp(src + 1, "client_id", 9) == 0) {
          resolvedClientId(cid_buf, sizeof(cid_buf));
          replacement = cid_buf;
        } else if (name_len == 6 && memcmp(src + 1, "pubkey", 6) == 0) {
          replacement = _pubkey_short_hex;
        }
        if (replacement) {
          while (*replacement && out + 1 < dest_sz) {
            dest[out++] = *replacement++;
          }
          src = end + 1;
          continue;
        }
        // Unknown placeholder (or disallowed in this context); pass through.
      }
    }
    dest[out++] = *src++;
  }
  dest[out] = 0;
}

void MqttBridge::buildTopic(char *dest, size_t dest_sz, const char *suffix) {
  snprintf(dest, dest_sz, "%s/%s", _resolved_topic_prefix, suffix);
}

void MqttBridge::begin() {
  BRIDGE_DEBUG_PRINTLN("MQTT: begin\n");

  if (!_prefs->mqtt_enabled) {
    BRIDGE_DEBUG_PRINTLN("MQTT: disabled in prefs\n");
    return;
  }
  if (_prefs->mqtt_host[0] == 0) {
    BRIDGE_DEBUG_PRINTLN("MQTT: no host configured\n");
    return;
  }

  // Port 0 means "use the protocol default for the current TLS setting":
  //   TLS off -> 1883 (plain MQTT)
  //   TLS on  -> 8883 (mqtts)
  uint16_t resolved_port = _prefs->mqtt_port;
  if (resolved_port == 0) {
    resolved_port = _prefs->mqtt_tls ? 8883 : 1883;
  }

  resolvedClientId(_resolved_client_id, sizeof(_resolved_client_id));
  resolvedTopicPrefix(_resolved_topic_prefix, sizeof(_resolved_topic_prefix));
  buildTopic(_topic_rx,     sizeof(_topic_rx),     "rx");
  buildTopic(_topic_tx,     sizeof(_topic_tx),     "tx");
  buildTopic(_topic_status, sizeof(_topic_status), "status");
  if (_prefs->mqtt_subscribe[0]) {
    // Placeholders ({client_id}, {pubkey}) work here too -- useful if you
    // want e.g. "yourcluster/{client_id}/rf" without hard-coding the id.
    expandPlaceholders(_prefs->mqtt_subscribe, _topic_subscribe,
                       sizeof(_topic_subscribe));
  } else {
    buildTopic(_topic_subscribe, sizeof(_topic_subscribe), "rf");
  }

  if (_prefs->mqtt_tls) {
    _tls_client.setInsecure();  // v1: no cert verification, see TODO
    _client.setClient(_tls_client);
  } else {
    _client.setClient(_plain_client);
  }
  _client.setServer(_prefs->mqtt_host, resolved_port);
  _client.setKeepAlive(30);
  _client.setSocketTimeout(15);

  _running = true;
  _initialized = true;
  _next_reconnect_ms = 0;
  _reconnect_backoff_ms = 1000;
}

void MqttBridge::end() {
  BRIDGE_DEBUG_PRINTLN("MQTT: end\n");
  if (_client.connected()) {
    publishStatus("offline");
    _client.disconnect();
  }
  _running = false;
  _initialized = false;
}

bool MqttBridge::tryConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  // Re-resolve the port at connect time so a runtime TLS toggle picks the
  // right default without requiring a fresh begin().
  uint16_t connect_port = _prefs->mqtt_port;
  if (connect_port == 0) {
    connect_port = _prefs->mqtt_tls ? 8883 : 1883;
  }
  BRIDGE_DEBUG_PRINTLN("MQTT: connecting to %s:%d as '%s'\n",
                       _prefs->mqtt_host, (int)connect_port, _resolved_client_id);

  const char *user = _prefs->mqtt_user[0] ? _prefs->mqtt_user : nullptr;
  const char *pass = _prefs->mqtt_password[0] ? _prefs->mqtt_password : nullptr;

  // Last Will: when we drop, retained "offline" appears on the status topic.
  bool ok = _client.connect(
      _resolved_client_id,
      user, pass,
      _topic_status, 0, true, "offline");

  if (!ok) {
    _last_rcd_state = _client.state();
    _last_err_ms = millis();
    BRIDGE_DEBUG_PRINTLN("MQTT: connect failed, state=%d\n", (int)_last_rcd_state);
    return false;
  }

  publishStatus("online");
  _client.subscribe(_topic_subscribe, 0);
  BRIDGE_DEBUG_PRINTLN("MQTT: connected, subscribed %s\n", _topic_subscribe);
  _reconnect_backoff_ms = 1000;
  return true;
}

void MqttBridge::publishStatus(const char *state) {
  if (_client.connected()) {
    _client.publish(_topic_status, (const uint8_t *)state, strlen(state), true);
  }
}

void MqttBridge::loop() {
  if (!_running) return;

  if (!_client.connected()) {
    uint32_t now = millis();
    if (now >= _next_reconnect_ms) {
      if (!tryConnect()) {
        _next_reconnect_ms = now + _reconnect_backoff_ms;
        _reconnect_backoff_ms = (_reconnect_backoff_ms < 30000)
                                  ? (_reconnect_backoff_ms * 2)
                                  : 30000;
      }
    }
    return;
  }

  _client.loop();
}

// Legacy AbstractBridge entrypoint. Not used directly by MyMesh anymore --
// MyMesh calls publishRx() / publishTx() explicitly per-event. Kept as a
// no-op so the interface is satisfied if someone wires it up.
void MqttBridge::sendPacket(mesh::Packet *packet) {
  (void)packet;
}

static void buildHeader(uint8_t *out, int8_t rssi, int8_t snr_x4) {
  uint32_t up = millis();
  out[0] = MqttBridge::DOWN_HEADER_VER;
  out[1] = (uint8_t)(up & 0xFF);
  out[2] = (uint8_t)((up >> 8) & 0xFF);
  out[3] = (uint8_t)((up >> 16) & 0xFF);
  out[4] = (uint8_t)((up >> 24) & 0xFF);
  out[5] = (uint8_t)rssi;
  out[6] = (uint8_t)snr_x4;
}

void MqttBridge::publishRx(mesh::Packet *packet) {
  if (!_running || !packet || !_client.connected()) return;
  if (!_prefs->mqtt_publish_rx) return;

  uint8_t pkt_buf[MAX_MESH_PACKET];
  int pkt_len = packet->writeTo(pkt_buf);
  if (pkt_len <= 0 || pkt_len > (int)MAX_MESH_PACKET) return;

  uint8_t out[DOWN_HEADER_LEN + MAX_MESH_PACKET];
  buildHeader(out, _last_rssi, _last_snr_x4);
  memcpy(out + DOWN_HEADER_LEN, pkt_buf, pkt_len);
  if (_client.publish(_topic_rx, out, DOWN_HEADER_LEN + pkt_len, false)) {
    _msgs_pub++;
  }
}

void MqttBridge::publishTx(mesh::Packet *packet) {
  if (!_running || !packet || !_client.connected()) return;
  if (!_prefs->mqtt_publish_tx) return;

  uint8_t pkt_buf[MAX_MESH_PACKET];
  int pkt_len = packet->writeTo(pkt_buf);
  if (pkt_len <= 0 || pkt_len > (int)MAX_MESH_PACKET) return;

  uint8_t out[DOWN_HEADER_LEN + MAX_MESH_PACKET];
  // TX-flavor publishes carry stale RSSI/SNR (from last RX); the daemon
  // should treat those fields as informational only for /tx topic.
  buildHeader(out, _last_rssi, _last_snr_x4);
  memcpy(out + DOWN_HEADER_LEN, pkt_buf, pkt_len);
  if (_client.publish(_topic_tx, out, DOWN_HEADER_LEN + pkt_len, false)) {
    _msgs_pub++;
  }
}

void MqttBridge::onMqttMessage(const char *topic, const uint8_t *payload, unsigned int length) {
  // Self-filter: if a wildcard subscribe pattern matches our own /rx or /tx
  // topic, the broker will route our own publishes back to us. Drop those.
  if (strcmp(topic, _topic_rx) == 0 || strcmp(topic, _topic_tx) == 0) {
    return;
  }

  // Topics ending in "/rx" or "/tx" carry the fixed publish header before the
  // packet bytes. Strip it. Any other topic (e.g. our /rf or whatever the user
  // pointed mqtt.subscribe at) is treated as raw mesh packet bytes.
  const uint8_t *pkt_bytes = payload;
  unsigned int pkt_len = length;
  size_t topic_len = strlen(topic);
  bool has_header = (topic_len >= 3 &&
                     (strcmp(topic + topic_len - 3, "/rx") == 0 ||
                      strcmp(topic + topic_len - 3, "/tx") == 0));
  if (has_header) {
    if (length < DOWN_HEADER_LEN) {
      BRIDGE_DEBUG_PRINTLN("MQTT: rx short header on %s\n", topic);
      return;
    }
    pkt_bytes += DOWN_HEADER_LEN;
    pkt_len -= DOWN_HEADER_LEN;
  }

  if (pkt_len == 0 || pkt_len > MAX_MESH_PACKET) {
    BRIDGE_DEBUG_PRINTLN("MQTT: rx invalid pkt_len=%u on %s\n", pkt_len, topic);
    return;
  }

  BRIDGE_DEBUG_PRINTLN("MQTT: inject %u bytes from %s\n", pkt_len, topic);
  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) {
    BRIDGE_DEBUG_PRINTLN("MQTT: alloc failed\n");
    return;
  }
  if (pkt->readFrom(pkt_bytes, pkt_len)) {
    _msgs_sub++;
    onPacketReceived(pkt);
  } else {
    BRIDGE_DEBUG_PRINTLN("MQTT: packet parse failed\n");
    _mgr->free(pkt);
  }
}

void MqttBridge::onPacketReceived(mesh::Packet *packet) {
  // Pre-seed _seen_packets so the next RF echo of this packet isn't re-published.
  // BridgeBase::handleReceivedPacket() will dedupe via hasSeen() and queue if novel.
  handleReceivedPacket(packet);
}

void MqttBridge::describeStatus(char *reply) {
  if (!_prefs->mqtt_enabled) {
    strcpy(reply, "disabled");
    return;
  }
  if (!_running) {
    strcpy(reply, "stopped");
    return;
  }
  if (_client.connected()) {
    sprintf(reply, "connected pub=%u sub=%u rx=%s tx=%s subs=%s",
            (unsigned)_msgs_pub, (unsigned)_msgs_sub,
            _prefs->mqtt_publish_rx ? "on" : "off",
            _prefs->mqtt_publish_tx ? "on" : "off",
            _topic_subscribe);
  } else {
    int st = _client.state();
    sprintf(reply, "disconnected state=%d (backoff=%ums)",
            st, (unsigned)_reconnect_backoff_ms);
  }
}

#endif // WITH_MQTT_BRIDGE
