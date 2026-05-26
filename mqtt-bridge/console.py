#!/usr/bin/env python3
"""
MeshCore serial console with line editing.

Wraps the bridge's serial CLI so you don't have to fight a raw TTY for every
typo. Local line editing (backspace, history, tab completion) is handled here;
commands are sent atomically to the device with a proper \\r terminator.

Modes:
    console.py                                  # interactive shell
    console.py --cmd "get mqtt.status"          # one-shot
    console.py --file commands.txt              # batch from file
    console.py --get-all                        # dump every known get
"""

import argparse
import os
import readline
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial not installed. Run: pip install -r requirements.txt\n")
    sys.exit(1)


# ---- Known commands for tab completion -----------------------------------------

# Properties that work as `get <prop>` (verified against CommonCLI.cpp::handleGetCmd).
# Also includes "all" -- a client-side virtual property that runs the full
# config dump (same as the --get-all CLI flag).
GET_PROPS = [
    "all",            # client-side virtual; runs the full config dump
    "dutycycle", "af", "int.thresh", "agc.reset.interval", "multi.acks",
    "allow.read.only", "flood.advert.interval", "advert.interval",
    "guest.password", "name", "repeat", "lat", "lon",
    "radio.rxgain",   # SX1262/SX1268 builds only
    "radio",          # combined: returns freq,bw,sf,cr
    "rxdelay", "txdelay", "flood.max", "direct.txdelay", "owner.info",
    "path.hash.mode", "loop.detect", "tx", "freq",
    "public.key", "role", "bootloader.ver",
    "bridge.type", "bridge.enabled", "bridge.delay", "bridge.source",
    "bridge.baud", "bridge.channel", "bridge.secret",
    "wifi.enabled", "wifi.ssid", "wifi.password",
    "wifi.address", "wifi.netmask", "wifi.gateway", "wifi.dns", "wifi.status",
    "mqtt.enabled", "mqtt.host", "mqtt.port",
    "mqtt.user", "mqtt.password", "mqtt.client_id", "mqtt.topic_prefix",
    "mqtt.tls", "mqtt.publish_rx", "mqtt.publish_tx", "mqtt.subscribe",
    "mqtt.status",
]

# Properties writable via `set <prop> <value>` (subset of GET_PROPS — these are
# the ones with a corresponding set handler).
SET_PROPS = [
    "af", "int.thresh", "agc.reset.interval", "multi.acks",
    "allow.read.only", "flood.advert.interval", "advert.interval",
    "guest.password", "name", "repeat", "lat", "lon",
    "radio.rxgain", "radio",
    "rxdelay", "txdelay", "flood.max", "direct.txdelay", "owner.info",
    "path.hash.mode", "loop.detect", "tx", "freq",
    "adc.multiplier",
    "bridge.enabled", "bridge.delay", "bridge.source",
    "bridge.baud", "bridge.channel", "bridge.secret",
    "wifi.enabled", "wifi.ssid", "wifi.password",
    "wifi.address", "wifi.netmask", "wifi.gateway", "wifi.dns",
    "mqtt.enabled", "mqtt.host", "mqtt.port",
    "mqtt.user", "mqtt.password", "mqtt.client_id", "mqtt.topic_prefix",
    "mqtt.tls", "mqtt.publish_rx", "mqtt.publish_tx", "mqtt.subscribe",
]

# Top-level commands (not `get` / `set`).
TOP = [
    "get", "set",
    "help", "?",          # intercepted client-side
    "exit", "quit",       # intercepted client-side
    "ver", "board",
    "advert", "advert.zerohop",
    "reboot", "shutdown", "poweroff", "clkreboot",
    "neighbors", "neighbor.remove",
    "clear stats",
    "clock", "clock sync", "time",
    "start ota",
    "password",
    "region", "gps", "sensor",
    "tempradio",
    "powersaving on", "powersaving off",
    "log start", "log stop", "log erase", "log",
    "stats-packets", "stats-radio", "stats-core",
]

