#include "MeshProxy.h"
#include <Arduino.h>
#include <string.h>
#include "../../companion_radio/MyMesh.h"   // full backend type (backend side only)

// The mesh backend instance (defined in companion_radio/main.cpp) and the
// variant radio controls (target.cpp). Reached only from this module — the UI
// never touches them.
extern MyMesh the_mesh;
extern void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
extern void radio_set_tx_power(int8_t dbm);
extern void radio_sleep();
extern void radio_standby();

namespace mproxy {

static constexpr int CMD_Q_LEN = 12;
static constexpr int EV_Q_LEN  = 16;
static constexpr uint8_t CONTACT_FLAG_FAVOURITE = 0x01;   // matches firmware
static constexpr int LOOKUP_PREFIX = 6;                   // prefix bytes that identify a contact

// ---- Concurrency-safe snapshot publish (triple-buffer + pin) ----------------
// The backend (core 0) rebuilds the snapshot while the UI (core 1) reads it, so
// we never mutate a buffer the UI might be reading. Three PSRAM buffers:
//   * s_published — the newest complete snapshot,
//   * the UI pins s_published for the duration of one UI pass (begin/endUiRead),
//   * the backend builds into whichever buffer is neither published nor pinned.
// With 3 buffers there is always a free one. A tiny mutex guards only the index
// bookkeeping (the swap + the pin grab) — never the rebuild itself.
static MeshSnapshot*    s_buf[3] = {nullptr, nullptr, nullptr};
static volatile int     s_published = 0;
static volatile int     s_pinned    = -1;
static const MeshSnapshot* s_ui_cur = nullptr;   // pinned buffer for the current UI pass (UI thread only)
static SemaphoreHandle_t s_snap_mtx = nullptr;
static volatile uint32_t s_version  = 0;

static QueueHandle_t s_cmd_q  = nullptr;
static QueueHandle_t s_ev_q   = nullptr;
static uint32_t s_last_sig    = 0;
static uint32_t s_last_check_ms = 0;

// Live node/radio stats: refreshed by the backend each loop, read by the UI's
// node-info screen. Kept OUT of the snapshot signature (these change every second
// and would otherwise force constant republishes). Guarded by s_snap_mtx; the data
// is display-only so the tiny critical section is plenty.
static NodeStats s_stats = {};

// Buffer the UI reads from: the pinned one during a pass, else the published one.
static inline const MeshSnapshot* readBuf() {
  return s_ui_cur ? s_ui_cur : s_buf[s_published];
}

// ---- init ------------------------------------------------------------------
bool init() {
  for (int i = 0; i < 3; i++) {
    s_buf[i] = (MeshSnapshot*)ps_malloc(sizeof(MeshSnapshot));
    if (!s_buf[i]) s_buf[i] = (MeshSnapshot*)malloc(sizeof(MeshSnapshot));
    if (!s_buf[i]) return false;
    memset(s_buf[i], 0, sizeof(MeshSnapshot));
    s_buf[i]->has_connection = true;
  }
  s_published = 0; s_pinned = -1; s_version = 0;
  s_snap_mtx = xSemaphoreCreateMutex();
  s_cmd_q = xQueueCreate(CMD_Q_LEN, sizeof(MeshCmd));
  s_ev_q  = xQueueCreate(EV_Q_LEN, sizeof(UiEvent));
  return s_snap_mtx && s_cmd_q && s_ev_q;
}

// ---- backend: snapshot publish ---------------------------------------------
static uint32_t fnv(uint32_t sig, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) sig = (sig ^ p[i]) * 16777619u;
  return sig;
}
static uint32_t fnvStr(uint32_t sig, const char* s) {
  for (const char* p = s; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
  return sig;
}

// Cheap change-detect over everything the UI mirrors: contacts (count, flags,
// pubkey, name, override), prefs bytes, channel names. Excludes last_advert so
// routine RF adverts don't force rebuilds (matches the old contactsSignature).
static uint32_t computeSig(MyMesh& mesh) {
  uint32_t sig = 2166136261u;
  int n = mesh.getNumContacts();
  ContactInfo c;
  for (int i = 0; i < n; i++) {
    if (!mesh.getContactByIdx(i, c)) continue;
    sig = fnv(sig, &c.flags, 1);
    sig = fnv(sig, c.id.pub_key, 4);
    sig = fnvStr(sig, c.name);
    // Out path: so a learned/changed route (Flood<->Direct<->N hops) republishes
    // and the chat header's route line stays live. out_path_len is ENCODED (hop count
    // | hash_size<<6), so the real byte length is count*size -- never out_path_len.
    sig = fnv(sig, &c.out_path_len, 1);
    if (c.out_path_len != OUT_PATH_UNKNOWN) {
      int blen = (c.out_path_len & 63) * ((c.out_path_len >> 6) + 1);
      if (blen > (int)sizeof(c.out_path)) blen = sizeof(c.out_path);
      if (blen > 0) sig = fnv(sig, c.out_path, blen);
    }
    char ov[32];
    if (mesh.getNameOverride(c.id.pub_key, ov, sizeof(ov))) sig = fnvStr(sig, ov);
  }
  sig ^= (uint32_t)(n << 1);
  const NodePrefs* pr = mesh.getNodePrefs();
  sig = fnv(sig, (const uint8_t*)pr, sizeof(NodePrefs));
  ChannelDetails ch;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++)
    if (mesh.getChannel(i, ch) && ch.name[0]) sig = fnvStr(sig, ch.name);
  return sig;
}

