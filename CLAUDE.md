# MeshCore — `meshed-up` fork

This repo is a personal fork of `meshcore-dev/MeshCore` on a branch called
**`meshed-up`**. It adds WiFi and an MQTT bridge to the simple_repeater
firmware on the Heltec v4, plus a small set of Python tools that talk
to the broker.

- **Origin** (`git push`): `https://github.com/ksanislo/MeshCore`
- **Upstream** (`git fetch upstream`): `https://github.com/meshcore-dev/MeshCore`
- **Active branch**: `meshed-up`

## What's built

A repeater node that:

- Connects to WiFi as a station (configurable via serial CLI; supports DHCP
  or static).
- Optionally connects to an MQTT broker and acts as a dumb pub/sub shovel
  between local RF and MQTT topics.
- Stays behavior-identical to stock MeshCore on the RF side — no mesh-level
  changes, no path mutation in the firmware.

All policy and routing logic intentionally lives **outside the firmware**, in
user-space tools that talk to the broker. The firmware itself is a thin
adapter that publishes what it heard and broadcasts what it's told to.

### MQTT topic layout

For a bridge with id `<id>` (default `meshcore-<8hex of pubkey>`) and a
default topic prefix `meshcore/<id>`:

```
meshcore/<id>/rx       firmware publishes RX-flavor (pre-forward) packets when
                       mqtt.publish_rx = on
meshcore/<id>/tx       firmware publishes TX-flavor (post-forward, with
                       this bridge's hash already in the path) when
                       mqtt.publish_tx = on
meshcore/<id>/rf       default subscribe target -- bytes published here are
                       injected onto local RF as if heard from the air
meshcore/<id>/status   retained "online"/"offline" via LWT
meshcore/<id>/advert   retained: bridge's pristine self-advert (raw mesh
                       packet bytes, no bridge header). Late subscribers
                       pick up identity immediately without waiting for an
                       RF advert cycle. Refreshed on every advert tx and
                       on hearing our own advert echoed back over RF.
```

`publish_rx` and `publish_tx` are independent — turn on either, both, or
neither. The subscribe topic (`mqtt.subscribe`) is configurable; setting it
to a peer's `/tx` topic gives you daemon-free peer-pairing.

`/rx` and `/tx` carry a versioned publish header prefixed to the raw mesh
packet bytes. The first byte is the version; receivers dispatch on it to
determine header length.

- **v0** (5 bytes): `version + uptime_ms(LE)`. Used on `/tx` -- no signal
  measurement applies to a packet that's about to be (or could have been)
  transmitted.
- **v1** (7 bytes): `version + uptime_ms(LE) + rssi(i8) + snr_x4(i8)`. Used
  on `/rx` where rssi/snr describe a real radio reception.

`/rf` expects raw bytes only (no header). The firmware strips the header
automatically when the subscribed topic ends in `/rx` or `/tx`, using the
version byte to pick the right strip length.

### Loop suppression

Deferred to the underlying mesh layer's `SimpleMeshTables`. No
firmware-level dedup table. Mesh dedup handles RF echoes and MQTT-injected
packets that loop back the same way it always has.

Topic-level self-filter: if our own `/rx` or `/tx` topic shows up via a
wildcard subscribe, it's dropped before parsing.

### Default-value conventions

Conventions for "use default" across all our prefs:

| Pref                  | "Use default" value | Resolves to                                |
|-----------------------|---------------------|--------------------------------------------|
| `mqtt.client_id`      | empty string        | `meshcore-<first 8 hex of pubkey>`         |
| `mqtt.topic_prefix`   | empty string        | `meshcore/<client_id>`                     |
| `mqtt.subscribe`      | empty string        | `<topic_prefix>/rf`                        |
| `mqtt.port`           | `0`                 | 1883 if mqtt.tls=off, 8883 if mqtt.tls=on  |
| `wifi.address`        | `0.0.0.0`           | DHCP                                       |

Strings can be cleared with `set <key> ` (trailing space, nothing after).

## Repo layout

```
src/helpers/CommonCLI.{h,cpp}            NodePrefs struct + set/get CLI handlers
                                         (added wifi.* and mqtt.* fields & cmds)
src/helpers/bridges/MqttBridge.{h,cpp}   MQTT BridgeBase implementation
src/helpers/bridges/{ESPNow,RS232}Bridge BridgeBase reference impls (unchanged)
src/helpers/AbstractBridge.h             5-method bridge interface
src/helpers/bridges/BridgeBase.h         shared base; seen_packets, fletcher16

examples/simple_repeater/MyMesh.{h,cpp}  WiFi + MQTT bridge integration into
                                         the repeater app (startWifi, logRx,
                                         logTx hooks)
variants/heltec_v4/platformio.ini        added envs:
                                           heltec_v4_repeater_wifi
                                           heltec_v4_repeater_mqtt

mqtt-bridge/sniffer.py                   subscribes to broker, decodes packets,
                                         labels paths from observed ADVERTs,
                                         dedup-cache stats
mqtt-bridge/console.py                   serial wrapper with line editing,
                                         tab completion, intercepted `help`,
                                         echo suppression, --get-all dump
mqtt-bridge/cli-commands.txt             plain-text command reference
mqtt-bridge/requirements.txt             paho-mqtt + pyserial

MQTT_BRIDGE_NOTES.md                     forward-looking design for the NAT
                                         routing daemon (not yet built)
```