# What --get-all queries, in display order. Each entry is the full command
# to send to the firmware (some are top-level like "ver", not "get ver").
GET_ALL_COMMANDS = [
    "ver",
    "board",                     # hardware manufacturer/model
    "get role",
    "get name",
    "get public.key",
    "get lat",
    "get lon",
    "clock",                     # current RTC time -- useful for sync check
    "get freq",
    "get radio",                 # gives freq,bw,sf,cr in one line
    "get tx",
    "get radio.rxgain",
    "get repeat",
    "get advert.interval",
    "get flood.advert.interval",
    "get flood.max",
    "get path.hash.mode",
    "get loop.detect",
    "get rxdelay",
    "get txdelay",
    "get direct.txdelay",
    "get af",
    "get int.thresh",
    "get multi.acks",
    "get bridge.type",           # rs232 | espnow | none -- which legacy bridge built in
    "get wifi.enabled",
    "get wifi.ssid",
    "get wifi.password",
    "get wifi.address",
    "get wifi.netmask",
    "get wifi.gateway",
    "get wifi.dns",
    "get wifi.status",
    "get mqtt.enabled",
    "get mqtt.host",
    "get mqtt.port",
    "get mqtt.user",
    "get mqtt.password",
    "get mqtt.client_id",
    "get mqtt.topic_prefix",
    "get mqtt.tls",
    "get mqtt.publish_rx",
    "get mqtt.publish_tx",
    "get mqtt.subscribe",
    "get mqtt.status",
]


# ---- Client-side commands (intercepted before sending to firmware) -------------

class ConsoleExit(Exception):
    """Raised by intercept() when the user types exit/quit/q."""
    pass




HELP_OVERVIEW = """\
meshcore console — client-side help (the firmware doesn't have its own).

Usage:
  help                  this overview
  help <topic>          details for one topic
  help all              everything (long)

Topics:
  radio        radio params (freq, bw, sf, cr, tx, gain)
  routing      flood/path tuning (flood.max, path.hash.mode, loop.detect, ...)
  wifi         WiFi station config
  mqtt         MQTT bridge config
  bridge       legacy RS232/ESPNow bridge prefs
  system       node identity, role, advert, reboot, OTA, logs
  examples     a few useful command sequences

Other client-only commands:
  exit / quit            leave the interactive shell (^D also works)
  get all                full config dump (every interesting `get <prop>`)
  --get-all              same dump, as a one-shot CLI flag

Things to know:
  * Top-level commands (ver, advert, reboot, ...) are sent without `get`/`set`.
  * `set <prop>` takes a value; `get <prop>` returns the current value.
  * Tab completes top-level verbs at start-of-line, and property names after
    `get ` or `set `.
"""