// Fill `dst` from the live mesh. Backend-exclusive (dst is neither published nor
// pinned), so no lock is needed here -- only around the index swap afterwards.
static void rebuild(MyMesh& mesh, MeshSnapshot* dst) {
  memcpy(dst->self_pub_key, mesh.self_id.pub_key, PUB_KEY_SIZE);
  dst->has_connection = true;
  dst->prefs = *mesh.getNodePrefs();
  int n = mesh.getNumContacts();
  if (n > MAX_CONTACTS) n = MAX_CONTACTS;
  dst->contact_count = (uint16_t)n;
  for (int i = 0; i < n; i++) {
    ContactSnap& cs = dst->contacts[i];
    if (!mesh.getContactByIdx(i, cs.c)) { cs.c.name[0] = 0; cs.c.id.pub_key[0] = 0; }
    cs.name_override[0] = 0;
    mesh.getNameOverride(cs.c.id.pub_key, cs.name_override, sizeof(cs.name_override));
  }
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++)
    if (!mesh.getChannel(i, dst->channels[i])) dst->channels[i].name[0] = 0;
}

bool publishIfChanged(MyMesh& mesh) {
  if (!s_buf[0]) return false;
  uint32_t now = millis();
  if (s_version != 0 && (now - s_last_check_ms) < 500) return false;  // throttle the scan
  s_last_check_ms = now;
  uint32_t sig = computeSig(mesh);
  if (s_version != 0 && sig == s_last_sig) return false;
  s_last_sig = sig;

  // Pick a buffer that is neither published nor pinned (always exists with 3).
  int build;
  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  for (build = 0; build < 3; build++)
    if (build != s_published && build != s_pinned) break;
  xSemaphoreGive(s_snap_mtx);

  rebuild(mesh, s_buf[build]);   // no lock: this buffer is backend-exclusive

  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  s_buf[build]->version = ++s_version;
  s_published = build;
  xSemaphoreGive(s_snap_mtx);
  return true;
}

