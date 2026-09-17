#include <Arduino.h>
#include "DataStore.h"

#if defined(EXTRAFS) || defined(QSPIFLASH)
  #define MAX_BLOBRECS 100
#else
  #define MAX_BLOBRECS 20
#endif

DataStore::DataStore(FILESYSTEM& fs, mesh::RTCClock& clock) : _fs(&fs), _fsExtra(nullptr), _clock(&clock),
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    identity_store(fs, "")
#elif defined(RP2040_PLATFORM)
    identity_store(fs, "/identity")
#else
    identity_store(fs, "/identity")
#endif
{
}

#if defined(EXTRAFS) || defined(QSPIFLASH)
DataStore::DataStore(FILESYSTEM& fs, FILESYSTEM& fsExtra, mesh::RTCClock& clock) : _fs(&fs), _fsExtra(&fsExtra), _clock(&clock),
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    identity_store(fs, "")
#elif defined(RP2040_PLATFORM)
    identity_store(fs, "/identity")
#else
    identity_store(fs, "/identity")
#endif
{
}
#endif

static File openWrite(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(filename);
  return fs->open(filename, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
  return fs->open(filename, "w");
#else
  return fs->open(filename, "w", true);
#endif
}

// Open an existing file for in-place read/write WITHOUT truncating it.
static File openUpdate(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs->open(filename, FILE_O_WRITE);  // O_RDWR|O_CREAT, no truncate, pos 0
#else
  return fs->open(filename, "r+");          // read/write, no truncate, must exist
#endif
}

// Crash-safe full-file rewrite: callers write the new contents to "<name>.tmp",
// then commitTmp() swaps it over the live file. The live file is only removed once
// the temp is fully written + closed, so a reset can leave at most: the old-complete
// live file (crash before/while writing the temp), or the new-complete temp (crash
// during the swap) -- never a truncated live file. recoverTmp() promotes a leftover
// temp at load time. This replaces the old "remove + rewrite in place" that lost the
// whole file if interrupted mid-write. Uses only rename/exists/remove, which all
// supported filesystems (SPIFFS, LittleFS, FAT/SdFat, Adafruit InternalFS) provide.
static bool commitTmp(FILESYSTEM* fs, const char* tmp, const char* final) {
  fs->remove(final);
  if (fs->rename(tmp, final)) return true;
  fs->remove(tmp);   // rename failed: drop the temp rather than leave it stale
  return false;
}

// If a previous commit was interrupted after the live file was removed but before
// the rename finished, the only intact copy is the temp -- promote it before loading.
static void recoverTmp(FILESYSTEM* fs, const char* tmp, const char* final) {
  if (fs->exists(tmp) && !fs->exists(final)) fs->rename(tmp, final);
}

// One contact record on disk (see saveContacts layout). Fixed size lets us
// rewrite a single contact in place instead of the whole file.
#define CONTACT_RECORD_SIZE  (32 + 32 + 1 + 1 + 1 + 4 + 1 + 4 + 64 + 4 + 4 + 4)  // 152

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  static uint32_t _ContactsChannelsTotalBlocks = 0;
#endif

void DataStore::begin() {
#if defined(RP2040_PLATFORM)
  identity_store.begin();
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  _ContactsChannelsTotalBlocks = _getContactsChannelsFS()->_getFS()->cfg->block_count;
  checkAdvBlobFile();
  #if defined(EXTRAFS) || defined(QSPIFLASH)
  migrateToSecondaryFS();
  #endif
#else
  // init 'blob store' support
  _fs->mkdir("/bl");
#endif
}

#if defined(ESP32)
  #include <SPIFFS.h>
  #include <nvs_flash.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
  #elif defined(EXTRAFS)
    #include <CustomLFS.h>
  #else 
    #include <InternalFileSystem.h>
  #endif
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
int _countLfsBlock(void *p, lfs_block_t block){
      if (block > _ContactsChannelsTotalBlocks) {
        MESH_DEBUG_PRINTLN("ERROR: Block %d exceeds filesystem bounds - CORRUPTION DETECTED!", block);
        return LFS_ERR_CORRUPT;  // return error to abort lfs_traverse() gracefully
    }
  lfs_size_t *size = (lfs_size_t*) p;
  *size += 1;
    return 0;
}

lfs_ssize_t _getLfsUsedBlockCount(FILESYSTEM* fs) {
  lfs_size_t size = 0;
  int err = lfs_traverse(fs->_getFS(), _countLfsBlock, &size);
  if (err) {
    MESH_DEBUG_PRINTLN("ERROR: lfs_traverse() error: %d", err);
    return 0;
  }
  return size;
}
#endif

uint32_t DataStore::getStorageUsedKb() const {
#if defined(ESP32)
  return SPIFFS.usedBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.usedBytes = 0;
  _fs->info(info);
  return info.usedBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  const lfs_config* config = _getContactsChannelsFS()->_getFS()->cfg;
  int usedBlockCount = _getLfsUsedBlockCount(_getContactsChannelsFS());
  int usedBytes = config->block_size * usedBlockCount;
  return usedBytes / 1024;
#else
  return 0;
#endif
}

uint32_t DataStore::getStorageTotalKb() const {
#if defined(ESP32)
  return SPIFFS.totalBytes() / 1024;
#elif defined(RP2040_PLATFORM)
  FSInfo info;
  info.totalBytes = 0;
  _fs->info(info);
  return info.totalBytes / 1024;
#elif defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  const lfs_config* config = _getContactsChannelsFS()->_getFS()->cfg;
  int totalBytes = config->block_size * config->block_count;
  return totalBytes / 1024;
#else
  return 0;
#endif
}

File DataStore::openRead(const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return _fs->open(filename, FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  return _fs->open(filename, "r");
#else
  return _fs->open(filename, "r", false);
#endif
}

File DataStore::openRead(FILESYSTEM* fs, const char* filename) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs->open(filename, FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  return fs->open(filename, "r");
#else
  return fs->open(filename, "r", false);
#endif
}

File DataStore::createFile(const char* filename) {
  return openWrite(_fs, filename);
}

bool DataStore::removeFile(const char* filename) {
  return _fs->remove(filename);
}

bool DataStore::removeFile(FILESYSTEM* fs, const char* filename) {
  return fs->remove(filename);
}

bool DataStore::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  if (_fsExtra == nullptr) {
    return _fs->format();
  } else {
    return _fs->format() && _fsExtra->format();
  }
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  bool fs_success = ((fs::SPIFFSFS *)_fs)->format();
  esp_err_t nvs_err = nvs_flash_erase(); // no need to reinit, will be done by reboot
  return fs_success && (nvs_err == ESP_OK);
#else
  #error "need to implement format()"
#endif
}

bool DataStore::loadMainIdentity(mesh::LocalIdentity &identity) {
  return identity_store.load("_main", identity);
}

bool DataStore::saveMainIdentity(const mesh::LocalIdentity &identity) {
  return identity_store.save("_main", identity);
}

// Appended UI-field "unset" defaults. Pulled into one helper so they apply on BOTH paths: the
// file-read path (loadPrefsInt sets them before reading, so a shorter/old file leaves them at these
// values) AND a fresh device with no prefs file at all (loadPrefs calls this in the no-file case).
// Previously these lived only inside the file-read path, so a first boot with no file kept them 0
// from MyMesh's memset -> chat history + notifications + chat colors OFF and the signal meter zeroed.
// Keep in sync with the NodePrefs tail.
static void applyAppendedPrefsDefaults(NodePrefs& _prefs) {
  _prefs.display_brightness = 0;                                                         // 137
  _prefs.display_rotation = 0;                                                           // 138
  _prefs.contacts_order = 0xFF;                                                          // 139 (0xFF = unset)
  _prefs.contacts_filter = 0xFF;                                                         // 140 (0xFF = unset)
  _prefs.tz_offset_minutes = 0;                                                          // 141 (default UTC)
  _prefs.clock_12h = 0;                                                                  // 143 (default 24h)
  _prefs.persist_history = 0xFF;                                                         // 144 (0xFF = unset -> on)
  _prefs.screen_timeout_s = 0;                                                           // 145 (0 = never / unset)
  _prefs.radio_off = 0;                                                                  // 147 (default: radio enabled)
  _prefs.lock_pin[0] = 0;                                                                // 148 (default: no PIN)
  _prefs.notify_enable = 1;                                                              // 156 (default: notifications on)
  _prefs.avatar_palette = 0;                                                             // 157 (default: curated palette)
  _prefs.theme_name[0] = 0;                                                              // 158 (default: "" -> Dark)
  _prefs.mention_user_colors = 1;                                                        // 159 (default: on)
  _prefs.hashtag_channel_colors = 1;                                                     // 160 (default: on)
  _prefs.notify_mute_default = 0;                                                        // 161 (default: opt-out)
  _prefs.channel_sender_colors = 1;                                                      // 162 (default: on)
  _prefs.auto_lock = 0;                                                                  // 163 (default: manual lock only)
  _prefs.wifi_enabled = 0;                                                               // 164 (default: off)
  _prefs.wifi_ssid[0] = 0;                                                               // 165
  _prefs.wifi_password[0] = 0;                                                           // 166
  _prefs.mqtt_enabled = 0;                                                               // 167 (default: off)
  _prefs.mqtt_host[0] = 0;                                                               // 168
  _prefs.mqtt_port = 0;                                                                  // 169 (0 = protocol default)
  _prefs.mqtt_user[0] = 0;                                                               // 170
  _prefs.mqtt_password[0] = 0;                                                           // 171
  _prefs.mqtt_topic_prefix[0] = 0;                                                       // 172
  _prefs.mqtt_tls = 0;                                                                   // 173
  _prefs.mqtt_publish_rx = 1;                                                            // 174 (default: dump RX)
  _prefs.mqtt_publish_tx = 0;                                                            // 175
  _prefs.wifi_dhcp = 1;                                                                  // 176 (default: DHCP on)
  _prefs.wifi_dns_override = 0;                                                          // 177
  _prefs.wifi_ip = 0;                                                                    // 178
  _prefs.wifi_netmask = 0;                                                               // 179
  _prefs.wifi_gateway = 0;                                                               // 180
  _prefs.wifi_dns = 0;                                                                   // 181
  _prefs.ntp_enabled = 1;                                                                // 182 (default: on)
  _prefs.ntp_server[0] = 0;                                                              // 183 (empty -> pool.ntp.org)
  _prefs.use_rtc_clock = 0xFF;                                                           // 184 (unset -> on)
  _prefs.sigmeter_snr_min = -12;                                                         // 185 (default)
  _prefs.sigmeter_snr_max = 6;                                                           // 186 (default)
  _prefs.sigmeter_hold_s = 30;                                                           // 187 (default)
  _prefs.sigmeter_decay_s = 100;                                                         // 188 (default)
  _prefs.show_chat_meta = 0;                                                             // 189 (default: off)
  _prefs.ota_url[0] = 0;                                                                 // 190 (empty)
  _prefs.power_monitor = 0;                                                              // 191 (None)
  _prefs.batt_type = 0;                                                                  // 192 (1S Li-ion)
  _prefs.batt_capacity_mah = 0;                                                          // 193 (0 -> default)
  _prefs.batt_soc_pmille = 0xFFFF;                                                       // 194 (unset)
  _prefs.gps_uart = 1;                                                                   // 195 (UART1 17/18)
  _prefs.ota_prerelease = 0;                                                             // 196 (stable only)
  _prefs.ota_custom_url = 0;                                                             // 197 (GitHub release mode)
  _prefs.trackball_speed = 0;                                                            // 198 (0 -> UI default)
  _prefs.trackball_invert = 0;                                                           // 199 (normal direction)
  _prefs.font_scale = 0;                                                                 // 200 (auto by screen size)
  _prefs.touch_suppress_ms = 250;                                                        // 201 (default 250ms; 0 = off)
  _prefs.mqtt_client_id[0] = 0;                                                           // 202 (empty -> auto from pubkey)
  _prefs.mqtt_subscribe[0] = 0;                                                           // 203 (empty -> "<prefix>/rf")
  _prefs.trackball_sel_invert = 0;                                                        // 204 (normal selection dir)
  _prefs.tcp_companion = 0;                                                               // 205 (off; USB companion)
  _prefs.buzzer_volume = 0xFF;                                                            // 206 (0xFF unset -> 5)
  _prefs.ringtone_name[0] = 0;                                                            // 207 (empty -> "Default" alert)
  _prefs.audio_output = 0xFF;                                                             // 208 (0xFF unset -> piezo)
}

// Recover the fork's appended prefs (the 63 UI/WiFi/MQTT/battery/audio fields at
// byte 137+ of the legacy /new_prefs blob) for a node whose /prefs.json was
// written by a build that did not know about them -- i.e. any device that booted
// upstream's ConfigSerializer migration first. Upstream does not delete
// /new_prefs, so the originals are still on disk.
//
// Guarded so it can only ever help: it runs only when the marker field is still
// at its "never written" value, only on a legacy file long enough to contain the
// tail, and it persists once so it never runs again.
// Clamp every fork-added pref to a sane range after any legacy read. Nothing in
// the old byte format is self-describing, so a short, truncated or
// slightly-misaligned file turns straight into live settings -- that is how a
// test device silently ended up with radio_off=1 and an unusable radio.
// Booleans FAIL SAFE: anything that is not exactly the "off"/"on" value we expect
// resolves to the harmless choice, never to "disable the user's radio".
static void sanitiseAppendedPrefs(NodePrefs& p) {
  p.radio_off = (p.radio_off == 1) ? 1 : 0;          // garbage -> radio ON
  p.display_rotation = (p.display_rotation <= 4) ? p.display_rotation : 0;
  p.font_scale       = (p.font_scale <= 3) ? p.font_scale : 0;
  p.gps_uart         = (p.gps_uart <= 1) ? p.gps_uart : 1;
  p.contacts_order   = (p.contacts_order <= 2 || p.contacts_order == 0xFF) ? p.contacts_order : 0xFF;
  p.contacts_filter  = (p.contacts_filter <= 5 || p.contacts_filter == 0xFF) ? p.contacts_filter : 0xFF;
  p.persist_history  = (p.persist_history <= 1) ? p.persist_history : 0xFF;
  p.notify_enable    = (p.notify_enable <= 1) ? p.notify_enable : 1;
  p.use_rtc_clock    = (p.use_rtc_clock <= 1) ? p.use_rtc_clock : 0xFF;
  p.buzzer_volume    = (p.buzzer_volume <= 10) ? p.buzzer_volume : 0xFF;
  p.audio_output     = (p.audio_output <= 1) ? p.audio_output : 0xFF;
  p.power_monitor    = (p.power_monitor <= 1) ? p.power_monitor : 0;
  p.batt_type        = (p.batt_type <= 2) ? p.batt_type : 0;
  if (p.batt_capacity_mah > 30000) p.batt_capacity_mah = 0;          // 0 -> default
  if (p.batt_soc_pmille > 1000 && p.batt_soc_pmille != 0xFFFF) p.batt_soc_pmille = 0xFFFF;
  if (p.touch_suppress_ms > 1000) p.touch_suppress_ms = 250;
  if (p.screen_timeout_s > 3600)  p.screen_timeout_s = 0;            // 0 = never
  if (p.tz_offset_minutes < -840 || p.tz_offset_minutes > 840) p.tz_offset_minutes = 0;
  if (p.trackball_speed > 60) p.trackball_speed = 0;                 // 0 -> UI default
  if (p.sigmeter_snr_min < -30 || p.sigmeter_snr_min > 0)  p.sigmeter_snr_min = -12;
  if (p.sigmeter_snr_max < 0   || p.sigmeter_snr_max > 30) p.sigmeter_snr_max = 6;
  if (p.sigmeter_hold_s  == 0  || p.sigmeter_hold_s  > 600)  p.sigmeter_hold_s  = 30;
  if (p.sigmeter_decay_s == 0  || p.sigmeter_decay_s > 3600) p.sigmeter_decay_s = 100;
  // plain booleans
  p.clock_12h = !!p.clock_12h;              p.auto_lock = !!p.auto_lock;
  p.avatar_palette = !!p.avatar_palette;    p.show_chat_meta = !!p.show_chat_meta;
  p.mention_user_colors = !!p.mention_user_colors;
  p.hashtag_channel_colors = !!p.hashtag_channel_colors;
  p.notify_mute_default = !!p.notify_mute_default;
  p.channel_sender_colors = !!p.channel_sender_colors;
  p.ota_prerelease = !!p.ota_prerelease;    p.ota_custom_url = !!p.ota_custom_url;
  p.trackball_invert = !!p.trackball_invert;
  p.trackball_sel_invert = !!p.trackball_sel_invert;
  p.tcp_companion = !!p.tcp_companion;      p.ntp_enabled = !!p.ntp_enabled;
  p.wifi_enabled = !!p.wifi_enabled;        p.wifi_dhcp = !!p.wifi_dhcp;
  p.wifi_dns_override = !!p.wifi_dns_override;
  p.mqtt_enabled = !!p.mqtt_enabled;        p.mqtt_tls = !!p.mqtt_tls;
  p.mqtt_publish_rx = !!p.mqtt_publish_rx;  p.mqtt_publish_tx = !!p.mqtt_publish_tx;
  // strings must be NUL-terminated even if the file was truncated mid-field
  p.lock_pin[sizeof(p.lock_pin)-1] = 0;         p.theme_name[sizeof(p.theme_name)-1] = 0;
  p.wifi_ssid[sizeof(p.wifi_ssid)-1] = 0;       p.wifi_password[sizeof(p.wifi_password)-1] = 0;
  p.mqtt_host[sizeof(p.mqtt_host)-1] = 0;       p.mqtt_user[sizeof(p.mqtt_user)-1] = 0;
  p.mqtt_password[sizeof(p.mqtt_password)-1] = 0;
  p.mqtt_topic_prefix[sizeof(p.mqtt_topic_prefix)-1] = 0;
  p.mqtt_client_id[sizeof(p.mqtt_client_id)-1] = 0;
  p.mqtt_subscribe[sizeof(p.mqtt_subscribe)-1] = 0;
  p.ntp_server[sizeof(p.ntp_server)-1] = 0;     p.ota_url[sizeof(p.ota_url)-1] = 0;
  p.ringtone_name[sizeof(p.ringtone_name)-1] = 0;
}

void DataStore::rescueAppendedPrefs(NodePrefs& _prefs) {
  // Deterministic: a /prefs.json written by this fork always carries the current
  // cfgver. Anything else -- upstream firmware, an older fork build, or a
  // truncated/corrupt write -- does not, and its fork fields cannot be trusted.
  if (_prefs.cfg_version == COMPANION_PREFS_CFGVER) {
    return;   // already fork-written: leave everything alone
  }
  if (!_fs->exists("/new_prefs")) {
    applyAppendedPrefsDefaults(_prefs);   // nothing to recover -> at least get real defaults
    savePrefs(_prefs);
    return;
  }

  // Re-run the SAME legacy reader a normal migration uses, into a scratch object.
  // This is deliberate: an earlier version of this function duplicated the byte
  // offsets and seeked to a hard-coded 137, which was WRONG (the fork block starts
  // at 140 -- the "// nnn" comments in savePrefs drift by 3 from autoadd_config on,
  // and 68 of them are inaccurate). Everything it read was shifted, which silently
  // set radio_off=1 and filled the UI/battery/OTA fields with garbage on a real
  // device. There must be exactly ONE place that knows this layout.
  NodePrefs legacy;                      // ctor + loadPrefsInt apply proper defaults
  loadPrefsInt("/new_prefs", legacy);    // short/missing file -> fields stay default

  // Copy the fork-added fields. Upstream's own fields (radio, name, scope...) are
  // left as loaded: on a genuine upstream->fork upgrade those migrated correctly
  // and must be preserved. If the JSON itself was corrupt, the caller has already
  // replaced it wholesale from the legacy blob before we get here.
  _prefs.display_brightness = legacy.display_brightness;
  _prefs.display_rotation = legacy.display_rotation;
  _prefs.contacts_order = legacy.contacts_order;
  _prefs.contacts_filter = legacy.contacts_filter;
  _prefs.tz_offset_minutes = legacy.tz_offset_minutes;
  _prefs.clock_12h = legacy.clock_12h;
  _prefs.persist_history = legacy.persist_history;
  _prefs.screen_timeout_s = legacy.screen_timeout_s;
  _prefs.radio_off = legacy.radio_off;
  memcpy(_prefs.lock_pin, legacy.lock_pin, sizeof(_prefs.lock_pin));
  _prefs.notify_enable = legacy.notify_enable;
  _prefs.avatar_palette = legacy.avatar_palette;
  memcpy(_prefs.theme_name, legacy.theme_name, sizeof(_prefs.theme_name));
  _prefs.mention_user_colors = legacy.mention_user_colors;
  _prefs.hashtag_channel_colors = legacy.hashtag_channel_colors;
  _prefs.notify_mute_default = legacy.notify_mute_default;
  _prefs.channel_sender_colors = legacy.channel_sender_colors;
  _prefs.auto_lock = legacy.auto_lock;
  _prefs.wifi_enabled = legacy.wifi_enabled;
  memcpy(_prefs.wifi_ssid, legacy.wifi_ssid, sizeof(_prefs.wifi_ssid));
  memcpy(_prefs.wifi_password, legacy.wifi_password, sizeof(_prefs.wifi_password));
  _prefs.mqtt_enabled = legacy.mqtt_enabled;
  memcpy(_prefs.mqtt_host, legacy.mqtt_host, sizeof(_prefs.mqtt_host));
  _prefs.mqtt_port = legacy.mqtt_port;
  memcpy(_prefs.mqtt_user, legacy.mqtt_user, sizeof(_prefs.mqtt_user));
  memcpy(_prefs.mqtt_password, legacy.mqtt_password, sizeof(_prefs.mqtt_password));
  memcpy(_prefs.mqtt_topic_prefix, legacy.mqtt_topic_prefix, sizeof(_prefs.mqtt_topic_prefix));
  _prefs.mqtt_tls = legacy.mqtt_tls;
  _prefs.mqtt_publish_rx = legacy.mqtt_publish_rx;
  _prefs.mqtt_publish_tx = legacy.mqtt_publish_tx;
  _prefs.wifi_dhcp = legacy.wifi_dhcp;
  _prefs.wifi_dns_override = legacy.wifi_dns_override;
  _prefs.wifi_ip = legacy.wifi_ip;
  _prefs.wifi_netmask = legacy.wifi_netmask;
  _prefs.wifi_gateway = legacy.wifi_gateway;
  _prefs.wifi_dns = legacy.wifi_dns;
  _prefs.ntp_enabled = legacy.ntp_enabled;
  memcpy(_prefs.ntp_server, legacy.ntp_server, sizeof(_prefs.ntp_server));
  _prefs.use_rtc_clock = legacy.use_rtc_clock;
  _prefs.sigmeter_snr_min = legacy.sigmeter_snr_min;
  _prefs.sigmeter_snr_max = legacy.sigmeter_snr_max;
  _prefs.sigmeter_hold_s = legacy.sigmeter_hold_s;
  _prefs.sigmeter_decay_s = legacy.sigmeter_decay_s;
  _prefs.show_chat_meta = legacy.show_chat_meta;
  memcpy(_prefs.ota_url, legacy.ota_url, sizeof(_prefs.ota_url));
  _prefs.power_monitor = legacy.power_monitor;
  _prefs.batt_type = legacy.batt_type;
  _prefs.batt_capacity_mah = legacy.batt_capacity_mah;
  _prefs.batt_soc_pmille = legacy.batt_soc_pmille;
  _prefs.gps_uart = legacy.gps_uart;
  _prefs.ota_prerelease = legacy.ota_prerelease;
  _prefs.ota_custom_url = legacy.ota_custom_url;
  _prefs.trackball_speed = legacy.trackball_speed;
  _prefs.trackball_invert = legacy.trackball_invert;
  _prefs.font_scale = legacy.font_scale;
  _prefs.touch_suppress_ms = legacy.touch_suppress_ms;
  memcpy(_prefs.mqtt_client_id, legacy.mqtt_client_id, sizeof(_prefs.mqtt_client_id));
  memcpy(_prefs.mqtt_subscribe, legacy.mqtt_subscribe, sizeof(_prefs.mqtt_subscribe));
  _prefs.trackball_sel_invert = legacy.trackball_sel_invert;
  _prefs.tcp_companion = legacy.tcp_companion;
  _prefs.buzzer_volume = legacy.buzzer_volume;
  memcpy(_prefs.ringtone_name, legacy.ringtone_name, sizeof(_prefs.ringtone_name));
  _prefs.audio_output = legacy.audio_output;

  sanitiseAppendedPrefs(_prefs);
  savePrefs(_prefs);   // persist into /prefs.json so this never runs again
}

void DataStore::loadPrefs(NodePrefs& prefs) {
  // NOTE: deliberately NO recoverTmp() for /prefs.json.tmp. A crash during
  // saveSerial() leaves a TRUNCATED .tmp that is indistinguishable from the
  // (safe) crash-during-rename case, and promoting it silently installs a
  // corrupt config -- which is exactly how a test device ended up with a
  // bogus frequency. Losing the .tmp just falls back to the legacy migration
  // below, which is always recoverable because /new_prefs is never deleted.
  _fs->remove("/prefs.json.tmp");
  recoverTmp(_fs, "/new_prefs.tmp", "/new_prefs");   // crash-safe save: promote a leftover tmp

  if (_fs->exists("/prefs.json")) {
    File file = openRead(_fs, "/prefs.json");
    if (file) {
      // Check the return: a tokenizer error abandons the REST of the file, leaving every
      // later field at its default with no other symptom. Silent truncation here is how a
      // bad serializer key went unnoticed (see the charset warning in NodePrefs.h).
      if (!prefs.loadSerial(file)) {
        Serial.printf("[PREFS] WARNING: /prefs.json parse failed -- fields after the fault are DEFAULTS\n");
      }
      file.close();
    }
    // Sanity-check what we just loaded. A /prefs.json can be corrupt or partial --
    // a crashed save, a bad migration, an interrupted write -- and once it exists
    // the legacy branches below never run again, so a bad file would be permanent
    // and leave the node unusable (this is not hypothetical: a test device came up
    // with a bogus frequency and a disabled radio, and nothing could self-correct).
    // The radio config is the honest canary: no real node has an out-of-band
    // frequency or an impossible spreading factor.
    const bool json_sane = (prefs.freq >= 100.0f && prefs.freq <= 1000.0f
                            && prefs.sf >= 5 && prefs.sf <= 12
                            && prefs.cr >= 5 && prefs.cr <= 8
                            && prefs.bw >= 7.0f && prefs.bw <= 500.0f);
    if (!json_sane && _fs->exists("/new_prefs")) {
      // Rebuild wholesale from the legacy blob, which is still intact because we
      // never delete it. Strictly better than keeping a config we know is wrong.
      loadPrefsInt("/new_prefs", prefs);
      savePrefs(prefs);
    } else {
      // A node that already booted a ConfigSerializer build has a /prefs.json that
      // was written WITHOUT the fork's appended fields (upstream's legacy reader
      // stops at its own tail), so the legacy branches below never run again for
      // it. /new_prefs is deliberately not deleted, so recover them from there.
      rescueAppendedPrefs(prefs);
    }
  } else if (_fs->exists("/new_prefs")) {
    loadPrefsInt("/new_prefs", prefs);   // applies the appended defaults first
    savePrefs(prefs);                    // migrate to /prefs.json (keep /new_prefs)
  } else if (_fs->exists("/node_prefs")) {
    loadPrefsInt("/node_prefs", prefs);
    savePrefs(prefs);
    _fs->remove("/node_prefs"); // remove old
  } else {
    applyAppendedPrefsDefaults(prefs);   // fresh device, no prefs file -> still apply the UI defaults
  }
}

void DataStore::loadPrefsInt(const char *filename, NodePrefs& _prefs) {
  File file = openRead(_fs, filename);
  if (file) {
    uint8_t pad[8];

    file.read((uint8_t *)&_prefs.airtime_factor, sizeof(float));                           // 0
    file.read((uint8_t *)_prefs.node_name, sizeof(_prefs.node_name));                      // 4
    file.read(pad, 4);                                                                     // 36
    file.read((uint8_t *)&_prefs.node_lat, sizeof(_prefs.node_lat));                       // 40
    file.read((uint8_t *)&_prefs.node_lon, sizeof(_prefs.node_lon));                       // 48
    file.read((uint8_t *)&_prefs.freq, sizeof(_prefs.freq));                               // 56
    file.read((uint8_t *)&_prefs.sf, sizeof(_prefs.sf));                                   // 60
    file.read((uint8_t *)&_prefs.cr, sizeof(_prefs.cr));                                   // 61
    file.read((uint8_t *)&_prefs._client_repeat, sizeof(_prefs._client_repeat));             // 62
    file.read((uint8_t *)&_prefs.manual_add_contacts, sizeof(_prefs.manual_add_contacts)); // 63
    file.read((uint8_t *)&_prefs.bw, sizeof(_prefs.bw));                                   // 64
    file.read((uint8_t *)&_prefs.tx_power_dbm, sizeof(_prefs.tx_power_dbm));               // 68
    file.read((uint8_t *)&_prefs.telemetry_mode_base, sizeof(_prefs.telemetry_mode_base)); // 69
    file.read((uint8_t *)&_prefs.telemetry_mode_loc, sizeof(_prefs.telemetry_mode_loc));   // 70
    file.read((uint8_t *)&_prefs.telemetry_mode_env, sizeof(_prefs.telemetry_mode_env));   // 71
    file.read((uint8_t *)&_prefs.rx_delay_base, sizeof(_prefs.rx_delay_base));             // 72
    file.read((uint8_t *)&_prefs.advert_loc_policy, sizeof(_prefs.advert_loc_policy));     // 76
    file.read((uint8_t *)&_prefs.multi_acks, sizeof(_prefs.multi_acks));                   // 77
    file.read((uint8_t *)&_prefs.path_hash_mode, sizeof(_prefs.path_hash_mode));           // 78
    file.read(pad, 1);                                                                     // 79
    file.read((uint8_t *)&_prefs.ble_pin, sizeof(_prefs.ble_pin));                         // 80
    file.read((uint8_t *)&_prefs.buzzer_quiet, sizeof(_prefs.buzzer_quiet));               // 84
    file.read((uint8_t *)&_prefs.gps_enabled, sizeof(_prefs.gps_enabled));                 // 85
    file.read((uint8_t *)&_prefs.gps_interval, sizeof(_prefs.gps_interval));               // 86
    file.read((uint8_t *)&_prefs.autoadd_config, sizeof(_prefs.autoadd_config));           // 87
    file.read((uint8_t *)&_prefs.autoadd_max_hops, sizeof(_prefs.autoadd_max_hops));       // 88
    file.read((uint8_t *)&_prefs.rx_boosted_gain, sizeof(_prefs.rx_boosted_gain));         // 89
    file.read((uint8_t *)_prefs.default_scope_name, sizeof(_prefs.default_scope_name));    // 90
    file.read((uint8_t *)_prefs.default_scope_key, sizeof(_prefs.default_scope_key));     // 121
    // Appended UI fields: set their "unset" defaults BEFORE reading, so a pre-existing (shorter) file
    // leaves them at these values (file.read() returns 0 at EOF without touching the destination). Same
    // helper the no-file fresh-device path uses, so both agree.
    applyAppendedPrefsDefaults(_prefs);
    file.read((uint8_t *)&_prefs.display_brightness, sizeof(_prefs.display_brightness));   // 137
    file.read((uint8_t *)&_prefs.display_rotation, sizeof(_prefs.display_rotation));       // 138
    file.read((uint8_t *)&_prefs.contacts_order, sizeof(_prefs.contacts_order));           // 139
    file.read((uint8_t *)&_prefs.contacts_filter, sizeof(_prefs.contacts_filter));         // 140
    file.read((uint8_t *)&_prefs.tz_offset_minutes, sizeof(_prefs.tz_offset_minutes));     // 141
    file.read((uint8_t *)&_prefs.clock_12h, sizeof(_prefs.clock_12h));                     // 143
    file.read((uint8_t *)&_prefs.persist_history, sizeof(_prefs.persist_history));         // 144
    file.read((uint8_t *)&_prefs.screen_timeout_s, sizeof(_prefs.screen_timeout_s));       // 145
    file.read((uint8_t *)&_prefs.radio_off, sizeof(_prefs.radio_off));                     // 147
    file.read((uint8_t *)_prefs.lock_pin, sizeof(_prefs.lock_pin));                        // 148
    file.read((uint8_t *)&_prefs.notify_enable, sizeof(_prefs.notify_enable));             // 156
    file.read((uint8_t *)&_prefs.avatar_palette, sizeof(_prefs.avatar_palette));           // 157
    file.read((uint8_t *)_prefs.theme_name, sizeof(_prefs.theme_name));                    // 158
    file.read((uint8_t *)&_prefs.mention_user_colors, sizeof(_prefs.mention_user_colors)); // 159
    file.read((uint8_t *)&_prefs.hashtag_channel_colors, sizeof(_prefs.hashtag_channel_colors)); // 160
    file.read((uint8_t *)&_prefs.notify_mute_default, sizeof(_prefs.notify_mute_default));     // 161
    file.read((uint8_t *)&_prefs.channel_sender_colors, sizeof(_prefs.channel_sender_colors)); // 162
    file.read((uint8_t *)&_prefs.auto_lock, sizeof(_prefs.auto_lock));                         // 163
    file.read((uint8_t *)&_prefs.wifi_enabled, sizeof(_prefs.wifi_enabled));                   // 164
    file.read((uint8_t *)_prefs.wifi_ssid, sizeof(_prefs.wifi_ssid));                          // 165
    file.read((uint8_t *)_prefs.wifi_password, sizeof(_prefs.wifi_password));                  // 166
    file.read((uint8_t *)&_prefs.mqtt_enabled, sizeof(_prefs.mqtt_enabled));                   // 167
    file.read((uint8_t *)_prefs.mqtt_host, sizeof(_prefs.mqtt_host));                          // 168
    file.read((uint8_t *)&_prefs.mqtt_port, sizeof(_prefs.mqtt_port));                         // 169
    file.read((uint8_t *)_prefs.mqtt_user, sizeof(_prefs.mqtt_user));                          // 170
    file.read((uint8_t *)_prefs.mqtt_password, sizeof(_prefs.mqtt_password));                  // 171
    file.read((uint8_t *)_prefs.mqtt_topic_prefix, sizeof(_prefs.mqtt_topic_prefix));          // 172
    file.read((uint8_t *)&_prefs.mqtt_tls, sizeof(_prefs.mqtt_tls));                           // 173
    file.read((uint8_t *)&_prefs.mqtt_publish_rx, sizeof(_prefs.mqtt_publish_rx));             // 174
    file.read((uint8_t *)&_prefs.mqtt_publish_tx, sizeof(_prefs.mqtt_publish_tx));             // 175
    file.read((uint8_t *)&_prefs.wifi_dhcp, sizeof(_prefs.wifi_dhcp));                         // 176
    file.read((uint8_t *)&_prefs.wifi_dns_override, sizeof(_prefs.wifi_dns_override));         // 177
    file.read((uint8_t *)&_prefs.wifi_ip, sizeof(_prefs.wifi_ip));                             // 178
    file.read((uint8_t *)&_prefs.wifi_netmask, sizeof(_prefs.wifi_netmask));                   // 179
    file.read((uint8_t *)&_prefs.wifi_gateway, sizeof(_prefs.wifi_gateway));                   // 180
    file.read((uint8_t *)&_prefs.wifi_dns, sizeof(_prefs.wifi_dns));                           // 181
    file.read((uint8_t *)&_prefs.ntp_enabled, sizeof(_prefs.ntp_enabled));                     // 182
    file.read((uint8_t *)_prefs.ntp_server, sizeof(_prefs.ntp_server));                        // 183
    file.read((uint8_t *)&_prefs.use_rtc_clock, sizeof(_prefs.use_rtc_clock));                 // 184
    file.read((uint8_t *)&_prefs.sigmeter_snr_min, sizeof(_prefs.sigmeter_snr_min));           // 185
    file.read((uint8_t *)&_prefs.sigmeter_snr_max, sizeof(_prefs.sigmeter_snr_max));           // 186
    file.read((uint8_t *)&_prefs.sigmeter_hold_s, sizeof(_prefs.sigmeter_hold_s));             // 187
    file.read((uint8_t *)&_prefs.sigmeter_decay_s, sizeof(_prefs.sigmeter_decay_s));           // 188
    file.read((uint8_t *)&_prefs.show_chat_meta, sizeof(_prefs.show_chat_meta));               // 189
    file.read((uint8_t *)_prefs.ota_url, sizeof(_prefs.ota_url));                              // 190
    file.read((uint8_t *)&_prefs.power_monitor, sizeof(_prefs.power_monitor));                 // 191
    file.read((uint8_t *)&_prefs.batt_type, sizeof(_prefs.batt_type));                         // 192
    file.read((uint8_t *)&_prefs.batt_capacity_mah, sizeof(_prefs.batt_capacity_mah));         // 193
    file.read((uint8_t *)&_prefs.batt_soc_pmille, sizeof(_prefs.batt_soc_pmille));             // 194
    file.read((uint8_t *)&_prefs.gps_uart, sizeof(_prefs.gps_uart));                           // 195
    file.read((uint8_t *)&_prefs.ota_prerelease, sizeof(_prefs.ota_prerelease));               // 196
    file.read((uint8_t *)&_prefs.ota_custom_url, sizeof(_prefs.ota_custom_url));               // 197
    file.read((uint8_t *)&_prefs.trackball_speed, sizeof(_prefs.trackball_speed));             // 198
    file.read((uint8_t *)&_prefs.trackball_invert, sizeof(_prefs.trackball_invert));           // 199
    file.read((uint8_t *)&_prefs.font_scale, sizeof(_prefs.font_scale));                       // 200
    file.read((uint8_t *)&_prefs.touch_suppress_ms, sizeof(_prefs.touch_suppress_ms));         // 201
    file.read((uint8_t *)_prefs.mqtt_client_id, sizeof(_prefs.mqtt_client_id));                // 202
    file.read((uint8_t *)_prefs.mqtt_subscribe, sizeof(_prefs.mqtt_subscribe));                // 203
    file.read((uint8_t *)&_prefs.trackball_sel_invert, sizeof(_prefs.trackball_sel_invert));   // 204
    file.read((uint8_t *)&_prefs.tcp_companion, sizeof(_prefs.tcp_companion));                 // 205
    file.read((uint8_t *)&_prefs.buzzer_volume, sizeof(_prefs.buzzer_volume));                 // 206
    file.read((uint8_t *)_prefs.ringtone_name, sizeof(_prefs.ringtone_name));                  // 207
    file.read((uint8_t *)&_prefs.audio_output, sizeof(_prefs.audio_output));                   // 208

    // migrate old fields
    _prefs.setRepeatEn(_prefs._client_repeat != 0);

    sanitiseAppendedPrefs(_prefs);   // never let a truncated/odd file become live settings
    file.close();
  }
}

bool DataStore::savePrefs(NodePrefs& _prefs) {
  _prefs.cfg_version = COMPANION_PREFS_CFGVER;   // mark this file as fork-written
  // Crash-safe: write a temp then atomically swap. Upstream writes /prefs.json
  // in place; on SPIFFS a reset mid-rewrite leaves a TRUNCATED live file, which
  // is how a contacts store once went from 350 entries to 27. Do not "simplify"
  // this back to an in-place write.
  File file = openWrite(_fs, "/prefs.json.tmp");
  if (file) {
    bool success = _prefs.saveSerial(file);
    file.close();
    if (success) {
      commitTmp(_fs, "/prefs.json.tmp", "/prefs.json");
    }
    return success;
  }
  return false;
}

void DataStore::loadContacts(DataStoreHost* host) {
  recoverTmp(_getContactsChannelsFS(), "/contacts3.tmp", "/contacts3");
File file = openRead(_getContactsChannelsFS(), "/contacts3");
    if (file) {
      bool full = false;
      while (!full) {
        ContactInfo c;
        uint8_t pub_key[32];
        uint8_t unused;

        bool success = (file.read(pub_key, 32) == 32);
        success = success && (file.read((uint8_t *)&c.name, 32) == 32);
        success = success && (file.read(&c.type, 1) == 1);
        success = success && (file.read(&c.flags, 1) == 1);
        success = success && (file.read(&unused, 1) == 1);
        success = success && (file.read((uint8_t *)&c.sync_since, 4) == 4); // was 'reserved'
        success = success && (file.read((uint8_t *)&c.out_path_len, 1) == 1);
        success = success && (file.read((uint8_t *)&c.last_advert_timestamp, 4) == 4);
        success = success && (file.read(c.out_path, 64) == 64);
        success = success && (file.read((uint8_t *)&c.lastmod, 4) == 4);
        success = success && (file.read((uint8_t *)&c.gps_lat, 4) == 4);
        success = success && (file.read((uint8_t *)&c.gps_lon, 4) == 4);

        if (!success) break; // EOF

        c.id = mesh::Identity(pub_key);
        if (!host->onContactLoaded(c)) full = true;
      }
      file.close();
    }
}

void DataStore::saveContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c)) {
  FILESYSTEM* fs = _getContactsChannelsFS();
  File file = openWrite(fs, "/contacts3.tmp");   // crash-safe: write temp, then swap
  if (file) {
    uint32_t idx = 0;
    ContactInfo c;
    uint8_t unused = 0;
    bool ok = true;

    while (host->getContactForSave(idx, c)) {
      if (filter && !filter(c)) {
        idx++;  // advance to next contact
        continue;
      }
      bool success = (file.write(c.id.pub_key, 32) == 32);
      success = success && (file.write((uint8_t *)&c.name, 32) == 32);
      success = success && (file.write(&c.type, 1) == 1);
      success = success && (file.write(&c.flags, 1) == 1);
      success = success && (file.write(&unused, 1) == 1);
      success = success && (file.write((uint8_t *)&c.sync_since, 4) == 4);
      success = success && (file.write((uint8_t *)&c.out_path_len, 1) == 1);
      success = success && (file.write((uint8_t *)&c.last_advert_timestamp, 4) == 4);
      success = success && (file.write(c.out_path, 64) == 64);
      success = success && (file.write((uint8_t *)&c.lastmod, 4) == 4);
      success = success && (file.write((uint8_t *)&c.gps_lat, 4) == 4);
      success = success && (file.write((uint8_t *)&c.gps_lon, 4) == 4);

      if (!success) { ok = false; break; } // write failed -- don't commit a short file

      idx++;  // advance to next contact
    }
    file.close();
    if (ok) commitTmp(fs, "/contacts3.tmp", "/contacts3");
    else    fs->remove("/contacts3.tmp");   // leave the existing /contacts3 intact
  }
}

