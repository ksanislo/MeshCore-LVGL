#include "CompanionMqtt.h"

#ifdef WITH_MQTT_BRIDGE

#include <string.h>
#include "helpers/bridges/MqttBridge.h"   // pulls in CommonCLI's NodePrefs -- THIS TU ONLY

// Private CommonCLI-NodePrefs used solely to feed the bridge. Zero-initialized so
// the fields we don't expose (mqtt_client_id, mqtt_subscribe, bridge_delay) default
// to empty/0 (auto client-id, default "<prefix>/rf" subscribe, no inject delay).
static NodePrefs    s_prefs;
static MqttBridge*  s_bridge = nullptr;

namespace cmqtt {

void init(mesh::PacketManager* mgr, mesh::RTCClock* rtc, const uint8_t* pubkey, size_t pklen) {
  if (s_bridge) return;
  memset(&s_prefs, 0, sizeof(s_prefs));
  s_prefs.mqtt_publish_rx = 1;
  s_bridge = new MqttBridge(&s_prefs, mgr, rtc);
  if (pubkey) s_bridge->setLocalPubkey(pubkey, pklen);
}

void setConfig(uint8_t mqtt_enabled, const char* host, uint16_t port,
               const char* user, const char* pass, const char* topic_prefix,
               uint8_t tls, uint8_t publish_rx, uint8_t publish_tx) {
  s_prefs.mqtt_enabled = mqtt_enabled;
  s_prefs.mqtt_port    = port;
  s_prefs.mqtt_tls     = tls;
  s_prefs.mqtt_publish_rx = publish_rx;
  s_prefs.mqtt_publish_tx = publish_tx;
  auto cp = [](char* d, size_t cap, const char* s) { strncpy(d, s ? s : "", cap - 1); d[cap - 1] = 0; };
  cp(s_prefs.mqtt_host,         sizeof(s_prefs.mqtt_host),         host);
  cp(s_prefs.mqtt_user,         sizeof(s_prefs.mqtt_user),         user);
  cp(s_prefs.mqtt_password,     sizeof(s_prefs.mqtt_password),     pass);
  cp(s_prefs.mqtt_topic_prefix, sizeof(s_prefs.mqtt_topic_prefix), topic_prefix);
}

void begin() {
  if (!s_bridge) return;
  if (s_bridge->isRunning()) s_bridge->end();
  if (s_prefs.mqtt_enabled) s_bridge->begin();
}
void end()  { if (s_bridge) s_bridge->end(); }
void loop() { if (s_bridge) s_bridge->loop(); }

void publishRx(mesh::Packet* p, int8_t rssi, int8_t snr_x4) {
  if (!s_bridge) return;
  s_bridge->setLastSignal(rssi, snr_x4);
  s_bridge->publishRx(p);
}
void publishTx(mesh::Packet* p)                    { if (s_bridge) s_bridge->publishTx(p); }
void publishSelfAdvert(const uint8_t* b, size_t l) { if (s_bridge) s_bridge->publishSelfAdvert(b, l); }

void status(char* out, size_t cap) {
  if (!out || cap == 0) return;
  if (s_bridge) s_bridge->describeStatus(out);   // fits in a normal status buffer
  else { strncpy(out, "off", cap - 1); out[cap - 1] = 0; }
}

}  // namespace cmqtt

#endif  // WITH_MQTT_BRIDGE
