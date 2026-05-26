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
  _topic_up[0] = 0;
  _topic_down[0] = 0;
  _topic_status[0] = 0;
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
    strncpy(dest, _prefs->mqtt_client_id, dest_sz - 1);
    dest[dest_sz - 1] = 0;
  } else if (_pubkey_short_hex[0]) {
    snprintf(dest, dest_sz, "meshcore-%s", _pubkey_short_hex);
  } else {
    strncpy(dest, "meshcore-bridge", dest_sz - 1);
    dest[dest_sz - 1] = 0;
  }
}

void MqttBridge::resolvedTopicPrefix(char *dest, size_t dest_sz) {
  if (_prefs->mqtt_topic_prefix[0]) {
    strncpy(dest, _prefs->mqtt_topic_prefix, dest_sz - 1);
    dest[dest_sz - 1] = 0;
  } else {
    char cid[24];
    resolvedClientId(cid, sizeof(cid));
    snprintf(dest, dest_sz, "meshcore/%s", cid);
  }
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
  if (_prefs->mqtt_host[0] == 0 || _prefs->mqtt_port == 0) {
    BRIDGE_DEBUG_PRINTLN("MQTT: no host/port configured\n");
    return;
  }

  resolvedClientId(_resolved_client_id, sizeof(_resolved_client_id));
  resolvedTopicPrefix(_resolved_topic_prefix, sizeof(_resolved_topic_prefix));
  buildTopic(_topic_up,     sizeof(_topic_up),     "up");
  buildTopic(_topic_down,   sizeof(_topic_down),   "down");
  buildTopic(_topic_status, sizeof(_topic_status), "status");

  if (_prefs->mqtt_tls) {
    _tls_client.setInsecure();  // v1: no cert verification, see TODO
    _client.setClient(_tls_client);
  } else {
    _client.setClient(_plain_client);
  }
  _client.setServer(_prefs->mqtt_host, _prefs->mqtt_port);
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
  BRIDGE_DEBUG_PRINTLN("MQTT: connecting to %s:%d as '%s'\n",
                       _prefs->mqtt_host, (int)_prefs->mqtt_port, _resolved_client_id);

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
  _client.subscribe(_topic_up, 0);
  BRIDGE_DEBUG_PRINTLN("MQTT: connected, subscribed %s\n", _topic_up);
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

void MqttBridge::sendPacket(mesh::Packet *packet) {
  if (!_running || !packet || !_client.connected()) {
    return;
  }
  if (_seen_packets.hasSeen(packet)) {
    // We already injected this from MQTT — don't republish it.
    return;
  }

  uint8_t pkt_buf[MAX_MESH_PACKET];
  int pkt_len = packet->writeTo(pkt_buf);
  if (pkt_len <= 0 || pkt_len > (int)MAX_MESH_PACKET) {
    BRIDGE_DEBUG_PRINTLN("MQTT: bad packet len=%d\n", pkt_len);
    return;
  }

  uint8_t out[DOWN_HEADER_LEN + MAX_MESH_PACKET];
  uint32_t up = millis();
  out[0] = DOWN_HEADER_VER;
  out[1] = (uint8_t)(up & 0xFF);
  out[2] = (uint8_t)((up >> 8) & 0xFF);
  out[3] = (uint8_t)((up >> 16) & 0xFF);
  out[4] = (uint8_t)((up >> 24) & 0xFF);
  out[5] = (uint8_t)_last_rssi;
  out[6] = (uint8_t)_last_snr_x4;
  memcpy(out + DOWN_HEADER_LEN, pkt_buf, pkt_len);

  if (_client.publish(_topic_down, out, DOWN_HEADER_LEN + pkt_len, false)) {
    _msgs_pub++;
  } else {
    BRIDGE_DEBUG_PRINTLN("MQTT: publish failed\n");
  }
}

void MqttBridge::onPacketTransmitted(mesh::Packet *packet) {
  if (!_prefs->mqtt_publish_tx) return;
  sendPacket(packet);
}

void MqttBridge::onMqttMessage(const char *topic, const uint8_t *payload, unsigned int length) {
  if (length == 0 || length > MAX_MESH_PACKET) {
    BRIDGE_DEBUG_PRINTLN("MQTT: rx invalid len=%u\n", length);
    return;
  }
  BRIDGE_DEBUG_PRINTLN("MQTT: rx %u bytes on %s\n", length, topic);

  mesh::Packet *pkt = _mgr->allocNew();
  if (!pkt) {
    BRIDGE_DEBUG_PRINTLN("MQTT: alloc failed\n");
    return;
  }
  if (pkt->readFrom(payload, length)) {
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
    sprintf(reply, "connected msgs_pub=%u msgs_sub=%u prefix=%s",
            (unsigned)_msgs_pub, (unsigned)_msgs_sub, _resolved_topic_prefix);
  } else {
    int st = _client.state();
    sprintf(reply, "disconnected state=%d (backoff=%ums)",
            st, (unsigned)_reconnect_backoff_ms);
  }
}

#endif // WITH_MQTT_BRIDGE