// Rewrite a single contact record in place (seek + write 152 bytes), avoiding
// a full-file rewrite. Crash-safe: never truncates the file, so an interrupted
// write can corrupt at most this one record, not the whole tail. Returns false
// if the file is missing/too short -- caller should fall back to saveContacts().
bool DataStore::updateContact(int idx, const ContactInfo& c) {
  if (idx < 0) return false;
  File file = openUpdate(_getContactsChannelsFS(), "/contacts3");
  if (!file) return false;
  if ((uint32_t)file.size() < (uint32_t)(idx + 1) * CONTACT_RECORD_SIZE) {
    file.close();
    return false;  // record not present yet -> full save resyncs the file
  }
  file.seek((uint32_t)idx * CONTACT_RECORD_SIZE);
  uint8_t unused = 0;
  bool ok = (file.write(c.id.pub_key, 32) == 32);
  ok = ok && (file.write((uint8_t *)&c.name, 32) == 32);
  ok = ok && (file.write(&c.type, 1) == 1);
  ok = ok && (file.write(&c.flags, 1) == 1);
  ok = ok && (file.write(&unused, 1) == 1);
  ok = ok && (file.write((uint8_t *)&c.sync_since, 4) == 4);
  ok = ok && (file.write((uint8_t *)&c.out_path_len, 1) == 1);
  ok = ok && (file.write((uint8_t *)&c.last_advert_timestamp, 4) == 4);
  ok = ok && (file.write(c.out_path, 64) == 64);
  ok = ok && (file.write((uint8_t *)&c.lastmod, 4) == 4);
  ok = ok && (file.write((uint8_t *)&c.gps_lat, 4) == 4);
  ok = ok && (file.write((uint8_t *)&c.gps_lon, 4) == 4);
  file.close();
  return ok;
}