HELP_TOPICS = {
    "radio": """\
Radio configuration:

  get freq                 current center frequency in MHz
  set freq <MHz>           change frequency (requires reboot)
  get radio                bundled view: freq,bw,sf,cr
  set radio <f,bw,sf,cr>   set all four at once, e.g. set radio 910.525,62.5,7,5
  get tx                   TX power in dBm
  set tx <dBm>             change TX power
  get radio.rxgain         boosted RX gain (SX1262/SX1268 only)
  set radio.rxgain on|off
""",
    "routing": """\
Routing / flood tuning:

  get repeat               whether this node forwards (on/off)
  set repeat on|off
  get flood.max            max hop count for flood packets (default 64)
  set flood.max <N>
  get advert.interval      zero-hop advert interval in minutes
  set advert.interval <N>
  get flood.advert.interval  flood advert interval in hours
  set flood.advert.interval <N>
  get path.hash.mode       0=1-byte, 1=2-byte, 2=3-byte path hashes
  set path.hash.mode <N>
  get loop.detect          off|minimal|moderate|strict
  set loop.detect <mode>
  get rxdelay              base RX delay factor
  get txdelay              TX delay factor (flood)
  get direct.txdelay       TX delay factor (direct)
""",
    "wifi": """\
WiFi station config (ESP32 only):

  set wifi.ssid <ssid>
  set wifi.password <pass>
  set wifi.address <a.b.c.d>     static IP; 0.0.0.0 = DHCP
  set wifi.netmask <a.b.c.d>
  set wifi.gateway <a.b.c.d>
  set wifi.dns <a.b.c.d>
  set wifi.enabled on|off
  get wifi.status                live state (connected/disconnected, IP, RSSI)
""",
    "mqtt": """\
MQTT bridge config:

  set mqtt.host <hostname-or-ip>   set "" to clear
  set mqtt.port <port>             0 = use default (1883 plain / 8883 TLS)
  set mqtt.user <user>           blank = anonymous
  set mqtt.password <pass>
  set mqtt.tls on|off            insecure-mode TLS (no cert verify) for v1
  set mqtt.client_id <id>        blank = "meshcore-<first 8 hex of pubkey>"
  set mqtt.topic_prefix <pfx>    blank = "meshcore/{client_id}"
  set mqtt.publish_rx on|off     publish heard packets to <prefix>/rx
  set mqtt.publish_tx on|off     publish transmitted packets to <prefix>/tx
  set mqtt.subscribe <topic>     blank = "<prefix>/rf"; wildcards (+, #) OK
  set mqtt.enabled on|off

  get mqtt.status                connection state + resolved subscribe topic

Placeholders in mqtt.client_id, mqtt.topic_prefix, and mqtt.subscribe:
  {client_id}  -> the resolved client_id (set value, or "meshcore-<8hex>")
                  Valid in topic_prefix and subscribe.
                  In mqtt.client_id itself it's passed through literally
                  to avoid recursion -- use {pubkey} there instead.
  {pubkey}     -> first 8 hex chars of this device's pubkey
                  Valid everywhere.

Examples:
  set mqtt.client_id rover-{pubkey}             # -> "rover-10db83e6"
  set mqtt.topic_prefix meshedup/{client_id}    # auto-fills the client id
  set mqtt.topic_prefix mything/{pubkey}        # uses raw 8-hex pubkey prefix
  set mqtt.subscribe meshedup/+/tx              # any peer's TX (no placeholder)
""",
    "bridge": """\
Legacy bridge config (RS232 / ESPNow):

  get bridge.type                rs232 | espnow | none
  set bridge.enabled on|off
  set bridge.delay <ms>          0-10000, default 500
  set bridge.source rx|tx        publish on logRx or logTx side
  set bridge.baud <rate>         RS232 only: 9600/19200/38400/57600/115200
  set bridge.channel <1-14>      ESPNow only
  set bridge.secret <key>        ESPNow only (up to 15 chars)
""",
    "system": """\
System / identity:

  ver                            firmware version + build date
  board                          board hardware name
  get name                       node name
  set name <name>                rename this node
  get role                       repeater | room_server | sensor | ...
  get lat / get lon              location (decimal degrees)
  set lat <deg> / set lon <deg>
  get public.key                 this node's full 32-byte pubkey
  password <admin_pw>            log in as admin (over-the-air)
  set password <new>             change admin password
  get guest.password / set guest.password <pw>
  set owner.info <text>          freeform contact info (| -> newline)

  advert                         send a flood advert
  advert.zerohop                 send a zero-hop advert (RF-local only)
  reboot                         reboot the device
  start ota                      enable softAP for OTA firmware update
  clock                          show current RTC time
  clock sync                     sync RTC over the air
  time <epoch>                   set RTC to a unix timestamp
  neighbors                      list known neighboring repeaters
  clear stats                    reset packet counters
  log start / log stop / log erase / log
  stats-packets / stats-radio / stats-core
""",
    "examples": """\
Common sequences:

  Bring up WiFi + a basic MQTT bridge (sniffer-friendly):
    set wifi.ssid YourNet
    set wifi.password YourPass
    set wifi.enabled on
    set mqtt.host broker.example.com
    set mqtt.port 1883
    set mqtt.enabled on

  Peer-pair two bridges (home + RV), no daemon needed:
    On home:
      set mqtt.client_id ks-home
      set mqtt.publish_tx on
      set mqtt.subscribe meshcore/ks-rv/tx
      set mqtt.enabled on
    On RV (mirror image):
      set mqtt.client_id ks-rv
      set mqtt.publish_tx on
      set mqtt.subscribe meshcore/ks-home/tx
      set mqtt.enabled on

  Dump everything for diagnostics:
    (run from your shell, not the device)
    python3 console.py --get-all
""",
}


def show_help(arg):
    arg = arg.strip().lower()
    if arg == "" or arg == "help":
        sys.stdout.write(HELP_OVERVIEW)
        return
    if arg == "all":
        for name, body in HELP_TOPICS.items():
            sys.stdout.write(f"--- {name} ---\n")
            sys.stdout.write(body)
            sys.stdout.write("\n")
        return
    if arg in HELP_TOPICS:
        sys.stdout.write(HELP_TOPICS[arg])
        return
    sys.stdout.write(f"no help topic '{arg}'. Try: help\n")


