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
import base64
import binascii
import hashlib
import hmac
import json
import os
import signal
import struct
import sys
import time
from collections import OrderedDict
from dataclasses import dataclass, field
from typing import Optional, Tuple, List, Dict

try:
    # Upstream PyCryptodome installs as `Crypto` (pip install pycryptodome).
    # Debian's python3-pycryptodome installs as `Cryptodome` to avoid colliding
    # with the legacy python-crypto package -- fall back to that namespace.
    try:
        from Crypto.Cipher import AES
    except ImportError:
        from Cryptodome.Cipher import AES
    _HAVE_AES = True
except ImportError:
    _HAVE_AES = False

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

# Bridge publish header layouts. v0 = version + uptime_ms only (used on /tx,
# where there's no signal data to report). v1 adds rssi + snr_x4 (used on /rx,
# where the values describe a real radio reception).
BRIDGE_HDR_VER_V0 = 0
BRIDGE_HDR_VER_V1 = 1
BRIDGE_HDR_LEN_V0 = 5
BRIDGE_HDR_LEN_V1 = 7

PAYLOAD_TYPE_ACK     = 0x03
PAYLOAD_TYPE_ADVERT  = 0x04
PAYLOAD_TYPE_GRP_TXT = 0x05
PAYLOAD_TYPE_GRP_DAT = 0x06
PAYLOAD_TYPE_TRACE   = 0x09

# MeshCore crypto constants (from src/MeshCore.h)
CIPHER_KEY_SIZE = 16   # AES-128 key size in bytes
CIPHER_MAC_SIZE = 2    # MAC bytes prefixed in encrypted payloads
PUB_KEY_SIZE    = 32   # HMAC key length used by firmware (PSK zero-padded to this)

# The hardcoded Public channel PSK that every MeshCore companion firmware
# pre-populates at first boot. See examples/companion_radio/MyMesh.cpp.
PUBLIC_GROUP_PSK_B64 = "izOH6cXN6mrJ5e26oRXNcg=="


# ---- Channel decryption --------------------------------------------------------

@dataclass
class ChannelKey:
    name: str
    psk: bytes              # 16 or 32 bytes
    hash_byte: int          # first byte of SHA256(psk) -- the on-wire channel_hash


def _channel_hash_byte(psk: bytes) -> int:
    """First byte of SHA256(psk) -- matches firmware's `mesh::Utils::sha256(
    dest->channel.hash, sizeof(dest->channel.hash), psk, len)` with
    sizeof(hash) = PATH_HASH_SIZE = 1."""
    return hashlib.sha256(psk).digest()[0]


def _channel_decrypt(psk: bytes, mac_and_ct: bytes) -> Optional[bytes]:
    """
    Mirror of `Utils::MACThenDecrypt`. Returns plaintext (with zero-padding
    intact) if HMAC verifies, else None.

    Firmware semantics:
      - HMAC key is the channel.secret buffer (PUB_KEY_SIZE = 32 bytes);
        PSKs shorter than 32 bytes are zero-padded.
      - HMAC output is truncated to CIPHER_MAC_SIZE (2 bytes).
      - AES-128-ECB decryption uses the first CIPHER_KEY_SIZE (16) bytes
        of the secret as the key.
      - Ciphertext is zero-padded to a 16-byte multiple at encrypt time.
    """
    if not _HAVE_AES:
        return None
    if len(mac_and_ct) <= CIPHER_MAC_SIZE:
        return None
    mac_received = mac_and_ct[:CIPHER_MAC_SIZE]
    ct = mac_and_ct[CIPHER_MAC_SIZE:]
    if len(ct) == 0 or len(ct) % 16 != 0:
        return None
    hmac_key = psk.ljust(PUB_KEY_SIZE, b"\x00")[:PUB_KEY_SIZE]
    mac_expected = hmac.new(hmac_key, ct, hashlib.sha256).digest()[:CIPHER_MAC_SIZE]
    if not hmac.compare_digest(mac_expected, mac_received):
        return None
    aes_key = psk[:CIPHER_KEY_SIZE].ljust(CIPHER_KEY_SIZE, b"\x00")[:CIPHER_KEY_SIZE]
    cipher = AES.new(aes_key, AES.MODE_ECB)
    return cipher.decrypt(ct)


