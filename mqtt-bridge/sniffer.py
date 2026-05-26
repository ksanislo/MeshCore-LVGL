#!/usr/bin/env python3
"""
MeshCore MQTT bridge sniffer.

Subscribes to one or more bridges' `down` topics, parses the bridge header
plus the MeshCore packet, and prints a human-readable view as packets arrive.

Topic convention (set by the firmware bridge):
    meshcore/<bridge_id>/down

Bridge `down` payload format:
    [1]  header version (currently 1)
    [4]  uptime millis (little-endian) when received by the bridge
    [1]  RSSI (signed dBm)
    [1]  SNR  (signed, x4 — divide by 4 for dB with 0.25 dB precision)
    [N]  raw MeshCore packet bytes

MeshCore packet format (see docs/packet_format.md):
    [1]  header byte: 0bVVPPPPRR (V=version, P=payload-type, R=route-type)
    [4]  transport_codes (only if route-type is TRANSPORT_FLOOD/TRANSPORT_DIRECT)
    [1]  path_length byte: bits 0-5 = hop count, bits 6-7 = (hash size - 1)
    [N]  path (hop_count * hash_size bytes)
    [N]  payload

Usage:
    python sniffer.py --host 192.168.1.10
    python sniffer.py --host broker.local --port 8883 --tls --user me --password secret
    python sniffer.py --host 192.168.1.10 --topic 'meshcore/+/down' -v
"""

import argparse
import hashlib
import signal
import struct
import sys
import time
from collections import OrderedDict
from dataclasses import dataclass
from typing import Optional, Tuple

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.stderr.write("paho-mqtt not installed. Run: pip install -r requirements.txt\n")
    sys.exit(1)


# ---- MeshCore protocol constants (from docs/packet_format.md) -----------------

ROUTE_TYPE_TRANSPORT_FLOOD  = 0b00
ROUTE_TYPE_FLOOD            = 0b01
ROUTE_TYPE_DIRECT           = 0b10
ROUTE_TYPE_TRANSPORT_DIRECT = 0b11

ROUTE_NAMES = {
    ROUTE_TYPE_TRANSPORT_FLOOD:  "TFLOOD",
    ROUTE_TYPE_FLOOD:            "FLOOD",
    ROUTE_TYPE_DIRECT:           "DIRECT",
    ROUTE_TYPE_TRANSPORT_DIRECT: "TDIRECT",
}

PAYLOAD_NAMES = {
    0x00: "REQ",
    0x01: "RESPONSE",
    0x02: "TXT_MSG",
    0x03: "ACK",
    0x04: "ADVERT",
    0x05: "GRP_TXT",
    0x06: "GRP_DATA",
    0x07: "ANON_REQ",
    0x08: "PATH",
    0x09: "TRACE",
    0x0A: "MULTIPART",
    0x0B: "CONTROL",
    0x0F: "RAW_CUSTOM",
}

# Advert app_data flag bits (low nibble = ADV_TYPE_*, high nibble = features)
ADV_TYPE_CHAT      = 1
ADV_TYPE_REPEATER  = 2
ADV_TYPE_ROOM      = 3
ADV_TYPE_SENSOR    = 4
ADV_TYPE_NAMES = {
    0: "NONE", 1: "CHAT", 2: "REPEATER", 3: "ROOM", 4: "SENSOR",
}
ADV_LATLON_MASK = 0x10
ADV_FEAT1_MASK  = 0x20
ADV_FEAT2_MASK  = 0x40
ADV_NAME_MASK   = 0x80

PUB_KEY_SIZE   = 32
SIGNATURE_SIZE = 64

BRIDGE_HDR_LEN = 7
BRIDGE_HDR_VER = 1

PAYLOAD_TYPE_ACK    = 0x03
PAYLOAD_TYPE_ADVERT = 0x04
PAYLOAD_TYPE_TRACE  = 0x09


# ---- Node registry (built from observed ADVERTs) -------------------------------

@dataclass
class NodeInfo:
    pubkey: bytes
    name: str
    adv_type: int
    last_seen: float