// ---- backend: command execution --------------------------------------------
static void execCommand(MyMesh& mesh, const MeshCmd& cmd) {
  switch (cmd.kind) {
    case CmdKind::Send: {
      uint32_t ts = mesh.getRTCClock()->getCurrentTimeUnique();
      bool ok = false; uint32_t ack = 0, timeout = 0;
      if (cmd.is_channel) {
        ChannelDetails ch;
        const char* me = mesh.getNodePrefs()->node_name;
        if (mesh.getChannel(cmd.channel_idx, ch))
          ok = mesh.sendGroupMessage(ts, ch.channel, (me && me[0]) ? me : "Me",
                                     cmd.text, strlen(cmd.text));
      } else {
        ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
        if (c) {
          int r = mesh.sendMessage(*c, ts, 0, cmd.text, ack, timeout);
          ok = (r != MSG_SEND_FAILED);
          if (ok && ack) mesh.addExpectedAck(ack, c);
        }
      }
      UiEvent ev{};
      ev.kind = EvKind::SendResult; ev.token = cmd.token;
      ev.ok = ok; ev.ack = ack; ev.timeout = timeout;
      pushEvent(ev);
      break;
    }
    case CmdKind::SaveGps: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) { c->gps_lat = cmd.gps_lat; c->gps_lon = cmd.gps_lon; mesh.saveContact(*c); }
      break;
    }
    case CmdKind::ToggleFav: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) { c->flags ^= CONTACT_FLAG_FAVOURITE; mesh.saveContact(*c); }
      break;
    }
    case CmdKind::SetNameOvr:
      mesh.setNameOverride(cmd.pubkey, cmd.name);   // "" clears the override
      break;
    case CmdKind::ResetPath: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) { mesh.resetPathTo(*c); mesh.saveContact(*c); }
      break;
    }
    case CmdKind::SetPath: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) {
        uint8_t len = cmd.path_len; if (len > MAX_PATH_SIZE) len = MAX_PATH_SIZE;
        memcpy(c->out_path, cmd.path, len);
        c->out_path_len = len;
        mesh.saveContact(*c);
      }
      break;
    }
    case CmdKind::AddContact:
      mesh.addContact(cmd.contact);
      break;
    case CmdKind::AddChannel: {
      // name = channel name, path = raw secret bytes (path_len = 16 or 32, validated
      // UI-side). loadChannels() fills slots via setChannel() WITHOUT tracking
      // num_channels, so addChannel(num_channels) could clobber a loaded channel --
      // append at the first free slot (empty name) instead. setChannel derives the
      // hash (128- vs 256-bit by the trailing 16 bytes).
      ChannelDetails probe;
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        if (mesh.getChannel(i, probe) && probe.name[0] == 0) {
          ChannelDetails nc;
          memset(&nc, 0, sizeof(nc));
          strncpy(nc.name, cmd.name, sizeof(nc.name) - 1);
          uint8_t len = cmd.path_len;
          if (len > sizeof(nc.channel.secret)) len = sizeof(nc.channel.secret);
          memcpy(nc.channel.secret, cmd.path, len);
          mesh.setChannel(i, nc);
          mesh.saveChannels();
          break;
        }
      }
      break;
    }
    case CmdKind::RenameChannel: {
      // Find the channel by its secret (the identity; the name is just a local
      // label) and re-store it with the new name. setChannel re-derives the
      // (unchanged) hash from the secret.
      ChannelDetails ch;
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        if (!mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
        if (memcmp(ch.channel.secret, cmd.path, sizeof(ch.channel.secret)) == 0) {
          strncpy(ch.name, cmd.name, sizeof(ch.name) - 1);
          ch.name[sizeof(ch.name) - 1] = 0;
          mesh.setChannel(i, ch);
          mesh.saveChannels();
          break;
        }
      }
      break;
    }
    case CmdKind::ServerLogin: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) {
        mesh.loginToServer(*c, cmd.password);        // reply -> onContactResponse -> _ui->loginResult
#ifdef ENABLE_LOGIN_STORE
        if (cmd.save_login || cmd.auto_login) mesh.saveLogin(cmd.pubkey, cmd.password, cmd.auto_login);
#endif
      }
      break;
    }
    case CmdKind::SendCommand: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) mesh.sendCliCommand(*c, cmd.text);      // reply -> onCommandDataRecv -> CLI_DATA message
      break;
    }
    case CmdKind::AutoLogin: {
#ifdef ENABLE_LOGIN_STORE
      const LoginCred* cred = mesh.findLoginCred(cmd.pubkey);  // only if saved + auto
      if (cred && cred->autolog) {
        ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
        if (c) mesh.loginToServer(*c, cred->password);
      }
#endif
      break;
    }
    case CmdKind::ReqTelem: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) mesh.requestTelemetry(*c);
      break;
    }
    case CmdKind::ShareZhop: {
      ContactInfo* c = mesh.lookupContactByPubKey(cmd.pubkey, LOOKUP_PREFIX);
      if (c) mesh.shareContactZeroHop(*c);
      break;
    }
    case CmdKind::Advert:
      mesh.advert();        // zero-hop self advert
      break;
    case CmdKind::AdvertFlood:
      mesh.advertFlood();   // flooded self advert
      break;
    case CmdKind::UpdatePrefs:
      *mesh.getNodePrefs() = cmd.prefs;
      mesh.savePrefs();
      break;
    case CmdKind::ApplyRadio: {
      NodePrefs* p = mesh.getNodePrefs();
      *p = cmd.prefs;
      radio_set_params(p->freq, p->bw, p->sf, p->cr);
      radio_set_tx_power(p->tx_power_dbm);
      mesh.savePrefs();
      break;
    }
    case CmdKind::SetRadio: {
      NodePrefs* p = mesh.getNodePrefs();
      p->radio_off = cmd.prefs.radio_off;
      mesh.savePrefs();
      if (p->radio_off) {
        radio_sleep();                                  // stop TX/RX (loop is gated in meshTask)
      } else {
        radio_standby();                                // wake, then re-apply the modem config
        radio_set_params(p->freq, p->bw, p->sf, p->cr);
        radio_set_tx_power(p->tx_power_dbm);
      }
      break;
    }
    case CmdKind::SetMute:
      mesh.setMute(cmd.name, cmd.muted);   // persists to /mutes
      break;
    case CmdKind::RemoveContact:
      mesh.removeContactAndPersist(cmd.pubkey);
      break;
    case CmdKind::ImportKey:
      // Adopt the new identity (cmd.path = 64-byte private key) and persist it, then
      // reboot so everything re-inits cleanly against it. On a bad key, do nothing.
      if (mesh.importPrivateKey(cmd.path)) {
        delay(150);          // let the SD write settle before the reset
        esp_restart();
      }
      break;
    case CmdKind::ApplyWifi:
      *mesh.getNodePrefs() = cmd.prefs;
      mesh.savePrefs();