def _parse_grp_txt(plain: bytes) -> Optional[Tuple[int, str, str]]:
    """
    Parse a decrypted GRP_TXT payload.

    Wire format (after decryption, per BaseChatMesh::sendGroupMessage):
      [0..3]  uint32_t timestamp  (little-endian)
      [4]     txt_type byte       (high 6 bits must be 0)
      [5..]   "sender_name: message" UTF-8, zero-padded to AES block size
    """
    if len(plain) < 5:
        return None
    ts = struct.unpack_from("<I", plain, 0)[0]
    txt_type = plain[4]
    if (txt_type >> 2) != 0:
        return None
    body = plain[5:]
    # Strip AES zero-padding (text was null-terminated before encryption).
    nul = body.find(b"\x00")
    if nul >= 0:
        body = body[:nul]
    try:
        text = body.decode("utf-8", errors="replace")
    except Exception:
        text = body.hex()
    if ": " in text:
        sender, _, message = text.partition(": ")
    else:
        sender, message = "", text
    return ts, sender, message


class ChannelRegistry:
    """
    Holds PSKs for any channels we want to decrypt. Public is pre-loaded.
    Multiple channels can share the same on-wire hash byte (1-in-256 chance);
    we try each in turn against the HMAC.
    """

    def __init__(self):
        self.by_hash: Dict[int, List[ChannelKey]] = {}
        self._add_internal("Public", base64.b64decode(PUBLIC_GROUP_PSK_B64))

    def _add_internal(self, name: str, psk: bytes) -> None:
        if len(psk) not in (16, 32):
            raise ValueError(f"channel PSK must be 16 or 32 bytes; got {len(psk)}")
        h = _channel_hash_byte(psk)
        self.by_hash.setdefault(h, []).append(ChannelKey(name=name, psk=psk, hash_byte=h))

    def add_from_spec(self, spec: str) -> None:
        """Accept 'Name:base64psk' from a CLI arg."""
        if ":" not in spec:
            raise ValueError(
                "channel spec must be 'NAME:base64_psk' (e.g. 'PNW:abc123==')"
            )
        name, _, psk_b64 = spec.partition(":")
        try:
            psk = base64.b64decode(psk_b64, validate=True)
        except (binascii.Error, ValueError) as e:
            raise ValueError(f"channel '{name}' PSK is not valid base64: {e}")
        self._add_internal(name.strip(), psk)

    def try_decode_grp(self, payload: bytes) -> Optional[Tuple[str, int, str, str]]:
        """
        Attempt to decrypt a GRP_TXT payload.
        Returns (channel_name, timestamp, sender, message) on success.
        """
        if len(payload) < 1 + CIPHER_MAC_SIZE:
            return None
        hb = payload[0]
        mac_and_ct = payload[1:]
        for ch in self.by_hash.get(hb, []):
            plain = _channel_decrypt(ch.psk, mac_and_ct)
            if plain is None:
                continue
            parsed = _parse_grp_txt(plain)
            if parsed is None:
                continue
            ts, sender, message = parsed
            return ch.name, ts, sender, message
        return None


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

    Optionally persisted to a JSON file across runs so we don't lose
    accumulated knowledge to every restart. Adverts in a regional mesh
    can be minutes-to-hours apart, so rebuilding from scratch every
    time costs real observability.
    """

    PERSIST_FORMAT_VERSION = 1
    AUTOSAVE_INTERVAL_SECS = 30.0   # min wall-clock between disk writes

    def __init__(self, persist_path: Optional[str] = None):
        self.nodes: dict = {}  # full pubkey -> NodeInfo
        self.persist_path = persist_path
        self._dirty = False
        self._last_save = 0.0
        if persist_path:
            self._load()

    def register(self, pubkey: bytes, name: str, adv_type: int) -> None:
        existing = self.nodes.get(pubkey)
        now = time.time()
        if existing and existing.name == name and existing.adv_type == adv_type:
            # Same data; just refresh last_seen. Still mark dirty so the
            # timestamp gets persisted, but the autosave throttle keeps
            # writes bounded.
            existing.last_seen = now
        else:
            self.nodes[pubkey] = NodeInfo(
                pubkey=pubkey, name=name, adv_type=adv_type, last_seen=now,
            )
        self._dirty = True

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

    # ---- persistence ---------------------------------------------------------

    def maybe_save(self, force: bool = False) -> None:
        """Save if dirty and either forced or autosave interval has elapsed."""
        if not self.persist_path or not self._dirty:
            return
        now = time.time()
        if not force and (now - self._last_save) < self.AUTOSAVE_INTERVAL_SECS:
            return
        self._save()

    def _save(self) -> None:
        tmp_path = self.persist_path + ".tmp"
        try:
            payload = {
                "version": self.PERSIST_FORMAT_VERSION,
                "saved_at": time.time(),
                "nodes": [
                    {
                        "pubkey": info.pubkey.hex(),
                        "name": info.name,
                        "adv_type": info.adv_type,
                        "last_seen": info.last_seen,
                    }
                    for info in self.nodes.values()
                ],
            }
            with open(tmp_path, "w") as f:
                json.dump(payload, f, indent=2)
            os.replace(tmp_path, self.persist_path)
            self._last_save = time.time()
            self._dirty = False
        except OSError as e:
            sys.stderr.write(f"[{ts_now()}] warning: registry save failed: {e}\n")
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

    def _load(self) -> None:
        if not os.path.exists(self.persist_path):
            return
        try:
            with open(self.persist_path) as f:
                data = json.load(f)
            ver = data.get("version", 0)
            if ver != self.PERSIST_FORMAT_VERSION:
                sys.stderr.write(
                    f"[{ts_now()}] warning: registry version mismatch "
                    f"(file v{ver}, expected v{self.PERSIST_FORMAT_VERSION}); "
                    f"starting fresh\n"
                )
                return
            loaded = 0
            for entry in data.get("nodes", []):
                try:
                    pk = bytes.fromhex(entry["pubkey"])
                    self.nodes[pk] = NodeInfo(
                        pubkey=pk,
                        name=entry.get("name", ""),
                        adv_type=int(entry.get("adv_type", 0)),
                        last_seen=float(entry.get("last_seen", 0.0)),
                    )
                    loaded += 1
                except (ValueError, KeyError, TypeError):
                    continue
            print(f"[{ts_now()}] registry: loaded {loaded} nodes from "
                  f"{self.persist_path}")
            self._last_save = time.time()
        except (json.JSONDecodeError, OSError) as e:
            sys.stderr.write(f"[{ts_now()}] warning: registry load failed: {e}\n")


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
    header_len: int                  # 5 (v0) or 7 (v1)
    rssi: Optional[int] = None       # v1 only
    snr_db: Optional[float] = None   # v1 only (already converted from x4)

    @classmethod
    def parse(cls, buf: bytes) -> "BridgeHeader":
        if len(buf) < 1:
            raise ValueError("empty payload")
        ver = buf[0]
        if ver == BRIDGE_HDR_VER_V0:
            if len(buf) < BRIDGE_HDR_LEN_V0:
                raise ValueError(f"bridge v0 header too short ({len(buf)} bytes)")
            _ver, uptime = struct.unpack_from("<BI", buf, 0)
            return cls(version=ver, uptime_ms=uptime, header_len=BRIDGE_HDR_LEN_V0)
        if ver == BRIDGE_HDR_VER_V1:
            if len(buf) < BRIDGE_HDR_LEN_V1:
                raise ValueError(f"bridge v1 header too short ({len(buf)} bytes)")
            _ver, uptime, rssi, snr_x4 = struct.unpack_from("<BIbb", buf, 0)
            return cls(version=ver, uptime_ms=uptime, header_len=BRIDGE_HDR_LEN_V1,
                       rssi=rssi, snr_db=snr_x4 / 4.0)
        raise ValueError(f"unknown bridge header version {ver}")


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
                  channels: Optional["ChannelRegistry"] = None,
                  label_min_bytes: int = 2) -> str:
    parts = []
    bridge_id = topic.split("/")[-2] if "/" in topic else topic

    # If we're in debug mode, dump the raw MQTT payload up front so it's
    # available even when parsing fails downstream.
    if debug:
        parts.append(f"[{ts_now()}] {bridge_id} RAW mqtt payload ({len(payload)} bytes):")
        parts.append(hexdump(payload))

    # /advert topic carries raw mesh packet bytes (no bridge header) -- it's a
    # retained snapshot of the bridge's pristine self-advert, used so late
    # subscribers can discover identities without waiting for an RF advert.
    is_advert_topic = topic.endswith("/advert")

    if is_advert_topic:
        mesh_bytes = payload
        sig_str = " [retained]"
        hdr = None
    else:
        # Bridge header (version byte selects layout: v0 = 5 bytes, v1 = 7 bytes)
        try:
            hdr = BridgeHeader.parse(payload)
        except ValueError as e:
            parts.append(f"[{ts_now()}] {bridge_id} (bad bridge header: {e})")
            return "\n".join(parts)

        if debug:
            bh = payload[:hdr.header_len]
            bh_desc = (
                "  bridge_hdr: "
                f"ver=0x{bh[0]:02x} "
                f"uptime=0x{bh[4]:02x}{bh[3]:02x}{bh[2]:02x}{bh[1]:02x}(LE)={hdr.uptime_ms}"
            )
            if hdr.version == BRIDGE_HDR_VER_V1:
                bh_desc += (
                    f" rssi=0x{bh[5]:02x}({hdr.rssi})"
                    f" snr_x4=0x{bh[6]:02x}({hdr.snr_db:.2f}dB)"
                )
            parts.append(bh_desc)

        mesh_bytes = payload[hdr.header_len:]

        # rssi/snr only meaningful when present (v1); v0 publishes (e.g. /tx)
        # carry no signal data, so omit those fields entirely.
        sig_str = (
            f" rssi={hdr.rssi} snr={hdr.snr_db:.1f}dB"
            if hdr.rssi is not None and hdr.snr_db is not None
            else ""
        )

    try:
        pkt = MeshPacket.parse(mesh_bytes)
    except ValueError as e:
        parts.append(f"[{ts_now()}] {bridge_id} (bad mesh packet: {e}){sig_str}")
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
        f"hop={pkt.hop_count} {pkt.payload_name} {pkt.route_name}"
        f"{sig_str} len={pkt.raw_len}{dup_tag}"
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
    elif pkt.payload_type == PAYLOAD_TYPE_GRP_TXT and channels is not None:
        decoded = channels.try_decode_grp(pkt.payload)
        if decoded is not None:
            chan_name, ts, sender, message = decoded
            age = int(time.time()) - ts
            sender_disp = sender if sender else "(no-sender)"
            parts.append(
                f"  CHAN[{chan_name}] {sender_disp}: {message}  (age={age}s)"
            )

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
        channels=userdata.get("channels"),
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

    # Throttled autosave of the node registry. Cheap (no-op when not dirty
    # or when within the autosave interval).
    reg: Optional[NodeRegistry] = userdata.get("registry")
    if reg is not None:
        reg.maybe_save()


def main():
    ap = argparse.ArgumentParser(description="MeshCore MQTT sniffer")
    ap.add_argument("--host", required=True, help="MQTT broker hostname or IP")
    ap.add_argument("--port", type=int, default=1883, help="MQTT broker port (default 1883)")
    ap.add_argument("--topic", action="append", default=None,
                    help="topic(s) to subscribe; repeatable. "
                         "Default: meshcore/+/rx, meshcore/+/tx, meshcore/+/advert "
                         "(covers any bridge publishing in any of those flavors).")
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
    ap.add_argument("--registry-file",
                    default=os.path.expanduser("~/.meshcore_node_registry.json"),
                    help="path to persisted node registry JSON "
                         "(default ~/.meshcore_node_registry.json)")
    ap.add_argument("--no-persist-registry", action="store_true",
                    help="disable loading/saving the node registry across runs")
    ap.add_argument("--channel", action="append", default=[], metavar="NAME:BASE64_PSK",
                    help="add a channel key for decryption (repeatable). "
                         "The Public channel is always pre-loaded. Example: "
                         "--channel 'MyClub:abc123def456=='")
    ap.add_argument("--no-channels", action="store_true",
                    help="disable channel decryption entirely (don't even try Public)")
    ap.add_argument("--client-id", default=f"meshcore-sniffer-{int(time.time())}",
                    help="MQTT client id")
    args = ap.parse_args()

    dedup = None if args.no_dedup else DedupCache(window_secs=args.dedup_window)
    registry_path = None if args.no_persist_registry else args.registry_file
    registry = NodeRegistry(persist_path=registry_path)

    channels: Optional[ChannelRegistry] = None
    if not args.no_channels:
        if not _HAVE_AES:
            sys.stderr.write(
                "[warn] pycryptodome not installed; channel decryption disabled. "
                "Run: pip install -r requirements.txt\n"
            )
        else:
            channels = ChannelRegistry()
            for spec in args.channel:
                try:
                    channels.add_from_spec(spec)
                except ValueError as e:
                    sys.stderr.write(f"[warn] --channel {spec!r}: {e}\n")
            chan_names = sorted({c.name for clist in channels.by_hash.values() for c in clist})
            print(f"[{ts_now()}] channels loaded: {', '.join(chan_names)}")

    topics = args.topic if args.topic else [
        "meshcore/+/rx", "meshcore/+/tx", "meshcore/+/advert",
    ]

    userdata = {
        "topics": topics,
        "verbose": args.verbose,
        "debug": args.debug,
        "bridge_filter": args.bridge,
        "dedup": dedup,
        "registry": registry,
        "channels": channels,
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
        registry.maybe_save(force=True)
        if registry.persist_path:
            print(f"[{ts_now()}] -- registry: {len(registry.nodes)} nodes -> "
                  f"{registry.persist_path}")
        print("bye")


if __name__ == "__main__":
    main()