class NodeRegistry:
    """
    Tracks (pubkey -> name) from every ADVERT we parse. Path hashes in
    MeshCore are just the first N bytes of the pubkey (see Identity.h's
    `copyHashTo` -- `memcpy(dest, pub_key, len)`), so we can label any
    path entry whose prefix matches a known pubkey.
    """

    def __init__(self):
        self.nodes: dict = {}  # full pubkey -> NodeInfo

    def register(self, pubkey: bytes, name: str, adv_type: int) -> None:
        self.nodes[pubkey] = NodeInfo(
            pubkey=pubkey, name=name, adv_type=adv_type, last_seen=time.time()
        )

    def lookup_prefix(self, prefix: bytes):
        """
        Return (matching_node, num_matches). When num_matches > 1, the
        hash is too short to disambiguate -- common with 1-byte hashes
        in a busy regional mesh.
        """
        matches = [info for pk, info in self.nodes.items()
                   if pk.startswith(prefix)]
        if not matches:
            return None, 0
        return matches[0], len(matches)

    def lookup_all_prefix(self, prefix: bytes):
        """
        Return every known node whose pubkey starts with this prefix.
        Works uniformly for 1-, 2-, and 3-byte path hash sizes.
        """
        return [info for pk, info in self.nodes.items()
                if pk.startswith(prefix)]


# ---- Dedup cache ---------------------------------------------------------------

@dataclass
class DedupEntry:
    first_seen: float
    last_seen: float
    count: int
    first_hop: int
    last_hop: int


class DedupCache:
    """
    Publish-side dedup keyed on (payload_type, payload) — matches what the
    firmware's calculatePacketHash() considers "the same packet" so that the
    dedup rate observed here is what a daemon would see using the same rule.

    The firmware special-cases ACKs (dedup by first 4 bytes of payload) and
    TRACE (includes path_len in the hash). We mirror both.
    """

    def __init__(self, window_secs: float):
        self.window = window_secs
        self.entries: OrderedDict[bytes, DedupEntry] = OrderedDict()
        self.hits = 0
        self.misses = 0

    @staticmethod
    def key_for(pkt: "MeshPacket") -> bytes:
        if pkt.payload_type == PAYLOAD_TYPE_ACK:
            # firmware: stores first 4 bytes of payload as the ack id
            return b"\x03" + pkt.payload[:4]
        h = hashlib.sha256()
        h.update(bytes([pkt.payload_type]))
        if pkt.payload_type == PAYLOAD_TYPE_TRACE:
            # firmware hashes path_len as a uint16_t; the wire only carries
            # the low byte, so pad with a zero high byte to match in-memory layout.
            h.update(struct.pack("<H", pkt.path_length_byte))
        h.update(pkt.payload)
        return h.digest()

    def lookup(self, pkt: "MeshPacket", now: float) -> Tuple[bool, Optional[DedupEntry]]:
        """
        Returns (is_dup, entry).
        - is_dup=True means we've seen this key within the dedup window.
        - entry is the cached entry (now updated with count/last_seen/last_hop).
        """
        key = self.key_for(pkt)
        self._evict_expired(now)
        existing = self.entries.get(key)
        if existing is None:
            self.entries[key] = DedupEntry(
                first_seen=now, last_seen=now, count=1,
                first_hop=pkt.hop_count, last_hop=pkt.hop_count,
            )
            self.entries.move_to_end(key)
            self.misses += 1
            return (False, self.entries[key])
        existing.count += 1
        existing.last_seen = now
        existing.last_hop = pkt.hop_count
        self.entries.move_to_end(key)
        self.hits += 1
        return (True, existing)

    def _evict_expired(self, now: float) -> None:
        cutoff = now - self.window
        while self.entries:
            k, v = next(iter(self.entries.items()))
            if v.first_seen < cutoff:
                self.entries.popitem(last=False)
            else:
                break

    def stats(self) -> str:
        total = self.hits + self.misses
        rate = (self.hits / total * 100.0) if total else 0.0
        return (
            f"dedup: {self.hits} dups / {total} total "
            f"({rate:.1f}% hit rate), {len(self.entries)} keys in window"
        )


# ---- Parsing ------------------------------------------------------------------