## Build / flash

PlatformIO is installed in a venv at `/tmp/pio-venv` (created in a prior
session; recreate with `python3 -m venv /tmp/pio-venv && /tmp/pio-venv/bin/pip
install platformio` if it's gone).

Build the MQTT-bridge firmware + produce a merged image:

```sh
/tmp/pio-venv/bin/pio run -e heltec_v4_repeater_mqtt -t mergebin
```

Output: `.pio/build/heltec_v4_repeater_mqtt/firmware-merged.bin`.

Flash (after BOOT+RST to enter ROM download mode):

```sh
/tmp/pio-venv/bin/python3 \
  /home/ken/.platformio/packages/tool-esptoolpy/esptool.py \
  --chip esp32s3 --port /dev/ttyACM0 --baud 921600 \
  write_flash -z 0x0 \
  /home/ken/git/MeshCore/.pio/build/heltec_v4_repeater_mqtt/firmware-merged.bin
```

The device enumerates as USB-CDC on `/dev/ttyACM0` (the user is not in the
`dialout` group; either `sudo chmod 666 /dev/ttyACM0` per session, or add to
dialout permanently via `sudo usermod -aG dialout ken`).

## Talking to the device

Use `mqtt-bridge/console.py` for serial sessions. The firmware's raw CLI
doesn't do line editing, so `console.py` wraps it with readline, history,
tab completion, and echo suppression:

```sh
python3 mqtt-bridge/console.py             # interactive shell
python3 mqtt-bridge/console.py --get-all   # dump every property
python3 mqtt-bridge/console.py --cmd "get mqtt.status"
python3 mqtt-bridge/console.py --file setup.txt
python3 mqtt-bridge/console.py --cmd "help"   # client-side help, no device
```

`help` and `?` are intercepted client-side (the firmware has no help command).

## Conventions

- **No co-author trailers in commits.** This is a personal fork; commits
  stand under the maintainer's authorship alone.
- **Push to `origin` (ksanislo fork), never to `upstream`** (meshcore-dev).
  Upstream explicitly rejected MQTT in the reference bridges (per PR #454);
  we maintain MQTT support as a personal fork only.
- **Never push without explicit approval.** Treat `commit` and `push` as
  two separate authorization scopes. After committing, end with "want me
  to push?" unless the user already said so. Even small follow-on commits
  in the same session should wait for a fresh push instruction — the
  maintainer wants the option to review or amend before things hit the
  remote.
- **Firmware stays a dumb pub/sub shovel.** Any policy logic (filtering,
  routing, NAT, dedup beyond what the mesh layer already does) belongs in
  user-space Python under `mqtt-bridge/`, not in firmware.
- **NodePrefs additions are append-only** in the on-disk binary layout (see
  the byte-offset comments in `CommonCLI.cpp::loadPrefsInt` /
  `savePrefsInt`). Reading past EOF leaves fields at their constructor
  defaults — preserves upgrade compatibility from older firmware.

## Reference

- **Upstream maintainer view (PR #454, jbrazio):** ruled MQTT out of the
  reference bridges because MQTT depends on internet/broker availability,
  which would compromise the off-grid mission. Our deployment isn't
  off-grid (it's an interim long-haul tool, e.g. home ↔ RV via Starlink),
  so we accept the tradeoff and keep this work in a fork.
- **Wire format reference:** `docs/packet_format.md` (header bits, transport
  codes, path encoding).
- **Path hash format:** Per `src/Identity.h`'s `copyHashTo`, the path field
  uses the **first N bytes of the pubkey verbatim** (no SHA), where N is 1,
  2, or 3 depending on `path.hash.mode`.

## Useful upstream knowledge worth keeping handy

- The default "Public" channel in companion firmware uses a hardcoded
  globally-known PSK (`PUBLIC_GROUP_PSK` in
  `examples/companion_radio/MyMesh.cpp`). Channel hashes are
  `SHA256(PSK)`, so the Public channel hash is identical across every
  MeshCore mesh on the planet. This makes Public **un-bridgeable without
  cross-contamination** between distant meshes. See
  `MQTT_BRIDGE_NOTES.md` for the full analysis and the
  private-channel-with-unique-PSK workaround.
- ACK packets are deduped at the mesh layer by the first 4 bytes of payload
  (the ack_crc), NOT by the full packet hash. This is in
  `SimpleMeshTables::hasSeen()`.
- `logRx` fires before mesh dedup (`Dispatcher.cpp:237`). `logTx` fires
  when a packet actually goes out on the radio (in `Dispatcher.cpp:108`).
  Both are the integration points for the MQTT bridge.