void DataStore::loadChannels(DataStoreHost* host) {
    recoverTmp(_getContactsChannelsFS(), "/channels2.tmp", "/channels2");
    File file = openRead(_getContactsChannelsFS(), "/channels2");
    if (file) {
      bool full = false;
      uint8_t channel_idx = 0;
      while (!full) {
        ChannelDetails ch;
        uint8_t unused[4];

        bool success = (file.read(unused, 4) == 4);
        success = success && (file.read((uint8_t *)ch.name, 32) == 32);
        success = success && (file.read((uint8_t *)ch.channel.secret, 32) == 32);

        if (!success) break; // EOF

        if (host->onChannelLoaded(channel_idx, ch)) {
          channel_idx++;
        } else {
          full = true;
        }
      }
      file.close();
    }
}

void DataStore::saveChannels(DataStoreHost* host) {
  FILESYSTEM* fs = _getContactsChannelsFS();
  File file = openWrite(fs, "/channels2.tmp");   // crash-safe: write temp, then swap
  if (file) {
    uint8_t channel_idx = 0;
    ChannelDetails ch;
    uint8_t unused[4];
    memset(unused, 0, 4);
    bool ok = true;

    while (host->getChannelForSave(channel_idx, ch)) {
      bool success = (file.write(unused, 4) == 4);
      success = success && (file.write((uint8_t *)ch.name, 32) == 32);
      success = success && (file.write((uint8_t *)ch.channel.secret, 32) == 32);

      if (!success) { ok = false; break; } // write failed -- don't commit a short file
      channel_idx++;
    }
    file.close();
    if (ok) commitTmp(fs, "/channels2.tmp", "/channels2");
    else    fs->remove("/channels2.tmp");   // leave the existing /channels2 intact
  }
}

