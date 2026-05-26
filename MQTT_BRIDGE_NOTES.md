# MeshCore MQTT Bridge — Research Notes

Scratch notes from a design conversation about adding an MQTT-over-IP bridge
to MeshCore, primarily for long-haul links (e.g. Seattle ↔ AZ Mojave via
Starlink) where direct RF is not feasible.

Not a spec, not a plan — a working set of conclusions so we don't have to
re-research the same ground.

## Use case framing

- **Goal:** bridge two distant MeshCore meshes (~1000 mi apart, mountain ranges
  in between) over IP so each site behaves to its local users as if there were
  a persistent neighbor on the other end.
- **Why this can't be RF:** distance + terrain. Even HF skywave is unreliable
  and would abandon what MeshCore is good at (narrow LoRa, low power, simple
  hardware).
- **Why MQTT-over-Starlink is appropriate:** at this distance you were never
  going to have an off-grid link anyway, so the dependency on
  internet/satellite isn't a downgrade — it's the only option. Starlink has
  the "deployable to nowhere" property at the physical layer that MeshCore has
  at the protocol layer, which makes it a philosophically reasonable carrier.
- **Layering perspective:**
  - **L1 (same site / same tower):** ESPNow or RS232. Tens of meters, zero
    infra.
  - **L2 (across town, LOS problems):** LoRa repeater chain or directional
    point-to-point RF link with existing bridges. Still RF-only.
  - **L3 (beyond practical RF):** MQTT-over-IP back-channel. Trades off-grid
    for reachability. This is what we're building.