@dataclass
class BridgeHeader:
    version: int
    uptime_ms: int
    rssi: int
    snr_db: float  # already converted from x4

    @classmethod
    def parse(cls, buf: bytes) -> "BridgeHeader":
        if len(buf) < BRIDGE_HDR_LEN:
            raise ValueError(f"bridge header too short ({len(buf)} bytes)")
        ver, uptime, rssi, snr_x4 = struct.unpack_from("<BIbb", buf, 0)
        return cls(version=ver, uptime_ms=uptime, rssi=rssi, snr_db=snr_x4 / 4.0)


@dataclass
class MeshPacket:
    payload_version: int  # bits 6-7 of header byte
    payload_type: int     # bits 2-5
    route_type: int       # bits 0-1
    transport_codes: Optional[tuple]  # (code1, code2) or None
    path_hash_size: int   # 1, 2, or 3 bytes per hop hash
    hop_count: int
    path: bytes           # raw path bytes
    payload: bytes        # raw payload bytes
    raw_len: int
    path_length_byte: int  # raw path_len byte (for firmware-parity TRACE hashing)

    @property
    def route_name(self) -> str:
        return ROUTE_NAMES.get(self.route_type, f"R{self.route_type}")

    @property
    def payload_name(self) -> str:
        return PAYLOAD_NAMES.get(self.payload_type, f"T{self.payload_type:#x}")

    @property
    def has_transport_codes(self) -> bool:
        return self.route_type in (ROUTE_TYPE_TRANSPORT_FLOOD, ROUTE_TYPE_TRANSPORT_DIRECT)

    @classmethod
    def parse(cls, buf: bytes) -> "MeshPacket":
        if len(buf) < 2:
            raise ValueError("packet too short for header + path_length")
        hdr = buf[0]
        route_type = hdr & 0b11
        payload_type = (hdr >> 2) & 0xF
        payload_version = (hdr >> 6) & 0b11

        i = 1
        tcodes = None
        if route_type in (ROUTE_TYPE_TRANSPORT_FLOOD, ROUTE_TYPE_TRANSPORT_DIRECT):
            if len(buf) < i + 4:
                raise ValueError("packet too short for transport_codes")
            tcodes = struct.unpack_from("<HH", buf, i)
            i += 4

        if len(buf) < i + 1:
            raise ValueError("packet too short for path_length")
        path_length_byte = buf[i]; i += 1
        hop_count = path_length_byte & 0b00111111
        path_hash_size = ((path_length_byte >> 6) & 0b11) + 1

        path_bytes = hop_count * path_hash_size
        if len(buf) < i + path_bytes:
            raise ValueError(f"packet too short for path ({path_bytes} bytes needed)")
        path = buf[i:i + path_bytes]
        i += path_bytes

        payload = buf[i:]

        return cls(
            payload_version=payload_version,
            payload_type=payload_type,
            route_type=route_type,
            transport_codes=tcodes,
            path_hash_size=path_hash_size,
            hop_count=hop_count,
            path=path,
            payload=payload,
            raw_len=len(buf),
            path_length_byte=path_length_byte,
        )


@dataclass
class AdvertInfo:
    pub_key: bytes
    timestamp: int        # unix epoch seconds
    signature: bytes
    adv_type: int         # ADV_TYPE_*
    has_latlon: bool
    lat: Optional[float]
    lon: Optional[float]
    name: str

    @classmethod
    def parse(cls, payload: bytes) -> Optional["AdvertInfo"]:
        if len(payload) < PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + 1:
            return None
        i = 0
        pub_key = payload[i:i + PUB_KEY_SIZE]; i += PUB_KEY_SIZE
        ts = struct.unpack_from("<I", payload, i)[0]; i += 4
        sig = payload[i:i + SIGNATURE_SIZE]; i += SIGNATURE_SIZE

        flags = payload[i]; i += 1
        adv_type = flags & 0x0F
        has_latlon = (flags & ADV_LATLON_MASK) != 0
        lat = lon = None
        if has_latlon:
            if len(payload) < i + 8:
                return None
            lat = struct.unpack_from("<i", payload, i)[0] / 1e6; i += 4
            lon = struct.unpack_from("<i", payload, i)[0] / 1e6; i += 4
        # skip feature fields if present
        if flags & ADV_FEAT1_MASK:
            i += 2
        if flags & ADV_FEAT2_MASK:
            i += 2
        name = ""
        if (flags & ADV_NAME_MASK) and i < len(payload):
            name_bytes = payload[i:]
            try:
                name = name_bytes.decode("utf-8", errors="replace").rstrip("\x00")
            except Exception:
                name = name_bytes.hex()
        return cls(
            pub_key=pub_key, timestamp=ts, signature=sig,
            adv_type=adv_type, has_latlon=has_latlon, lat=lat, lon=lon, name=name,
        )