// Saved repeater/room logins: opaque blob on internal flash (_fs), crash-safe.
void DataStore::saveLogins(const uint8_t* data, size_t len) {
  File file = openWrite(_fs, "/logins.tmp");
  if (file) {
    bool ok = (file.write(data, len) == len);
    file.close();
    if (ok) commitTmp(_fs, "/logins.tmp", "/logins");
    else    _fs->remove("/logins.tmp");
  }
}
size_t DataStore::loadLogins(uint8_t* data, size_t maxlen) {
  recoverTmp(_fs, "/logins.tmp", "/logins");
  File file = openRead(_fs, "/logins");
  if (!file) return 0;
  int n = file.read(data, maxlen);
  file.close();
  return n > 0 ? (size_t)n : 0;
}

// Muted-conversation keys blob (crash-safe temp+rename, like the logins blob).
void DataStore::saveMutes(const uint8_t* data, size_t len) {
  File file = openWrite(_fs, "/mutes.tmp");
  if (file) {
    bool ok = (file.write(data, len) == len);
    file.close();
    if (ok) commitTmp(_fs, "/mutes.tmp", "/mutes");
    else    _fs->remove("/mutes.tmp");
  }
}
size_t DataStore::loadMutes(uint8_t* data, size_t maxlen) {
  recoverTmp(_fs, "/mutes.tmp", "/mutes");
  File file = openRead(_fs, "/mutes");
  if (!file) return 0;
  int n = file.read(data, maxlen);
  file.close();
  return n > 0 ? (size_t)n : 0;
}
void DataStore::saveUnmutes(const uint8_t* data, size_t len) {
  File file = openWrite(_fs, "/unmutes.tmp");
  if (file) {
    bool ok = (file.write(data, len) == len);
    file.close();
    if (ok) commitTmp(_fs, "/unmutes.tmp", "/unmutes");
    else    _fs->remove("/unmutes.tmp");
  }
}
size_t DataStore::loadUnmutes(uint8_t* data, size_t maxlen) {
  recoverTmp(_fs, "/unmutes.tmp", "/unmutes");
  File file = openRead(_fs, "/unmutes");
  if (!file) return 0;
  int n = file.read(data, maxlen);
  file.close();
  return n > 0 ? (size_t)n : 0;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)