- **Upstream maintainer view (PR #454, jbrazio):** explicitly chose not to add
  MQTT to the reference bridges because it "relies on external networks or
  internet-based protocols, such as MQTT, that are prone to failure in
  emergency situations." Fair stance for the upstream off-grid mission; our
  use case is a different layer and is not in conflict.

## Existing MeshCore infrastructure we can reuse

### The bridge abstraction (already perfect for this)

- `src/helpers/AbstractBridge.h` — interface with just five methods:
  `begin()`, `end()`, `isRunning()`, `loop()`, `sendPacket(mesh::Packet*)`,
  `onPacketReceived(mesh::Packet*)`.
- `src/helpers/bridges/BridgeBase.h` — base class with goodies we get for
  free:
  - `SimpleMeshTables _seen_packets` (packet-hash LRU for loop suppression)
  - `fletcher16()` checksum + `validateChecksum()`
  - `handleReceivedPacket()` — canonical "dedupe then queue" pattern
  - `BRIDGE_PACKET_MAGIC = 0xC03E` and standard header sizes
  - Access to `_prefs`, `_mgr` (packet manager), `_rtc`
- Reference implementations to crib from:
  - `src/helpers/bridges/ESPNowBridge.{h,cpp}` — broadcast-style, closest in
    shape to what we want (publish heard, inject received).
  - `src/helpers/bridges/RS232Bridge.{h,cpp}` — point-to-point serial.

### Repeater integration is already wired

In `examples/simple_repeater/MyMesh.{h,cpp}`:

- `extern AbstractBridge* bridge;` under `#ifdef WITH_BRIDGE` (`MyMesh.h:39`)
- `logRx()` and `logTx()` already call `bridge.sendPacket(pkt)` on every TX
  or RX, selectable via `_prefs.bridge_pkt_src` (`MyMesh.cpp:471-502`)
- `setBridgeState(bool)` and `restartBridge()` callbacks
  (`MyMesh.h:230,242`)
- `hasPendingWork()` already returns true when bridge is running so the
  device doesn't sleep (`MyMesh.cpp:1306-1309`)

We add `#ifdef WITH_MQTT_BRIDGE` next to the existing two
(`WITH_RS232_BRIDGE`, `WITH_ESPNOW_BRIDGE`) and drop in our bridge class.
No changes to mesh core.

### Bridge config keys are already persisted

`src/helpers/CommonCLI.h:46-51` defines:

```
bridge_enabled, bridge_delay, bridge_pkt_src, bridge_baud,
bridge_channel, bridge_secret[16]
```

with full load/save/CLI plumbing in `CommonCLI.cpp` (persistence at lines
76-172, `set bridge.*` CLI at 668-710, `get` at 828-842). Pattern to copy
for adding `mqtt_*` keys.

### WiFi support

- WiFi-station code is in `examples/companion_radio/main.cpp:196-199` and the
  WiFi-flavored variants (e.g. `heltec_v4_companion_radio_wifi`). The repeater
  app does NOT currently bring up station-mode WiFi.
- Repeater code does have softAP WiFi for OTA in
  `src/helpers/ESP32Board.cpp:5-39` (creates `MeshCore-OTA` hotspot, runs
  AsyncWebServer + AsyncElegantOTA, triggered by admin CLI). Not what we want
  but proves WiFi can come up in the repeater path.
- `src/helpers/esp32/SerialWifiInterface.{h,cpp}` is a working
  WiFiServer/WiFiClient reference for ESP32 socket semantics.

## PR #454 — the reference for bridge design intent

- URL: https://github.com/meshcore-dev/MeshCore/pull/454
- Title: "RS232/ESP-NOW Bridge/cross repeater implementation"
- Author: jbrazio (João Brázio), merged 2025-09-09
- Stated use cases:
  1. Devices on different frequencies/bands
  2. Local community ↔ distant repeater via Yagi + omni
  3. Re-radiate signal in a new direction with two Yagis
- Confirms in the PR body: "All types of traffic appear to be functioning
  properly, including paths, pings, administrative requests and direct
  messaging between clients on different bands." — i.e. bridge-injected
  packets ride the normal mesh RX path correctly for all packet types.
- Same PR explicitly rules MQTT out of the reference implementations for
  off-grid-purity reasons (see Use Case Framing above).

## What's missing — work to add for MQTT support

1. **MQTT client library.** Add to `lib_deps` — `knolleary/PubSubClient` is the
   best style match (synchronous, small, well-tested, fits the "no dynamic
   memory except setup" rule from CONTRIBUTING.md). Alternative:
   `marvinroger/AsyncMqttClient` if we need async, but probably overkill.

2. **TLS for MQTT-over-TLS (port 8883).** `WiFiClientSecure` is in ESP32
   Arduino core but isn't used anywhere in this repo today. ~50 lines for CA
   cert handling. Probably mandatory for any internet-exposed broker.

3. **NodePrefs extension.** Add `mqtt_host[64]`, `mqtt_port`, `mqtt_user[32]`,
   `mqtt_pass[32]`, `mqtt_topic[64]`, `mqtt_client_id[24]`, `mqtt_tls` (bool).
   Mirror the persistence + CLI patterns from `bridge_*` keys in
   `CommonCLI.{h,cpp}`. Consider a struct version bump if layout matters.

4. **The bridge class itself.** `MqttBridge : public BridgeBase`. ~200-400
   lines following the `ESPNowBridge` template:
   - PubSubClient member
   - Reconnect state machine in `loop()` with backoff
   - MQTT callback that calls `BridgeBase::handleReceivedPacket()` (dedup
     handled by base)
   - `sendPacket()` publishes raw packet bytes (binary payload, no base64)
   - Should retain=false on publishes (retained mesh packets would replay
     ancient state to new subscribers — foot-gun)
   - Unique client ID per device (derive from MAC or stored UUID); duplicate
     client IDs cause MQTT broker to kick connections in a loop

5. **WiFi lifecycle in the repeater app.** Currently the repeater doesn't do
   `WiFi.begin(SSID, PWD)` anywhere. Bring it up in `setBridgeState(true)`,
   tear down in `setBridgeState(false)`. Use `board.setInhibitSleep(true)`
   (already used in `companion_radio/main.cpp:197`). Need reconnect logic for
   inevitable AP drops / Starlink handoffs.

6. **New build variant.** E.g. `heltec_v4_repeater_bridge_mqtt`. Clone
   `heltec_v4_repeater_bridge_espnow` from `variants/heltec_v4/platformio.ini`
   (line 105) and swap the bridge define. Note: WiFi station mode and ESPNow
   share the WiFi chip — they can coexist but it's not free; don't enable both
   bridges in one build unless we've tested it.

7. **WiFi credential UX.** Today `WIFI_SSID`/`WIFI_PWD` are compile-time
   defines. For a flashed binary you'd want either: runtime CLI config
   (`set wifi.ssid`, `set wifi.pwd`), captive-portal provisioning, or just
   keep compile-time for now if we control the deployment. Compile-time is
   the v1 default; runtime config is the v2 polish.

## Design decisions to settle

- **Publish on RX vs TX vs both.**
  - RX-only: clean, but misses locally-originated packets (own adverts,
    admin responses) since those never RX.
  - TX-only: includes locally-originated, but causes one-round-trip echo
    when bridge-injected packets are re-transmitted locally and the TX hook
    re-publishes (dies at remote dedup, harmless but ugly).
  - Both, with a "bridge-injected, don't re-publish" flag pre-seeded into
    `_seen_packets` at inject time: cleanest.
  - **Recommendation:** start with publish-on-both, pre-seed seen_packets
    at inject. ESPNowBridge does substantially the same thing.

- **What packet types to bridge.**
  - Adverts: yes — propagates discovery cross-site. Caveat: distant
    repeaters appear in local users' contact lists looking RF-local. Users
    will eventually notice. Plan: tag bridge-injected packets with a flag
    that clients can use to render a "(via internet)" badge later.
  - DMs / group messages: yes, this is the whole point. Payload is already
    end-to-end encrypted; broker sees ciphertext.
  - Admin commands / OTA / region-map sync: probably no by default. Cross
    administrative boundary; opt-in only.
  - Path / trace packets: yes; they're how the mesh discovers routes.
  - **Recommendation:** publish payload types {ADVERT, TXT_MSG, GRP_TXT,
    GRP_DATA, ACK, REQ, RESPONSE, PATH, TRACE}; skip {REQ when payload
    matches admin opcodes}.

- **Hop count semantics.**
  - IP leg currently costs zero hops (packet's `path_length` field doesn't
    increment when going through a bridge).
  - This means a 3-hop packet at one site arrives at the other with 3 hops
    still spent — generous, but possibly "free teleport" depending on view.
  - Option: have the bridge consume one hop on egress, so total topology
    behaves like "one extra long hop."
  - **Recommendation:** make it configurable (`mqtt_egress_hops`,
    default 1). Document the tradeoff.

- **Topic structure.**
  - Two-site bootstrap: one topic per direction or one shared topic with
    a site-id tag in publishes. Shared topic is simpler.
  - Plan for growth: include a site identifier in the topic
    (`meshcore/<deployment>/<site>/packets`) so a third site can join
    without restructuring.
  - Don't conflate with MeshCore's region system (see Regions note below).
    Regions are a separate, optional, mostly-unused feature; don't make
    the bridge depend on them.

- **Rate limiting / airtime budget.**
  - A chatty remote site can flood local RF via the bridge. Need a token
    bucket per bridge on inject.
  - Probably also a per-bridge cap on bytes/sec published, so a misbehaving
    local mesh can't DoS the broker.

## Things to NOT do

- Don't use MQTT retained messages for mesh packets. New subscribers would
  replay ancient packets onto the air.
- Don't depend on regions/transport-codes (see correction note below).
- Don't try to add the MQTT bridge to upstream `simple_repeater` as a default
  — keep it behind `#ifdef WITH_MQTT_BRIDGE` and as a separate build variant.
  PR #454 made the upstream stance clear; this is for our deployments, not a
  generic feature.
- Don't bridge regulatory-region-mismatched traffic blindly if you care about
  RF regulations (e.g. EU 869 traffic re-transmitted on US 915). Add a
  per-bridge allowlist for which RF-region traffic is acceptable to emit.

## Correction: Regions are not what they sounded like

We initially thought MeshCore's "region" system (`src/helpers/RegionMap.{h,cpp}`,
`src/helpers/TransportKeyStore.{h,cpp}`) would do most of the topic-routing /
zoning work for free. After actually tracing the code:

- **Regions are off by default.** No `DEFAULT_FLOOD_SCOPE_NAME` is defined in
  any shipped `platformio.ini`. Default `default_scope.key` is all-zeros,
  `sendFloodScoped(default_scope, ...)` falls through to plain `sendFlood()`
  with no transport codes. Normal MeshCore traffic is unscoped
  `ROUTE_TYPE_FLOOD`.
- **Region "keys" are not preshared secrets.** They're literally
  `SHA256(region_name)` — see `TransportKeyStore::getAutoKeyFor()` at
  `TransportKeyStore.cpp:37-50`. Comments call these "publicly-known hashtag
  region names." Anyone who knows the name has the key. Privacy is namespace
  obscurity, not crypto.
- **There's a `// TODO: retrieve from difficult-to-copy keystore` comment**
  hinting at future private-region-key support, but that's not implemented.
- **Real-world usage** seems to be repeater operators in crowded multi-region
  meshes opting in via CLI (`region put`, `region allowf`, `region denyf`) to
  cut cross-region flood load. Not a user-facing crypto feature.
- **Practical implication for the bridge:** if we want to use regions for
  topic labeling, fine — but treat them as optional labels, not as a
  foundational protection mechanism. Most traffic will be unscoped. The
  bridge's own packet-hash LRU is what actually keeps things sane.

Payload-level encryption (DM keys, group channel keys) is entirely separate
from regions and works regardless. End-to-end privacy of DM content over an
MQTT bridge is preserved because the payload is already encrypted at a
different layer.

## Open questions

- Does MeshCore's mesh layer support multiple active `Bridge` instances on one
  device? E.g. a rooftop node running both ESPNow (to same-site neighbor) and
  MQTT (to remote site). The abstraction allows it but `MyMesh` currently
  has one `bridge` member, not a vector. Probably needs a small refactor or
  a "composite bridge" pattern.
- What's the right place for a "this packet came from the IP bridge" tag?
  A bit in the `Packet` struct? A side-table keyed by packet hash? The
  former touches mesh core; the latter is bridge-local.
- BLE + WiFi + LoRa concurrency on ESP32-S3: officially supported; how much
  do they actually contend in practice? Need bench testing.
- Starlink RTT (~30-50ms one-way + broker hop) — any ACK timeouts in the
  protocol that might trip on this? Doesn't look like it from the code but
  worth confirming under load.

## Reference: key files

```
src/helpers/AbstractBridge.h          — bridge interface
src/helpers/bridges/BridgeBase.{h,cpp}— shared base with seen-packets, etc
src/helpers/bridges/ESPNowBridge.{h,cpp} — broadcast bridge ref impl
src/helpers/bridges/RS232Bridge.{h,cpp}  — serial bridge ref impl
src/helpers/CommonCLI.{h,cpp}         — NodePrefs struct + CLI handlers
src/helpers/ESP32Board.cpp            — softAP OTA, the only WiFi in repeater
src/helpers/esp32/SerialWifiInterface.{h,cpp} — WiFiServer/Client reference
src/helpers/RegionMap.{h,cpp}         — region tree + transport codes
src/helpers/TransportKeyStore.{h,cpp} — SHA256-of-name "keys"
examples/simple_repeater/MyMesh.{h,cpp} — bridge integration points
examples/companion_radio/main.cpp:196 — how WiFi station mode is brought up
variants/heltec_v4/platformio.ini     — variant defines; copy the ESPNow env
docs/packet_format.md                  — header bits, transport codes
docs/kiss_modem_protocol.md           — KISS mode (for reference, not used here)
docs/cli_commands.md                  — bridge.* CLI, region commands
```

## Reference: PR/commit pointers

- `5b9d11ac` — Initial ESPNow bridge + improved docs (jbrazio, 2025-09-07)
- `1c93c162` — Rolled out ESPNow bridge envs to all ESP32 variants
- `1948d284` — Extracted AbstractBridge from inline serial-bridge code
- `0051ccef` — Refactor bridges to inherit from BridgeBase
- `8edcb46a` — Bridge: enhance CLI configuration options
- `ea33f395` — Merge PR #454 — full RS232 + ESPNow bridge implementation

## Daemon-side filtering design (RV / master+slave NAT bridge)

Concrete deployment this design targets:

- **Master bridge**: at home, full participant in main mesh (e.g. Seattle).
- **Slave bridge**: in motorhome, short-range LoRa, follows the rig wherever
  it's parked. Frequently parked near unrelated meshes (e.g. Cascadia).
- **Client device**: phone-paired MeshCore companion radio.
- **Daemon**: runs somewhere with reliable connectivity (home server, VPS),
  subscribed to both bridges' MQTT topics, implements all routing/NAT policy.

Goal: client in the RV reaches Seattle through the bridges; **no traffic from
any local mesh that happens to be near the RV** can propagate into Seattle,
and **no Seattle traffic leaks into the local mesh** the RV is parked near.

### Architecture

- Firmware stays the dumb pub/sub shovel (no policy logic).
- All filtering / NAT / path mutation happens in the daemon.
- Bridges have separate `up` / `down` topics per [[topic-naming]] (see earlier
  in this doc).
- Daemon maintains per-deployment state; no firmware reconfig to change rules.

### Slave → main (upstream) filter chain

Strict admission control. In order:

1. **Hop count check**: `hop_count == 0` only. Drop anything that was already
   forwarded by another repeater on the slave side. Naturally bounds the
   "DMZ scope" to whatever the slave bridge can hear directly.
2. **Dedup**: same `(payload_type, payload)` hash we use in the sniffer; matches
   firmware's `SimpleMeshTables.hasSeen()` semantics so multi-acks etc. survive.
3. **Per-payload-type policy**:
   - `ADVERT`: verify the embedded full pubkey is in source-allowlist. Update
     daemon's `src_hash → pubkey` table from this advert. Forward.
   - `REQ` / `RESPONSE` / `TXT_MSG` / `PATH`: look up `src_hash` in the table;
     if it maps to an allowlisted pubkey, forward. Otherwise drop.
   - `ACK` / `MULTIPART (ack-style)`: forward unconditionally OR check against
     a `recent_acks_we_expect: dict[crc, ts_expiry]` table populated when we
     forward a REQ from an allowlisted source. Either is safe; the latter is
     tighter.
   - `GRP_TXT` / `GRP_DATA`: **drop by default** — no identity in cleartext.
     See "Public channel cross-contamination" below.
   - `ANON_REQ`: drop. By design carries only an ephemeral pubkey.
   - `CONTROL`: drop. Slave-side admin/discovery must not leak to main.
   - `TRACE`: drop. Don't expose slave-side topology to main.
   - `RAW_CUSTOM`: drop. No standard identity field.

### Main → slave (downstream)

Wide open by default — slave-side users (your client) see all of main mesh —
but emit on the slave RF with **fake-padded path** and **`hop_count` near
`flood_max`** so neighbouring meshes can hear but won't re-flood.

- Path padding makes the contact appear at a realistic "distance" in the
  client's UI (avoids "Seattle contact looks like a direct neighbor").
- Saturated hop count prevents any nearby non-RV repeater from propagating
  Seattle traffic deeper into whatever local mesh the RV is parked near.
- Optional: drop `CONTROL` and admin `REQ` / `RESPONSE` in this direction so
  Seattle's admin traffic doesn't air-out on the motorhome's RF.

### Source allowlist mechanics

Daemon config stores a set of trusted pubkey hex strings. Minimum useful set:
- Your client device's pubkey
- The slave bridge's own pubkey (if `publish_tx` is on and you want the bridge
  visible on main)

