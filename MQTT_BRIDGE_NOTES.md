# MQTT Bridge — Forward-looking Design Notes

This file documents design work for **features not yet built**. Current
state of what IS built lives in `CLAUDE.md`. This file is the planning doc
for the next phase — primarily a smart routing/NAT daemon and policy
handling for cross-mesh bridging.

## Deployment target: home + RV NAT bridge

The motivating use case:

- **Master bridge**: at home, full participant in main mesh (e.g. Seattle's
  regional Cascadia mesh).
- **Slave bridge**: in a motorhome, short-range LoRa, follows the rig
  wherever it parks. Frequently within RF earshot of unrelated meshes.
- **Client device**: phone-paired MeshCore companion, talking to the slave
  bridge over RF when in the rig.
- **Daemon**: runs somewhere with reliable connectivity (home server, VPS),
  subscribed to both bridges' MQTT topics, implements all
  routing/NAT/filtering policy.

Goal: the client in the RV reaches Seattle via the bridges, but:

1. **No traffic from any local mesh the RV is parked near can propagate
   into Seattle.**
2. **No Seattle traffic leaks into whichever local mesh the RV is parked
   near.**

## Architecture

Firmware is the dumb shovel (already built — see CLAUDE.md). The daemon
adds policy on top. Two bridges share a broker; the daemon subscribes to
their `/rx` and/or `/tx` topics and publishes to their `/rf` (or whichever
topic each bridge has `mqtt.subscribe` set to).

Two flavors of pairing are supported by the existing firmware:

**A. Daemon-mediated** (asymmetric / DMZ / NAT): the use case here. Daemon
applies filtering, identity allowlists, path mutation, etc.

**B. Daemon-free symmetric peer-pair**: both bridges set `publish_tx=on`
and each subscribes to the other's `/tx`. Useful for two trusted bridges
that want to act as a single virtual RF environment, but **doesn't give
you any of the cross-contamination isolation** that the NAT case needs.
For the home+RV use case, use the daemon flavor.

## Slave → main (upstream) filter chain

Strict admission control in the daemon. In order:

1. **Hop count check**: only forward packets where `hop_count == 0`. The
   bridge was the first repeater to hear the packet, meaning its
   originator was a direct RF neighbor of the slave bridge. Anything 1+
   hops deep into a foreign mesh gets dropped.
2. **Dedup**: by `(payload_type, payload)` hash to avoid forwarding
   multiple RF receptions of the same logical packet. Matches firmware's
   `SimpleMeshTables.hasSeen()` semantics (ACKs deduped by ack_crc,
   TRACE includes path_len in hash).
3. **Per-payload-type policy**:
   - `ADVERT`: verify embedded pubkey is in source-allowlist; on pass,
     register `src_hash → pubkey` mapping; forward.
   - `REQ` / `RESPONSE` / `TXT_MSG` / `PATH`: look up `src_hash` against
     the table; forward only if it maps to an allowlisted pubkey.
   - `ACK` / `MULTIPART(ack)`: forward unconditionally, OR correlate to
     `recent_acks_we_expect: {crc → expiry}` populated when we forward
     a REQ. Either is safe.
   - `GRP_TXT` / `GRP_DATA`: **drop by default** (no identity in
     cleartext; see Public channel note below).
   - `ANON_REQ`: drop (carries only ephemeral pubkey by design).
   - `CONTROL`: drop (slave-side admin/discovery should not leak to main).
   - `TRACE`: drop (don't expose slave-side topology).
   - `RAW_CUSTOM`: drop (no standard identity field).

## Main → slave (downstream)

Wide open by default — slave-side users see all of main mesh — but **emit
on the slave RF with hop count saturated near `flood_max`** so a nearby
non-RV repeater hearing the transmission refuses to forward, keeping the
Seattle mesh from leaking into whatever local mesh the RV is parked near.

Optional path-padding strategies (in lieu of hop saturation):

- **Empty path / route_type=DIRECT**: UI on receivers shows contact as
  zero-hop neighbor. Other repeaters in earshot won't forward.
- **Padded flood with synthetic hashes**: receiver UI shows realistic
  "via N hops" distance, propagates one local hop, then dies at hop
  saturation. Synthetic hashes must be unique-per-chunk to avoid
  triggering loop detection. **This is the recommended default for
  egress** because it gives both realistic UX and 1-hop reach into the
  local mesh.

Always-drop on downstream (preserve op sec):

- `CONTROL` (don't leak Seattle admin to RV's local mesh)
- Admin `REQ` / `RESPONSE` (same)

## Source allowlist mechanics

Daemon config stores a set of trusted pubkey hex strings. Minimum useful
set for the home+RV deployment:

1. Your client device's pubkey
2. The slave bridge's own pubkey (only if `publish_tx=on` and you want the
   bridge visible on main)

Hash table built dynamically from observed allowlisted adverts:

```python
known_pubkeys: dict[hash, pubkey] = {}

on ADVERT (post-allowlist):
    known_pubkeys[short_hash(pubkey, hash_size)] = pubkey
```