#define MAX_ADVERT_PKT_LEN   (2 + 32 + PUB_KEY_SIZE + 4 + SIGNATURE_SIZE + MAX_ADVERT_DATA_SIZE)

struct BlobRec {
  uint32_t timestamp;
  uint8_t  key[7];
  uint8_t  len;
  uint8_t  data[MAX_ADVERT_PKT_LEN];
};

void DataStore::checkAdvBlobFile() {
  if (!_getContactsChannelsFS()->exists("/adv_blobs")) {
    File file = openWrite(_getContactsChannelsFS(), "/adv_blobs");
    if (file) {
      BlobRec zeroes;
      memset(&zeroes, 0, sizeof(zeroes));
      for (int i = 0; i < MAX_BLOBRECS; i++) {     // pre-allocate to fixed size
        file.write((uint8_t *) &zeroes, sizeof(zeroes));
      }
      file.close();
    }
  }
}

void DataStore::migrateToSecondaryFS() {
  // migrate old adv_blobs, contacts3 and channels2 files to secondary FS if they don't already exist
  if (!_fsExtra->exists("/adv_blobs")) {
    if (_fs->exists("/adv_blobs")) {
    File oldAdvBlobs = openRead(_fs, "/adv_blobs");
    File newAdvBlobs = openWrite(_fsExtra, "/adv_blobs");

    if (oldAdvBlobs && newAdvBlobs) {
      BlobRec rec;
      size_t count = 0;

      // Copy 20 BlobRecs from old to new
      while (count < 20 && oldAdvBlobs.read((uint8_t *)&rec, sizeof(rec)) == sizeof(rec)) {
        newAdvBlobs.seek(count * sizeof(BlobRec));
        newAdvBlobs.write((uint8_t *)&rec, sizeof(rec));
        count++;
      }
    }
    if (oldAdvBlobs) oldAdvBlobs.close();
    if (newAdvBlobs) newAdvBlobs.close();
    _fs->remove("/adv_blobs");
    }
  }
  if (!_fsExtra->exists("/contacts3")) {
    if (_fs->exists("/contacts3")) {
      File oldFile = openRead(_fs, "/contacts3");
      File newFile = openWrite(_fsExtra, "/contacts3");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fs->remove("/contacts3");
    }
  }
  if (!_fsExtra->exists("/channels2")) {
    if (_fs->exists("/channels2")) {
      File oldFile = openRead(_fs, "/channels2");
      File newFile = openWrite(_fsExtra, "/channels2");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fs->remove("/channels2");
    }
  }
  // cleanup nodes which have been testing the extra fs, copy _main.id and new_prefs back to primary
  if (_fsExtra->exists("/_main.id")) {
      if (_fs->exists("/_main.id")) {_fs->remove("/_main.id");}
      File oldFile = openRead(_fsExtra, "/_main.id");
      File newFile = openWrite(_fs, "/_main.id");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fsExtra->remove("/_main.id");
  }
  if (_fsExtra->exists("/new_prefs")) {
    if (_fs->exists("/new_prefs")) {_fs->remove("/new_prefs");}
      File oldFile = openRead(_fsExtra, "/new_prefs");
      File newFile = openWrite(_fs, "/new_prefs");

      if (oldFile && newFile) {
        uint8_t buf[64];
        int n;
        while ((n = oldFile.read(buf, sizeof(buf))) > 0) {
          newFile.write(buf, n);
        }
      }
      if (oldFile) oldFile.close();
      if (newFile) newFile.close();
      _fsExtra->remove("/new_prefs");
  }
  // remove files from where they should not be anymore
  if (_fs->exists("/adv_blobs")) {
    _fs->remove("/adv_blobs");
  }
  if (_fs->exists("/contacts3")) {
    _fs->remove("/contacts3");
  }
  if (_fs->exists("/channels2")) {
    _fs->remove("/channels2");
  }
  if (_fsExtra->exists("/_main.id")) {
    _fsExtra->remove("/_main.id");
  }
  if (_fsExtra->exists("/new_prefs")) {
    _fsExtra->remove("/new_prefs");
  }
}