#if defined(WITH_WIFI) && defined(ESP32)
      mesh.applyWifiConfig();
      mesh.applyNtpConfig();      // pick up ntp_enabled / server changes
#endif
      break;
    case CmdKind::SyncNtp:
#if defined(WITH_WIFI) && defined(ESP32)
      mesh.syncNtpNow();
#endif
      break;
    case CmdKind::UpdatePresets:
#if defined(WITH_WIFI) && defined(ESP32)
      mesh.updateRadioPresets();   // HTTPS fetch + write /radio_presets (backend core)
#endif
      break;
    case CmdKind::OtaUpdate:
#if defined(WITH_WIFI) && defined(ESP32)
      mesh.otaFromUrl();           // download + write OTA partition + reboot (backend core; blocks)
#endif
      break;
    case CmdKind::ApplyMqtt:
      *mesh.getNodePrefs() = cmd.prefs;
      mesh.savePrefs();
#if defined(WITH_MQTT_BRIDGE)
      mesh.applyMqttConfig();
#endif
      break;
    case CmdKind::RemoveChannel: {
      ChannelDetails ch;
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        if (!mesh.getChannel(i, ch) || ch.name[0] == 0) continue;
        if (memcmp(ch.channel.secret, cmd.path, sizeof(ch.channel.secret)) == 0) {
          ChannelDetails empty; memset(&empty, 0, sizeof(empty));   // name[0]=0 -> empty slot
          mesh.setChannel(i, empty);
          mesh.saveChannels();
          break;
        }
      }
      break;
    }
  }
}

static MyMesh* s_backend = nullptr;   // captured for read-only backend accessors (mutes)
void setBackend(MyMesh& mesh) { s_backend = &mesh; }   // call before the UI seeds (begin), since
                                                       // drainCommands (which also sets it) runs later
void drainCommands(MyMesh& mesh) {
  s_backend = &mesh;
  if (!s_cmd_q) return;
  MeshCmd cmd;
  while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE) execCommand(mesh, cmd);
}

void pushEvent(const UiEvent& ev) {
  if (s_ev_q) xQueueSend(s_ev_q, &ev, 0);
}

const char* firmwareVersion() { return FIRMWARE_VERSION; }   // from MyMesh.h

void updateStats(MyMesh& mesh) {
  NodeStats t;
  mesh.getNodeStats(t);                 // reads dispatcher/radio counters (backend core)
  if (!s_snap_mtx) { s_stats = t; return; }
  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  s_stats = t;
  xSemaphoreGive(s_snap_mtx);
}