# ---- Rendering ----------------------------------------------------------------

def short_hex(b: bytes, n: int = 8) -> str:
    return b[:n].hex()


def render_path(path: bytes, hash_size: int) -> str:
    if not path:
        return "(empty)"
    return " ".join(path[i:i + hash_size].hex() for i in range(0, len(path), hash_size))


def render_path_labeled(path: bytes, hash_size: int,
                        registry: Optional["NodeRegistry"],
                        label_min_bytes: int = 2) -> str:
    """
    Compact path render. Each chunk (1-, 2-, or 3-byte hash, per hash_size)
    is shown as hex; when one or more known nodes share the prefix, their
    names are appended in parentheses, comma-separated. Chunks are joined
    with '-' to keep long paths readable on a single line.

    Names are only shown when hash_size >= label_min_bytes. With 1-byte
    hashes and a regional mesh (e.g. Cascadia at ~1700 nodes across 256
    buckets), every chunk would otherwise match 5+ nodes -- the noise
    drowns out any signal. 2- and 3-byte hashes give meaningfully unique
    matches and are worth labeling.

    Examples:
        c7-d4-db-7f                             (1-byte hashes; no labels)
        a1b2(SouthCapHill)-c3d4(Bob)-e5f6       (2-byte; usually unique)
        a1b2c3(SouthCap)-d4e5f6(MikeRep)        (3-byte; effectively unique)
    """
    if not path:
        return "(empty)"
    show_labels = (registry is not None) and (hash_size >= label_min_bytes)
    parts = []
    for i in range(0, len(path), hash_size):
        chunk = path[i:i + hash_size]
        label = chunk.hex()
        if show_labels:
            matches = registry.lookup_all_prefix(chunk)
            if matches:
                names = ",".join(m.name for m in matches)
                label = f"{chunk.hex()}({names})"
        parts.append(label)
    return "-".join(parts)


def hexdump(b: bytes, indent: str = "    ") -> str:
    out = []
    for off in range(0, len(b), 16):
        chunk = b[off:off + 16]
        hex_part = " ".join(f"{x:02x}" for x in chunk)
        ascii_part = "".join((chr(x) if 32 <= x < 127 else ".") for x in chunk)
        out.append(f"{indent}{off:04x}  {hex_part:<47}  {ascii_part}")
    return "\n".join(out)


