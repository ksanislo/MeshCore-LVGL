# Companion protocol over TCP (WiFi mode) — implemented behind a toggle

Status: **shipped behind an opt-in toggle** (default off). Written 2026-06.

## What it is
MeshCore abstracts the companion frame protocol (what the phone/desktop apps speak) behind
`BaseSerialInterface`; the mesh loop polls `checkRecvFrame()`/`writeFrame()` on whichever single
interface `the_mesh.startInterface()` was given — transport-agnostic. Implementations: BLE
(`SerialBLEInterface`), USB/UART (`ArduinoSerialInterface`), and **TCP** (`SerialWifiInterface`,
`src/helpers/esp32/SerialWifiInterface.{h,cpp}`): a single-client `WiFiServer` on **port 5000**
(`TCP_PORT`) that frames over the socket with a 3-byte `{type,length}` header.

Upstream exposes TCP only via a compile-time `WIFI_SSID` build that *replaces* BLE and uses a static
SSID (`main.cpp` `#elif defined(WIFI_SSID)`). That doesn't fit our model (runtime WiFi for
MQTT/NTP/OTA, BLE for pairing). Our build (`BLE_PIN_CODE` + `WITH_WIFI`) drops the companion onto
**USB serial** while in WiFi mode — nothing on the network.

## What we did
Added a runtime opt-in so that, **in WiFi mode**, the companion protocol can ride TCP on the LAN
instead of USB. `SerialWifiInterface::begin(port)` only starts the server (WiFi is managed elsewhere),
so it slots straight onto our runtime WiFi.

- **`NodePrefs.tcp_companion`** (tail-appended, offset 205, default 0) — DataStore read/write +
  `applyAppendedPrefsDefaults`.
- **`examples/companion_radio/main.cpp`** — in the `WITH_WIFI` + `wifi_enabled` branch: if
  `tcp_companion`, `WiFi.mode(WIFI_STA)` (init netif so the server can bind before `wifiLoop()`
  connects) → `tcp_companion.begin(TCP_PORT)` → `startInterface(tcp_companion)`. Else the existing
  USB-serial path. The TCP path leaves **USB Serial free for `[REL]`/`[OTA]` diagnostics** (no
  `g_dbg_serial=false`). Boot-time transport choice → reboot to apply.
- **UI** — an "App over WiFi (TCP)" switch near the top of the WiFi Settings page (under the WiFi
  status), with a one-line note. `set_tcp_companion_cb` sets the pref + `pushPrefs()` + "Reboot to
  apply" toast; `populateSettings` reflects it.

## Notes / limits
- **One transport at a time** — the mesh holds a single `_serial`. WiFi mode is BLE-off, so this is
  TCP *instead of* USB; no BLE+TCP. Single client at a time (a new connection kicks the old, like BLE).
- **Open access** — it's a plaintext, unauthenticated socket; anyone who can reach port 5000 on the
  LAN can use the companion protocol. The UI note states this plainly; it's opt-in and default off.
- **Clients** — `meshcore-cli` (python) and the web client can connect over TCP to `<device-ip>:5000`.
- Code (`SerialWifiInterface.cpp`) already compiled into all 7 LVGL binaries before this change
  (built + dead-stripped); we just instantiate it now.

## Possible follow-ups (deferred)
- A lightweight access gate (shared token / reuse the BLE PIN) if open-LAN access proves too loose.
- Surface the device IP:port on the WiFi page once connected, so users know where to point the app.