def intercept(line: str) -> bool:
    """
    Handle commands that should run client-side instead of being sent to the
    firmware. Returns True if intercepted (caller should NOT send to device).
    Raises ConsoleExit on exit/quit so the interactive loop can unwind.

    Note: `get all` is intercepted separately via intercept_with_serial()
    because it needs the serial port to run real `get` commands.
    """
    stripped = line.strip()
    if not stripped:
        return False
    tokens = stripped.split(None, 1)
    cmd = tokens[0].lower()
    rest = tokens[1] if len(tokens) > 1 else ""
    if cmd in ("help", "?"):
        show_help(rest)
        return True
    if cmd in ("exit", "quit"):
        raise ConsoleExit()
    return False


def intercept_with_serial(line: str, ser, settle: float) -> bool:
    """
    Like intercept(), but for commands that need the serial port. Handles
    `get all` -- runs the full config dump (same as the --get-all CLI flag).
    """
    if line.strip().lower() == "get all":
        run_get_all(ser, settle)
        return True
    return False


# ---- Tab completion ------------------------------------------------------------

class Completer:
    def __init__(self):
        self.matches = []

    def complete(self, text, state):
        if state == 0:
            line = readline.get_line_buffer()
            # Look at what comes before the word we're currently completing.
            # readline.get_begidx() points at the start of the current word,
            # so everything before that is fully-typed tokens (with any
            # trailing space stripped).
            before_cursor = line[:readline.get_begidx()]
            prior_tokens = before_cursor.split()

            if not prior_tokens:
                pool = TOP
            elif len(prior_tokens) == 1 and prior_tokens[0] == "get":
                pool = GET_PROPS
            elif len(prior_tokens) == 1 and prior_tokens[0] == "set":
                pool = SET_PROPS
            else:
                pool = TOP

            matches = [w for w in pool if w.startswith(text)]

            # When the match is unique, append a space so the user can type
            # the next argument without manually adding it. (Python's readline
            # binding doesn't always wire up completion_append_character on
            # every platform, and explicit is more reliable than implicit.)
            if len(matches) == 1:
                matches[0] = matches[0] + " "

            self.matches = matches
        return self.matches[state] if state < len(self.matches) else None


# ---- Serial I/O ----------------------------------------------------------------

def open_serial(port, baud):
    try:
        return serial.Serial(port, baud, timeout=0.05)
    except serial.SerialException as e:
        sys.stderr.write(f"error: failed to open {port} @ {baud}: {e}\n")
        sys.stderr.write("hint: another program may have the port open. "
                         "Close any minicom/screen/sniffer/IDE session and retry.\n")
        sys.exit(1)


class EchoFilter:
    """
    Line-buffered filter that drops the firmware's echo of the last command we
    sent. The firmware echoes each typed character then prints \\n, so the
    echoed line always appears as a complete line on its own. We compare each
    received line to the most recently sent command; if it matches, drop it
    (once) and pass the rest through unchanged.
    """

    def __init__(self):
        self._buf = b""
        self._suppress = None  # str (raw cmd) or None

    def expect_echo(self, cmd: str):
        self._suppress = cmd

    def feed(self, data: bytes) -> bytes:
        if not data:
            return b""
        self._buf += data
        out = bytearray()
        while True:
            idx = self._buf.find(b"\n")
            if idx < 0:
                # No complete line yet. Hold the partial in the buffer UNLESS
                # we're not suppressing anything -- in that case, flush so the
                # caller sees progress (e.g. async output without a newline).
                if self._suppress is None:
                    out.extend(self._buf)
                    self._buf = b""
                break
            line = bytes(self._buf[:idx + 1])
            self._buf = self._buf[idx + 1:]
            content = line.rstrip(b"\r\n").decode("utf-8", errors="replace")
            if self._suppress is not None and content == self._suppress:
                self._suppress = None
                continue  # drop this line entirely
            out.extend(line)
        return bytes(out)


