#pragma once
// Thin shim around src/helpers/bridges/MqttBridge for the companion app.
//
// Why a shim: MqttBridge/BridgeBase are hard-wired to the CommonCLI `NodePrefs`
// (BridgeBase.h includes helpers/CommonCLI.h). The companion has its OWN global
// `NodePrefs` (examples/companion_radio/NodePrefs.h), so a translation unit can't
// include both. This shim's .cpp is the ONLY companion TU that pulls in the bridge
// (and thus CommonCLI's NodePrefs); it owns a private CommonCLI-NodePrefs that the
// companion feeds via setConfig() with plain values. Everything else in the
// companion talks to MQTT only through these plain functions -- no NodePrefs leak.
//
// All entry points are no-ops unless built with WITH_MQTT_BRIDGE (this file is only
// added to the build on the WiFi/MQTT env).

#include <stdint.h>
#include <stddef.h>

namespace mesh { class Packet; class PacketManager; class RTCClock; }

namespace cmqtt {

// Create the bridge instance (once). pubkey seeds the default client-id.
void init(mesh::PacketManager* mgr, mesh::RTCClock* rtc, const uint8_t* pubkey, size_t pklen);

// Push the companion's current settings into the bridge's private config.
void setConfig(uint8_t mqtt_enabled, const char* host, uint16_t port,
               const char* user, const char* pass, const char* topic_prefix,
               uint8_t tls, uint8_t publish_rx, uint8_t publish_tx);

void begin();                 // (re)start per current config (end() first if running)
void end();
void loop();                  // poll connection + pump MQTT (non-blocking)
void publishRx(mesh::Packet* p, int8_t rssi, int8_t snr_x4);
void publishTx(mesh::Packet* p);
void publishSelfAdvert(const uint8_t* bytes, size_t len);
void status(char* out, size_t cap);   // human-readable status line for the UI

}  // namespace cmqtt