def render_packet(topic: str, payload: bytes, verbose: bool, debug: bool = False,
                  dedup: Optional[DedupCache] = None,
                  registry: Optional["NodeRegistry"] = None,
                  label_min_bytes: int = 2) -> str:
    parts = []
    bridge_id = topic.split("/")[-2] if "/" in topic else topic

    # If we're in debug mode, dump the raw MQTT payload up front so it's
    # available even when parsing fails downstream.
    if debug:
        parts.append(f"[{ts_now()}] {bridge_id} RAW mqtt payload ({len(payload)} bytes):")
        parts.append(hexdump(payload))

    # Bridge header
    try:
        hdr = BridgeHeader.parse(payload)
    except ValueError as e:
        parts.append(f"[{ts_now()}] {bridge_id} (bad bridge header: {e})")
        return "\n".join(parts)

    if hdr.version != BRIDGE_HDR_VER:
        parts.append(f"[{ts_now()}] {bridge_id} (unknown bridge header version {hdr.version})")
        return "\n".join(parts)

    if debug:
        bh = payload[:BRIDGE_HDR_LEN]
        parts.append(
            "  bridge_hdr: "
            f"ver=0x{bh[0]:02x} "
            f"uptime=0x{bh[4]:02x}{bh[3]:02x}{bh[2]:02x}{bh[1]:02x}(LE)={hdr.uptime_ms} "
            f"rssi=0x{bh[5]:02x}({hdr.rssi}) "
            f"snr_x4=0x{bh[6]:02x}({hdr.snr_db:.2f}dB)"
        )

    mesh_bytes = payload[BRIDGE_HDR_LEN:]

    try:
        pkt = MeshPacket.parse(mesh_bytes)
    except ValueError as e:
        parts.append(f"[{ts_now()}] {bridge_id} (bad mesh packet: {e}) rssi={hdr.rssi}")
        if debug:
            parts.append(f"  mesh bytes ({len(mesh_bytes)}):")
            parts.append(hexdump(mesh_bytes))
        return "\n".join(parts)

    # Dedup check — annotates the line but never drops the message.
    dup_tag = ""
    if dedup is not None:
        is_dup, entry = dedup.lookup(pkt, time.time())
        if is_dup:
            age = entry.last_seen - entry.first_seen
            hop_delta = pkt.hop_count - entry.first_hop
            dup_tag = (
                f" DUP[#{entry.count} age={age:.1f}s "
                f"first_hop={entry.first_hop} +{hop_delta}]"
            )

    # One-line summary
    line = (
        f"[{ts_now()}] {bridge_id} "
        f"hop={pkt.hop_count} {pkt.payload_name} {pkt.route_name} "
        f"rssi={hdr.rssi} snr={hdr.snr_db:.1f}dB len={pkt.raw_len}{dup_tag}"
    )
    parts.append(line)

    # In debug mode, break down the header byte and path_length byte explicitly.
    if debug:
        h = mesh_bytes[0]
        parts.append(
            "  hdr byte: "
            f"0x{h:02x}=0b{h:08b} -> ver={pkt.payload_version} "
            f"type={pkt.payload_type:#x}({pkt.payload_name}) "
            f"route={pkt.route_type}({pkt.route_name})"
        )
        # path_length byte offset depends on whether transport_codes are present
        plb_off = 1 + (4 if pkt.has_transport_codes else 0)
        if plb_off < len(mesh_bytes):
            plb = mesh_bytes[plb_off]
            parts.append(
                "  path_len: "
                f"0x{plb:02x}=0b{plb:08b} -> "
                f"hash_size_bits=0b{(plb >> 6) & 0b11:02b}->{pkt.path_hash_size}B, "
                f"hop_count_bits=0b{plb & 0b00111111:06b}->{pkt.hop_count}"
            )
        parts.append(f"  full mesh packet ({pkt.raw_len} bytes):")
        parts.append(hexdump(mesh_bytes))

    # Payload-specific decoding
    if pkt.payload_type == PAYLOAD_TYPE_ADVERT:
        adv = AdvertInfo.parse(pkt.payload)
        if adv:
            type_name = ADV_TYPE_NAMES.get(adv.adv_type, str(adv.adv_type))
            ts_age = int(time.time()) - adv.timestamp
            line2 = (
                f"  ADVERT type={type_name} name={adv.name!r} "
                f"pubkey={short_hex(adv.pub_key)} age={ts_age}s"
            )
            if adv.has_latlon and (adv.lat or adv.lon):
                line2 += f" loc=({adv.lat:.4f}, {adv.lon:.4f})"
            parts.append(line2)
            # Register the node so future paths can be labeled with its name.
            if registry is not None:
                registry.register(adv.pub_key, adv.name, adv.adv_type)

    # Default: show the labeled path on a second line for any non-empty path.
    if pkt.hop_count > 0:
        parts.append(
            f"  path[{pkt.hop_count}@{pkt.path_hash_size}]: "
            f"{render_path_labeled(pkt.path, pkt.path_hash_size, registry, label_min_bytes)}"
        )

    # Verbose: extra details (transport codes, raw bytes, uptime)
    if verbose:
        if pkt.has_transport_codes:
            parts.append(f"  transport_codes: 0x{pkt.transport_codes[0]:04x} 0x{pkt.transport_codes[1]:04x}")
        parts.append(f"  payload[{len(pkt.payload)}]: {pkt.payload.hex()}")
        parts.append(f"  uptime: {hdr.uptime_ms} ms")

    return "\n".join(parts)