// ---- UI side ---------------------------------------------------------------
void beginUiRead() {
  if (!s_snap_mtx) return;
  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  s_pinned = s_published;
  s_ui_cur = s_buf[s_pinned];
  xSemaphoreGive(s_snap_mtx);
}
void endUiRead() {
  if (!s_snap_mtx) return;
  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  s_pinned = -1;
  s_ui_cur = nullptr;
  xSemaphoreGive(s_snap_mtx);
}

// Version of the buffer the UI is actually reading (pinned during a pass), so the
// UI's rebuild-on-change compares against what it rendered -- not a newer version
// the backend may have published after the pin (which the UI hasn't seen yet).
uint32_t snapshotVersion() { const MeshSnapshot* s = readBuf(); return s ? s->version : 0; }
bool     hasConnection()   { const MeshSnapshot* s = readBuf(); return s ? s->has_connection : true; }
const uint8_t* selfPubKey() { const MeshSnapshot* s = readBuf(); return s ? s->self_pub_key : nullptr; }
// Seed the UI's in-RAM muted set at begin (backend captured in drainCommands, primed first).
int copyMutedKeys(char out[][CHAT_PEER_NAME_MAX], int max) {
  if (!s_backend || max <= 0) return 0;
  int n = s_backend->numMutes();
  if (n > max) n = max;
  for (int i = 0; i < n; i++) { strncpy(out[i], s_backend->muteKey(i), CHAT_PEER_NAME_MAX - 1); out[i][CHAT_PEER_NAME_MAX-1]=0; }
  return n;
}
int copyUnmutedKeys(char out[][CHAT_PEER_NAME_MAX], int max) {
  if (!s_backend || max <= 0) return 0;
  int n = s_backend->numUnmutes();
  if (n > max) n = max;
  for (int i = 0; i < n; i++) { strncpy(out[i], s_backend->unmuteKey(i), CHAT_PEER_NAME_MAX - 1); out[i][CHAT_PEER_NAME_MAX-1]=0; }
  return n;
}
// Copy our 64-byte private key out for the UI export popup (backend-thread read).
bool exportPrivKey(uint8_t out[64]) {
  if (!s_backend) return false;
  s_backend->exportPrivateKey(out);
  return true;
}

void wifiStatus(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if defined(WITH_WIFI) && defined(ESP32)
  if (s_backend) { s_backend->getWifiStatus(out, cap); return; }
#endif
  strncpy(out, "n/a", cap - 1); out[cap - 1] = 0;
}
void mqttStatus(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if defined(WITH_MQTT_BRIDGE)
  if (s_backend) { s_backend->getMqttStatus(out, cap); return; }
#endif
  strncpy(out, "n/a", cap - 1); out[cap - 1] = 0;
}
void ntpStatus(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if defined(WITH_WIFI) && defined(ESP32)
  if (s_backend) { s_backend->getNtpStatus(out, cap); return; }
#endif
  strncpy(out, "n/a", cap - 1); out[cap - 1] = 0;
}
void presetStatus(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if defined(WITH_WIFI) && defined(ESP32)
  if (s_backend) { s_backend->getPresetStatus(out, cap); return; }
#endif
  strncpy(out, "n/a", cap - 1); out[cap - 1] = 0;
}
void otaStatus(char* out, size_t cap) {
  if (!out || cap == 0) return;
#if defined(WITH_WIFI) && defined(ESP32)
  if (s_backend) { s_backend->getOtaStatus(out, cap); return; }
#endif
  strncpy(out, "n/a", cap - 1); out[cap - 1] = 0;
}
void wifiIpInfo(char* ip, char* mask, char* gw, char* dns, size_t cap) {
  if (cap == 0) return;
  if (ip) ip[0] = 0; if (mask) mask[0] = 0; if (gw) gw[0] = 0; if (dns) dns[0] = 0;
#if defined(WITH_WIFI) && defined(ESP32)
  if (s_backend) s_backend->getWifiIpInfo(ip, mask, gw, dns, cap);
#endif
}
int  getNumContacts()       { const MeshSnapshot* s = readBuf(); return s ? s->contact_count : 0; }

bool getContactByIdx(int idx, ContactInfo& out) {
  const MeshSnapshot* s = readBuf();
  if (!s || idx < 0 || idx >= s->contact_count) return false;
  out = s->contacts[idx].c;
  return true;
}

