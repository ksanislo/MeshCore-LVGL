# Scoping: native MQTT Observer output (CrowPanel/T-Deck LVGL companion)

Status: **scoped, not started.** Written 2026-06 (supersedes the roadmap stub). Additive feature.
Verdict up front: **a tidy ~1–2 day add** — we already capture and MQTT-publish the exact data; the
observer is just a second *output format* (JSON envelope) on top of it. No decode/decrypt on-device,
no new privacy class beyond what the raw `/rx` bridge already does.

---

## What "observer" means (and what it is NOT)
The MeshCore observer ecosystem (observer.gessaman.com map, Cisien/meshcoretomqtt, agessaman tooling)
is **publish-only per-packet telemetry**: "this node heard this packet, with this signal, at this
time." The **map server decodes the raw frame** to extract adverts/positions — so the *node* does NOT
decode or decrypt anything. It's NOT human-readable chat, NOT an observer of message content; it's RF
coverage/sighting data.

Today these run as an **external Python tool on a Pi** that parses a repeater's `MESH_PACKET_LOGGING`
serial output and republishes it to MQTT. The opportunity: emit that JSON **natively from our
firmware** → the CrowPanel/T-Deck becomes a **self-contained observer, no Pi in the loop.**

## The wire format (match Cisien `meshcoretomqtt` exactly)
Topic: **`meshcore/{IATA}/{PUBLIC_KEY}/packets`** (also `/status`, `/debug` for parity).
- `{IATA}` = user-set location code (new config field).
- `{PUBLIC_KEY}` = *our* node's pubkey (the observing node).

Payload: a flat JSON object per heard packet:
```json
{ "origin":"<our node name>", "origin_id":"<our pubkey>", "timestamp":"<ISO8601>",
  "type":"PACKET", "direction":"rx", "time":"HH:MM:SS", "date":"D/M/YYYY",
  "len":"<bytes>", "packet_type":"<n>", "route":"F|D|T|U", "payload_len":"<n>",
  "raw":"<packet hex>", "SNR":"<n>", "RSSI":"<n>", "score":"<n>", "hash":"<hex>" }
```
QoS 0; retained configurable. (Match field names/casing verbatim so it drops into existing maps.)

## Why this is cheap for us — the data already exists
Everything the JSON needs is already in hand at our existing publish point:
- **The tap:** `MyMesh::logRx(pkt,len,score)` → today calls `cmqtt::publishRx(pkt,rssi,snr_x4)` on core 0
  (and `logTx`, `logRxRaw`). We're *already* MQTT-publishing this exact packet as **binary** to `/rx`.
- **The fields:** `mesh::Packet` exposes everything the Cisien builder uses — `getPayloadType()`
  (→ packet_type), `isRouteFlood()`/`isRouteDirect()` (→ route F|D), `getRawLength()`/`payload_len`,
  `writeTo()` (→ raw hex), `calculatePacketHash()` (→ hash); `score` is the logRx param; RSSI/SNR are
  already pulled there; `origin`/`origin_id` = our own name+pubkey; timestamp from the RTC.
- **The plumbing:** PubSubClient is already in the build; WiFi/clock/identity are already managed by the
  companion (the external observers hardwire their own WiFi/NTP/task — we sidestep all of that).

So implementation = **serialize the logRx data to the Cisien JSON and publish to the observer topic.**

## Recommended v1
- **ADDITIVE, not replace** (per the "alongside the raw packets" intent): keep the existing binary
  `/rx`+`/tx` virtual-radio bridge; add observer JSON as a *parallel* output. (The earlier roadmap note
  said "replace the transport bridge" — superseded: run both, gated by separate toggles.)
- **Hook `logRxRaw` (every received copy, pre-dedup), not `logRx`** — observers want every copy for
  coverage/multipath, which is the whole point of a map. (We already have `logRxRaw`.)
- **Hand-rolled JSON** (fixed `snprintf` into a buffer) — it's a flat object; no need to pull cJSON into
  the publish hot path (cJSON is linked but heavier).
- **Config (reuse the client_id/subscribe pattern we just shipped):** append to `NodePrefs` →
  `DataStore` read/write + `applyAppendedPrefsDefaults` → `CompanionMqtt::setConfig`:
  - `mqtt_observer_enabled` (default off)
  - `mqtt_observer_iata` (location code; required for the topic)
  - optionally `observer_publish_rx`/`observer_publish_tx` toggles
  UI: a small "Observer" subsection on the existing MQTT pane (CAT_MQTT) with the toggle + IATA field.
- **Topic build:** `meshcore/<iata>/<pubkey-hex>/packets` — independent of `mqtt_topic_prefix` (the
  observer ecosystem keys off IATA+pubkey, not our prefix).

## Effort
- **v1 (~1–2 days):** JSON serializer for packet+signal, IATA config field + topic, the observer toggle,
  wire it off `logRxRaw`/`logTx`. Mostly formatting + the (now-routine) NodePrefs/UI plumbing.
- **Phase 2 (deferred):** "Let's Mesh Analyzer" JWT auth (Ed25519 / JWTHelper, owner/email, US/EU broker)
  if we want to feed the authenticated analyzer endpoints rather than a plain broker.

## Privacy / notice
Same exposure class as the **raw `/rx` bridge**: we publish *heard RF frames* (opaque encrypted blobs +
public adverts) + signal to the broker; the server only decodes the *public* adverts. So **no new
plaintext leak** vs. what `/rx` already does — but it IS contributing your heard traffic to a
(often public) map, so it stays **opt-in** with a clear one-liner ("publishes heard radio traffic +
signal to the observer broker for coverage mapping; development/diagnostic"). Reuses/extends the MQTT-page
notice we were drafting.

## Decisions to confirm before coding
1. **Additive vs. replace** — recommend additive (run observer + raw bridge independently). ✅ matches intent.
2. **Exact topic + field casing** — lock to Cisien so it drops into observer.gessaman.com unchanged.
3. **`logRxRaw` vs `logRx`** — recommend `logRxRaw` (every copy) for true coverage data.
4. **IATA** — required field; block enabling the observer with a blank IATA (and guard wildcards like we
   did for prefix/client-id).

(Cross-links: shares packet metadata with [[radio-diagnostics]]; builds on [[project-wifi-mqtt]] +
the client_id/subscribe config pattern just shipped. Roadmap index: [[project-crowpanel-roadmap]].)