def ts_now() -> str:
    return time.strftime("%H:%M:%S")


# ---- MQTT loop ----------------------------------------------------------------

def on_connect(client, userdata, flags, reason_code, properties=None):
    print(f"[{ts_now()}] connected: {reason_code}")
    for topic in userdata["topics"]:
        client.subscribe(topic, qos=0)
        print(f"[{ts_now()}] subscribed: {topic}")


def on_disconnect(client, userdata, *args):
    print(f"[{ts_now()}] disconnected")


def on_message(client, userdata, msg):
    bridge_filter = userdata.get("bridge_filter")
    if bridge_filter:
        parts = msg.topic.split("/")
        if len(parts) < 2 or bridge_filter not in parts:
            return
    line = render_packet(
        msg.topic, msg.payload,
        userdata["verbose"], userdata["debug"],
        dedup=userdata.get("dedup"),
        registry=userdata.get("registry"),
        label_min_bytes=userdata.get("label_min_bytes", 2),
    )
    print(line)

    # Periodic dedup stats line
    dedup: Optional[DedupCache] = userdata.get("dedup")
    if dedup is not None:
        userdata["msg_count"] += 1
        every = userdata.get("stats_every", 100)
        if every > 0 and userdata["msg_count"] % every == 0:
            print(f"[{ts_now()}] -- {dedup.stats()}")


def main():
    ap = argparse.ArgumentParser(description="MeshCore MQTT sniffer")
    ap.add_argument("--host", required=True, help="MQTT broker hostname or IP")
    ap.add_argument("--port", type=int, default=1883, help="MQTT broker port (default 1883)")
    ap.add_argument("--topic", action="append", default=None,
                    help="topic(s) to subscribe; repeatable. "
                         "Default: meshcore/+/rx AND meshcore/+/tx "
                         "(covers any bridge publishing in either flavor).")
    ap.add_argument("--user", help="MQTT username")
    ap.add_argument("--password", help="MQTT password")
    ap.add_argument("--tls", action="store_true",
                    help="use TLS (system trust store; pair with --port 8883)")
    ap.add_argument("--bridge", help="filter to a specific bridge_id (substring match in topic)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="show transport codes, path bytes, raw payload hex")
    ap.add_argument("--debug", action="store_true",
                    help="dump full raw MQTT payload, bridge-header bytes, mesh header/path_len bit breakdown")
    ap.add_argument("--no-dedup", action="store_true",
                    help="disable publish-side dedup logging")
    ap.add_argument("--dedup-window", type=float, default=60.0,
                    help="dedup time window in seconds (default 60)")
    ap.add_argument("--stats-every", type=int, default=100,
                    help="print dedup stats line every N messages (0 to disable)")
    ap.add_argument("--label-min-bytes", type=int, default=2, choices=[1, 2, 3],
                    help="only attach known-node names when hash_size >= this many "
                         "bytes (default 2; 1-byte hashes collide too often in big "
                         "meshes to be informative)")
    ap.add_argument("--client-id", default=f"meshcore-sniffer-{int(time.time())}",
                    help="MQTT client id")
    args = ap.parse_args()

    dedup = None if args.no_dedup else DedupCache(window_secs=args.dedup_window)
    registry = NodeRegistry()

    topics = args.topic if args.topic else ["meshcore/+/rx", "meshcore/+/tx"]

    userdata = {
        "topics": topics,
        "verbose": args.verbose,
        "debug": args.debug,
        "bridge_filter": args.bridge,
        "dedup": dedup,
        "registry": registry,
        "label_min_bytes": args.label_min_bytes,
        "msg_count": 0,
        "stats_every": args.stats_every,
    }

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=args.client_id,
        userdata=userdata,
    )
    if args.user:
        client.username_pw_set(args.user, args.password or "")
    if args.tls:
        client.tls_set()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    print(f"[{ts_now()}] connecting to {args.host}:{args.port} (tls={args.tls})...")
    client.connect(args.host, args.port, keepalive=30)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        if dedup is not None:
            print(f"\n[{ts_now()}] -- final {dedup.stats()}")
        print("bye")


if __name__ == "__main__":
    main()