Pre-seed at config time for zero-warmup operation (no gap before the
client's first advert is heard).

### Identity coverage per payload type

| Type | Identity in cleartext | Filter approach |
|------|----------------------|-----------------|
| ADVERT | full pubkey + Ed25519 signature | direct allowlist check; verify sig |
| REQ / RESPONSE / TXT_MSG / PATH | src_hash truncation only | hash→pubkey table from adverts |
| ACK / MULTIPART(ack) | none (just 4-byte ack_crc) | forward, or correlate to outstanding REQs |
| GRP_TXT / GRP_DATA | channel_hash only (sender hidden inside encrypted payload) | drop, or channel_hash allowlist |
| ANON_REQ | ephemeral pubkey by design | drop |
| CONTROL | unencrypted but subtype-dependent | drop on slave→main |
| TRACE | per-hop diagnostic | drop on slave→main |
| RAW_CUSTOM | app-defined | drop unless explicit allowlist |

Hash collision risk: with 1-byte hashes (default `path_hash_mode`),
~1/256 random foreign nodes hash to the same byte as your client. The
`hop_count==0` filter narrows the universe to direct RF neighbors of the
slave bridge, so in practice this is rare and bounded. With 2- or 3-byte
hashes (`path_hash_mode` 1 or 2), collision risk becomes negligible.

## Public channel cross-contamination

The "Public" channel pre-configured in companion firmware uses a hardcoded
PSK (`PUBLIC_GROUP_PSK` in `examples/companion_radio/MyMesh.cpp`).
Channel hash is `SHA256(PSK)`, so **Public's channel_hash is identical
across every MeshCore deployment on the planet**.

Consequences:

- Public **cannot be cleanly bridged**. There's no protocol-level signal
  distinguishing our user's Public post from any random other mesh
  member's Public post. Forwarding any Public traffic forwards all of it.
- Decrypting at the daemon doesn't help: the embedded sender "name" is
  freeform unsigned text (the protocol marks GRP_TXT as "unverified").
- **Drop all GRP_TXT/GRP_DATA on slave→main unconditionally.**

For cross-mesh group chat, use **private channels with unique PSKs**:

- User creates a channel with a non-default PSK on their client.
- PSK shared out-of-band with intended participants (e.g. specific Seattle
  contacts).
- Daemon's channel-hash allowlist includes only that private channel's
  hash.
- Cascadia members don't have that PSK and can't post valid messages to
  the daemon-allowlisted hash.

Net: Public works locally on either side of the bridge. Cross-mesh group
chat requires a private channel — by design, not a bridge limitation.

## Workflow for adding a new client device

1. On the new client: `get public.key` from its CLI / settings app.
2. Add the hex pubkey to the daemon's allowlist config.
3. Restart daemon (or SIGHUP if we add reload).
4. Client's next advert populates the hash table; from then on its
   traffic bridges upstream.

## What this gets you concretely

Park the RV next to a Cascadia mesh segment in Tacoma:

- Your client → slave bridge → daemon → main bridge → Seattle: DMs work. ✓
- Cascadia repeater 50m away → slave (hop=0, but pubkey not allowlisted)
  → daemon drops. Nothing leaks to Seattle. ✓
- Seattle traffic → main → daemon → slave → emitted on slave RF with
  padded path + saturated hop. Cascadia repeater hears, refuses to
  forward. Nothing leaks into Cascadia. ✓

Two-way isolation, single user-space daemon, no firmware changes from the
dumb-shovel base.

## Daemon implementation notes (not yet built)

- Language: Python (continuing from sniffer.py / console.py).
- Reuse `sniffer.py`'s packet parser, `NodeRegistry`, `DedupCache`.
- New state: per-deployment allowlist (pubkeys), conversation table
  (for path-rewrite if we add direct-routing across the bridge), recent
  acks table (if we go strict on ACK forwarding).
- Topology config: which bridges are "slave-style" (apply upstream
  filter), which are "main-style" (full passthrough), partner bridge ID
  per pair.
- Optional: route packets between MORE than two bridges (hub-and-spoke
  with a single main mesh and many slaves). Requires the daemon to track
  which slave a destination_hash lives behind, similar to a NAT
  translation table.

## Limits and edge cases worth flagging

- **MAX_PATH_SIZE = 64 bytes.** Egress path padding can't exceed this.
  With 1-byte hashes: up to 64 hops. With 3-byte hashes: ~21 hops.
- **Cyclic eviction in mesh's seen-table.** 128 entries; in a busy mesh
  (Cascadia at ~1700 nodes), this rolls over fast. Echo dedup degrades
  proportionally. Daemon dedup with a longer window covers the gap.
- **DM path learning across the bridge is broken in the simple model.**
  When a main user gets a DM from an RV user and the original DM arrived
  via flood, their app records a path that ends at the main bridge.
  Subsequent direct-routed DMs from main will travel that path until
  the bridge intercepts. Fine for the home+RV case where the bridge IS
  the bridgehead. For deeper mesh-of-meshes, see the Conversation Table
  / NAT-translation design discussed in earlier conversations (not
  captured here).
- **MQTT broker as single point of failure** for the cross-mesh link.
  Local meshes keep working when the broker is down; only the long-haul
  link drops. Acceptable for our use case.

## Reference: existing infrastructure recap

(Pointers; not copying CLAUDE.md content)

- `src/helpers/AbstractBridge.h` — interface our MqttBridge implements
- `src/helpers/bridges/BridgeBase.h` — shared base class
- `src/helpers/bridges/MqttBridge.{h,cpp}` — current firmware bridge
- `mqtt-bridge/sniffer.py` — packet parser + node registry + dedup
- `mqtt-bridge/console.py` — serial wrapper

The daemon to be built lives alongside these in `mqtt-bridge/`.