Hash table is built dynamically from observed allowlisted adverts:

```
known_pubkeys: dict[hash, pubkey] = {}

on ADVERT (post-allowlist):
    known_pubkeys[short_hash(pubkey)] = pubkey
```

Pre-seed the table at config time if you want zero-warmup operation (no
discovery gap before the client's first advert is heard).

### Identity coverage per payload type

| Type | Identity available | Filter approach |
|------|-------------------|-----------------|
| ADVERT | full pubkey + signature | direct allowlist check, optionally verify sig |
| REQ / RESPONSE / TXT_MSG / PATH | 1-byte src_hash truncation | hash→pubkey table built from allowlisted adverts |
| ACK / MULTIPART(ack) | none (just 4-byte ack_crc) | forward, or correlate to outstanding REQs we forwarded |
| GRP_TXT / GRP_DATA | channel_hash only; sender is inside encrypted payload | drop, or channel_hash allowlist (collision-prone) |
| ANON_REQ | ephemeral pubkey, by design anonymous | drop |
| CONTROL | unencrypted, subtype-dependent | drop in slave→main |
| TRACE | per-hop diagnostic | drop in slave→main |
| RAW_CUSTOM | app-defined | drop unless explicit allowlist |

Hash-collision risk for the hash-only types: with 1-byte hashes there's a
1/256 chance a random foreign node's pubkey hashes to the same byte. The
hop_count==0 filter narrows the universe to direct RF neighbors, so in
practice this is rare and bounded. With `path_hash_mode = 2` (3-byte hashes)
it becomes negligible.

### Workflow for adding a new client device

1. On the new client, run `get public.key` from CLI / get from the app.
2. Add the hex pubkey to the daemon's allowlist config.
3. Restart daemon (or send SIGHUP).
4. Client's next advert populates the hash table; from then on its traffic
   bridges.

### What this gets you concretely

Park next to a Cascadia mesh segment in Tacoma:
- Your client → slave → daemon → main → Seattle: DMs work. ✓
- Cascadia repeater 50m away → slave (hop=0, pubkey not allowlisted) →
  daemon drops. Nothing leaks to Seattle. ✓
- Seattle → main → daemon → slave → RF (fake-padded, hop saturated). Cascadia
  repeater hears, refuses to forward. Nothing leaks into Cascadia. ✓

Two-way isolation, all policy in a single user-space daemon, no firmware
changes from the dumb-shovel base.

## Public channel cross-contamination

The default "Public" channel that ships pre-configured in every companion
firmware uses a globally-known hardcoded PSK
(`PUBLIC_GROUP_PSK = "izOH6cXN6mrJ5e26oRXNcg=="`, see
`examples/companion_radio/MyMesh.cpp:108,951`). The channel hash is just
`SHA256(PSK)`, so:

- **Every MeshCore device, in every mesh, has the same Public channel_hash.**
  Seattle's Public hash == Cascadia's Public hash == every other deployment's.
- Wire payload is AES-encrypted with the PSK, but the PSK is in every firmware
  binary — no scope-limiting, no real protection against decryption.
- `Packet.h` flags GRP_TXT as "(unverified)" — sender identity is just a
  freeform name string inside the encrypted payload, with no signature or
  pubkey binding. Anyone can post as anyone.

Consequences for the bridge:

- **The Public channel cannot be cleanly bridged.** There's no protocol-level
  signal that distinguishes "our user's Public post" from "any random other
  mesh member's Public post." Forwarding any Public traffic forwards all of it.
- **Decrypting at the daemon doesn't help.** The "name" field after
  decryption is spoofable.
- **The right answer is "don't bridge Public at all."** Drop GRP_TXT/GRP_DATA
  on slave→main unconditionally.

For cross-mesh group chat, use **private channels** with unique PSKs:

- User creates a new channel with a non-default PSK on their client device.
- The PSK is shared out-of-band with intended participants (e.g. Seattle
  contacts they care about).
- Daemon's channel-hash allowlist includes only the private channel's hash.
- Cascadia members don't have that PSK and don't know the hash, so their
  Public traffic gets dropped at the channel_hash mismatch.
- The private channel's traffic crosses the bridge cleanly because its
  channel_hash uniquely identifies it.

Net behavior:
- Public works locally on either side of the bridge (it's a per-mesh local
  chatter channel by nature).
- Cross-mesh participation requires a private channel — by design, not by
  bridge limitation.