def reader_loop(ser, stop_event, echo_filter=None):
    """Continuously print serial bytes to stdout. Runs in a background thread."""
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            data = ser.read(waiting if waiting else 1)
            if data:
                if echo_filter is not None:
                    data = echo_filter.feed(data)
                if data:
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
        except (serial.SerialException, OSError):
            break
        except Exception as e:
            sys.stderr.write(f"\nreader error: {e}\n")
            break


def send_line(ser, cmd):
    """Send a command followed by \\r (the firmware's line terminator)."""
    ser.write((cmd + "\r").encode("utf-8"))


def _strip_echo_prefix(raw: str, cmd: str) -> str:
    """
    Remove the echoed-back command from the start of a captured response.
    The firmware echoes the command verbatim, then '\\n', then the reply.
    """
    if raw.startswith(cmd):
        rest = raw[len(cmd):]
        # Eat the trailing \r and/or \n that the firmware appends after echo
        rest = rest.lstrip("\r\n")
        return rest
    return raw


def send_and_collect(ser, cmd, settle=0.1):
    """
    Send a command and return its reply with the echo stripped.

    Detection is event-driven: the firmware formats replies as
    `  -> <text>\\r\\n` (main.cpp prints "  -> " then Serial.println(reply)),
    so we return as soon as we see the marker followed by a newline.

    `settle` acts only as the no-reply fallback timeout -- commands that
    don't produce a reply (rare; `reboot` is an example) wait this long
    then give up. Reply-producing commands typically complete in 30-80 ms.

    Once we DO see the `-> ` marker, the deadline is extended by 0.2 s so
    a slow trailing newline doesn't cut off the reply.
    """
    ser.reset_input_buffer()
    send_line(ser, cmd)

    buf = b""
    saw_marker = False
    marker_end = 0
    deadline = time.time() + settle

    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue  # serial timeout (port has its own 50ms timeout); re-check deadline
        buf += chunk
        if not saw_marker:
            idx = buf.find(b"-> ")
            if idx >= 0:
                saw_marker = True
                marker_end = idx + 3
                # Extend the deadline now that we know a reply is on the way.
                deadline = max(deadline, time.time() + 0.2)
        if saw_marker:
            nl_idx = buf.find(b"\n", marker_end)
            if nl_idx >= 0:
                # Brief tail-drain for any straggling bytes from the firmware
                # (CRLF endings, trailing whitespace, etc).
                time.sleep(0.02)
                more = ser.read(ser.in_waiting or 0)
                if more:
                    buf += more
                break

    return _strip_echo_prefix(buf.decode("utf-8", errors="replace"), cmd)


# ---- History persistence -------------------------------------------------------

HISTFILE = os.path.expanduser("~/.meshcore_console_history")

def load_history():
    try:
        readline.read_history_file(HISTFILE)
    except (FileNotFoundError, OSError):
        pass

def save_history():
    try:
        readline.set_history_length(1000)
        readline.write_history_file(HISTFILE)
    except OSError:
        pass


# ---- Modes ---------------------------------------------------------------------

def run_one_shot(ser, cmd, settle):
    try:
        if intercept(cmd):
            return
    except ConsoleExit:
        return  # `--cmd exit` is a clean no-op
    if intercept_with_serial(cmd, ser, settle):
        return
    out = send_and_collect(ser, cmd, settle)
    sys.stdout.write(out)
    if not out.endswith("\n"):
        sys.stdout.write("\n")


def run_batch(ser, path, settle):
    with open(path) as f:
        for raw in f:
            cmd = raw.split("#", 1)[0].strip()
            if not cmd:
                continue
            sys.stdout.write(f"\n>>> {cmd}\n")
            sys.stdout.flush()
            try:
                if intercept(cmd):
                    continue
            except ConsoleExit:
                # An `exit` in a batch script stops further commands.
                return
            if intercept_with_serial(cmd, ser, settle):
                continue
            out = send_and_collect(ser, cmd, settle)
            sys.stdout.write(out)


def _extract_reply(raw, cmd):
    """
    Pull the firmware's reply out of the raw serial output for a single
    command. Firmware echoes the command, then prints "  -> <reply>".
    For `get` commands the reply itself is "> <value>" (we strip that
    leading "> ").
    """
    # Strip the echoed command if present (firmware echoes char-by-char)
    cleaned = raw.strip()
    if "->" in cleaned:
        cleaned = cleaned.split("->", 1)[1]
    cleaned = cleaned.strip()
    if cleaned.startswith("> "):
        cleaned = cleaned[2:]
    elif cleaned.startswith(">"):
        cleaned = cleaned[1:].lstrip()
    return cleaned