uint8_t DataStore::getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) {
  File file = openRead(_getContactsChannelsFS(), "/adv_blobs");
  uint8_t len = 0;  // 0 = not found
  if (file) {
    BlobRec tmp;
    while (file.read((uint8_t *) &tmp, sizeof(tmp)) == sizeof(tmp)) {
      if (memcmp(key, tmp.key, sizeof(tmp.key)) == 0) {  // only match by 7 byte prefix
        len = tmp.len;
        memcpy(dest_buf, tmp.data, len);
        break;
      }
    }
    file.close();
  }
  return len;
}

bool DataStore::putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len) {
  if (len < PUB_KEY_SIZE+4+SIGNATURE_SIZE || len > MAX_ADVERT_PKT_LEN) return false;
  checkAdvBlobFile();
  File file = _getContactsChannelsFS()->open("/adv_blobs", FILE_O_WRITE);
  if (file) {
    uint32_t pos = 0, found_pos = 0;
    uint32_t min_timestamp = 0xFFFFFFFF;

    // search for matching key OR evict by oldest timestamp
    BlobRec tmp;
    file.seek(0);
    while (file.read((uint8_t *) &tmp, sizeof(tmp)) == sizeof(tmp)) {
      if (memcmp(key, tmp.key, sizeof(tmp.key)) == 0) {  // only match by 7 byte prefix
        found_pos = pos;
        break;
      }
      if (tmp.timestamp < min_timestamp) {
        min_timestamp = tmp.timestamp;
        found_pos = pos;
      }

      pos += sizeof(tmp);
    }

    memcpy(tmp.key, key, sizeof(tmp.key));  // just record 7 byte prefix of key
    memcpy(tmp.data, src_buf, len);
    tmp.len = len;
    tmp.timestamp = _clock->getCurrentTime();

    file.seek(found_pos);
    file.write((uint8_t *) &tmp, sizeof(tmp));

    file.close();
    return true;
  }
  return false; // error
}
bool DataStore::deleteBlobByKey(const uint8_t key[], int key_len) {
  return true; // this is just a stub on NRF52/STM32 platforms
}
#else
inline void makeBlobPath(const uint8_t key[], int key_len, char* path, size_t path_size) {
  char fname[18];
  if (key_len > 8) key_len = 8; // just use first 8 bytes (prefix)
  mesh::Utils::toHex(fname, key, key_len);
  sprintf(path, "/bl/%s", fname);
}

uint8_t DataStore::getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) {
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  if (_fs->exists(path)) {
    File f = openRead(_fs, path);
    if (f) {
      int len = f.read(dest_buf, 255); // currently MAX 255 byte blob len supported!!
      f.close();
      return len;
    }
  }
  return 0; // not found
}

bool DataStore::putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len) {
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  File f = openWrite(_fs, path);
  if (f) {
    int n = f.write(src_buf, len);
    f.close();
    if (n == len) return true; // success!

    _fs->remove(path); // blob was only partially written!
  }
  return false; // error
}

bool DataStore::deleteBlobByKey(const uint8_t key[], int key_len) {
  char path[64];
  makeBlobPath(key, key_len, path, sizeof(path));

  _fs->remove(path);
  
  return true; // return true even if file did not exist
}
#endif