const ContactInfo* lookupContactByPubKey(const uint8_t* key, int prefix_len) {
  const MeshSnapshot* s = readBuf();
  if (!s || !key) return nullptr;
  if (prefix_len > PUB_KEY_SIZE) prefix_len = PUB_KEY_SIZE;
  int n = s->contact_count;
  for (int i = 0; i < n; i++)
    if (memcmp(s->contacts[i].c.id.pub_key, key, prefix_len) == 0)
      return &s->contacts[i].c;
  return nullptr;
}

bool getChannel(int idx, ChannelDetails& out) {
  const MeshSnapshot* s = readBuf();
  if (!s || idx < 0 || idx >= MAX_GROUP_CHANNELS) return false;
  out = s->channels[idx];
  return true;
}

void getStats(NodeStats& out) {
  if (!s_snap_mtx) { out = s_stats; return; }
  xSemaphoreTake(s_snap_mtx, portMAX_DELAY);
  out = s_stats;
  xSemaphoreGive(s_snap_mtx);
}

bool getNameOverride(const uint8_t* pubkey, char* out, size_t cap) {
  const MeshSnapshot* s = readBuf();
  if (!s || !pubkey || !out || !cap) return false;
  int n = s->contact_count;
  for (int i = 0; i < n; i++) {
    if (memcmp(s->contacts[i].c.id.pub_key, pubkey, LOOKUP_PREFIX) == 0) {
      if (!s->contacts[i].name_override[0]) return false;
      strncpy(out, s->contacts[i].name_override, cap - 1);
      out[cap - 1] = 0;
      return true;
    }
  }
  return false;
}

const NodePrefs* prefsSnap() { const MeshSnapshot* s = readBuf(); return s ? &s->prefs : nullptr; }
uint32_t rtcSeconds()        { return the_mesh.getRTCClock()->getCurrentTime(); }

// ---- Signal-strength meter (SNR peak-hold-with-decay) ----------------------
// Two scalars: the held peak (dB) and when it was last raised. Written on the backend
// (noteRxSnr), read on the UI (signalLevelDb); a torn read just skews one display frame.
static const float SIG_FLOOR_DB = -30.0f;   // decays to here (well below the bottom bar) = empty
static volatile float    s_sig_peak_db = SIG_FLOOR_DB;
static volatile uint32_t s_sig_peak_ms = 0;
static volatile bool     s_sig_seen    = false;

// Hold the peak flat for hold_ms, then ramp linearly down to the floor over tau_ms.
static float sigDecay(float peak, uint32_t elapsed_ms, uint32_t hold_ms, uint32_t tau_ms) {
  if (elapsed_ms <= hold_ms) return peak;
  if (tau_ms == 0) return SIG_FLOOR_DB;
  float frac = (float)(elapsed_ms - hold_ms) / (float)tau_ms;
  if (frac >= 1.0f) return SIG_FLOOR_DB;
  return peak - (peak - SIG_FLOOR_DB) * frac;
}

void noteRxSnr(float snr_db, uint32_t now_ms, uint32_t hold_ms, uint32_t tau_ms) {
  float cur = s_sig_seen ? sigDecay(s_sig_peak_db, now_ms - s_sig_peak_ms, hold_ms, tau_ms)
                         : SIG_FLOOR_DB;
  // Only a stronger-than-current signal raises the meter (and re-arms the hold); weaker
  // packets don't extend it -- so it stays high only while strong signals keep arriving.
  if (!s_sig_seen || snr_db > cur) {
    s_sig_peak_db = snr_db;
    s_sig_peak_ms = now_ms;
    s_sig_seen    = true;
  }
}
float signalLevelDb(uint32_t now_ms, uint32_t hold_ms, uint32_t tau_ms) {
  if (!s_sig_seen) return -127.0f;
  return sigDecay(s_sig_peak_db, now_ms - s_sig_peak_ms, hold_ms, tau_ms);
}
bool signalHasData() { return s_sig_seen; }
const uint8_t* hookKey()     { return the_mesh.hookKey(); }
bool hookIsChannel()         { return the_mesh.hookIsChannel(); }

bool postCommand(const MeshCmd& cmd) { return s_cmd_q && xQueueSend(s_cmd_q, &cmd, 0) == pdTRUE; }
bool pollEvent(UiEvent& ev)          { return s_ev_q && xQueueReceive(s_ev_q, &ev, 0) == pdTRUE; }

}  // namespace mproxy