def _display_key(cmd):
    """Trim 'get ' prefix so the label column is clean."""
    if cmd.startswith("get "):
        return cmd[4:]
    return cmd


# When a pref is left blank, the firmware substitutes a default at connect
# time. The `get` command returns the literal stored value (empty), not the
# resolved one, so we show the *pattern* the firmware will use.
BLANK_DEFAULT_DESCRIPTIONS = {
    "mqtt.client_id":   '(default: "meshcore-{pubkey}")',
    "mqtt.topic_prefix": '(default: "meshcore/{client_id}"; supports {client_id}/{pubkey} placeholders)',
    "mqtt.subscribe":   '(default: "<topic_prefix>/rf"; supports {client_id}/{pubkey} placeholders)',
}

# Same idea but for numeric fields where "0" means "use default".
ZERO_DEFAULT_DESCRIPTIONS = {
    "mqtt.port":        '0  (default: 1883 if mqtt.tls=off, 8883 if mqtt.tls=on)',
}


def run_get_all(ser, settle):
    for cmd in GET_ALL_COMMANDS:
        raw = send_and_collect(ser, cmd, settle)
        value = _extract_reply(raw, cmd)
        key = _display_key(cmd)

        if value.startswith("??:") or value.startswith("unknown config"):
            display = "(not supported on this build)"
        elif value == "" and key in BLANK_DEFAULT_DESCRIPTIONS:
            display = BLANK_DEFAULT_DESCRIPTIONS[key]
        elif value == "0" and key in ZERO_DEFAULT_DESCRIPTIONS:
            display = ZERO_DEFAULT_DESCRIPTIONS[key]
        else:
            display = value

        print(f"{key:<24} {display}")


def run_interactive(ser, settle=0.1):
    stop = threading.Event()
    echo_filter = EchoFilter()
    rt = threading.Thread(target=reader_loop, args=(ser, stop, echo_filter), daemon=True)
    rt.start()

    completer = Completer()
    readline.set_completer(completer.complete)
    readline.set_completer_delims(" \t\n")
    readline.parse_and_bind("tab: complete")
    load_history()

    print(f"meshcore console on {ser.port} @ {ser.baudrate}")
    print("readline: ↑/↓ history, tab completes, ^D exits. Type `help` for commands.")
    print()

    try:
        while True:
            try:
                line = input("")
            except (EOFError, KeyboardInterrupt):
                break
            if not line.strip():
                continue
            try:
                if intercept(line):
                    continue
            except ConsoleExit:
                break
            if intercept_with_serial(line, ser, settle):
                continue
            echo_filter.expect_echo(line)
            send_line(ser, line)
            # Give the reader a moment to flush the firmware's reply
            # before the next prompt — reduces interleaving.
            time.sleep(0.3)
    finally:
        stop.set()
        time.sleep(0.1)
        save_history()
        print()


# ---- Main ----------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="MeshCore serial console (line-edited shell over /dev/tty*)"
    )
    ap.add_argument("--port", default="/dev/ttyACM0",
                    help="serial port (default /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200,
                    help="baud rate (default 115200)")
    ap.add_argument("--cmd",
                    help="send a single command, print response, exit")
    ap.add_argument("--file",
                    help="run commands from file (one per line; '#' for comments)")
    ap.add_argument("--get-all", action="store_true",
                    help="dump every known property via 'get <prop>'")
    ap.add_argument("--settle", type=float, default=0.1,
                    help="seconds to wait per command before reading the response "
                         "(default 0.1; the drain loop catches late stragglers)")
    args = ap.parse_args()

    # For a one-shot help / exit, skip opening the serial port entirely.
    if args.cmd:
        try:
            if intercept(args.cmd):
                return
        except ConsoleExit:
            return

    ser = open_serial(args.port, args.baud)
    try:
        if args.cmd:
            run_one_shot(ser, args.cmd, args.settle)
        elif args.file:
            run_batch(ser, args.file, args.settle)
        elif args.get_all:
            run_get_all(ser, args.settle)
        else:
            run_interactive(ser, args.settle)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
