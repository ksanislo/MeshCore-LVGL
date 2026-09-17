#pragma once
#include <cstdint> // For uint8_t, uint32_t
#include <helpers/ConfigSerializer.h>

#define TELEM_MODE_DENY            0
#define TELEM_MODE_ALLOW_FLAGS     1     // use contact.flags
#define TELEM_MODE_ALLOW_ALL       2

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1

// Bump when the fork's prefs schema changes in a way that makes an older
// /prefs.json untrustworthy.
#define COMPANION_PREFS_CFGVER  1

// IMPORTANT: scoped in a namespace because src/helpers/CommonCLI.h declares a
// DIFFERENT global class also called NodePrefs, and the companion build links both
// (BridgeBase.h -> CommonCLI.h, via the CompanionMqtt shim). While both were plain
// structs the duplicate name was a benign ODR violation -- no vtables, so nothing
// was ever dispatched across the two layouts. Once upstream made both classes
// ConfigSerializer subclasses with a virtual structure(), the linker deduplicated
// the two identically-named weak vtable symbols and the companion's object got the
// REPEATER's vtable: structure() then walked the repeater's field list over the
// companion's memory and NULL-deref'd on the first sub-config. Distinct namespaces
// give distinct mangled vtable symbols. The trailing `using` keeps every existing
// reference to plain `NodePrefs` compiling; a TU that wrongly includes both headers
// now fails to compile (ambiguity) instead of corrupting itself at runtime.
namespace companion {

class NodePrefs : public ConfigSerializer {  // persisted to file
public:
  float airtime_factor = 0;
  char node_name[32];
  double node_lat = 0, node_lon = 0;
  float freq = 0;
  uint8_t sf = 0;
  uint8_t cr = 0;
  uint8_t multi_acks = 0;
  uint8_t manual_add_contacts = 0;
  float bw = 0;
  int8_t tx_power_dbm = 0;
  uint8_t telemetry_mode_base = 0;
  uint8_t telemetry_mode_loc = 0;
  uint8_t telemetry_mode_env = 0;
  float rx_delay_base = 0;
  uint32_t ble_pin = 0;
  uint8_t  advert_loc_policy = 0;
  uint8_t  buzzer_quiet = 0;
  uint8_t  vibe_quiet = 0;
  uint8_t  gps_enabled = 0;      // GPS enabled flag (0=disabled, 1=enabled)
  uint32_t gps_interval = 0;     // GPS read interval in seconds
  uint8_t autoadd_config = 0;    // bitmask for auto-add contacts config
  uint8_t rx_boosted_gain = 0; // SX126x RX boosted gain mode (0=power saving, 1=boosted)
  uint8_t radio_fem_rxgain = 0; // external LoRa FEM RX gain (LNA)
  uint8_t radio_fem_txgain = 0; // external LoRa FEM TX gain (low by default)
  uint8_t _client_repeat = 0;  // DEPRECATED -> use repeat.disable_fwd
  uint8_t path_hash_mode = 0;    // which path mode to use when sending
  uint8_t autoadd_max_hops = 0;  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
  char default_scope_name[31];
  uint8_t default_scope_key[16];

  // ---------------------------------------------------------------------------
  // LVGL-companion fork fields.
  //
  // These were appended at the TAIL of the legacy /new_prefs byte layout, after
  // every upstream field. That layout is unchanged by the ConfigSerializer move
  // (node_lat/node_lon still sit at offsets 40/48 -- upstream reads them into
  // NodePrefs where we used to read them into locals), so DataStore's legacy
  // reader can still recover all of these from a pre-1.17 device and migrate
  // them into /prefs.json. See DataStore::loadPrefsInt().
  //
  // 0 / 0xFF / 0xFFFF are "unset" sentinels: a file written by older firmware
  // lacks these bytes entirely, and the real defaults are applied in ONE place,
  // DataStore::applyAppendedPrefsDefaults(), which runs on both the file-read
  // path and the fresh-device no-file path.
  // ---------------------------------------------------------------------------
  uint8_t display_brightness = 0;  // 8-bit LEDC backlight duty (1-255); 0 = board default
  uint8_t display_rotation = 0;    // rotation index + 1 (1=rot0 .. 4=rot3); 0 = compile default
  uint8_t contacts_order = 0;      // contacts list sort: 0=A-Z,1=Heard,2=Latest; 0xFF = unset
  uint8_t contacts_filter = 0;     // contacts list filter: 0=All,1=Fav,2=Users,3=Rptr,4=Room,5=Sensor; 0xFF = unset
  int16_t tz_offset_minutes = 0;   // local-time display offset from UTC, in minutes (0 = UTC)
  uint8_t clock_12h = 0;           // 0 = 24-hour clock display, 1 = 12-hour (AM/PM)
  uint8_t persist_history = 0;     // 0 = session-only chat history, 1 = save to SD (0xFF = unset -> on)
  uint16_t screen_timeout_s = 0;   // backlight idle-off after N s of no touch; 0 = never
  uint8_t  radio_off = 0;          // 1 = LoRa radio disabled (no TX/RX); safe to detach the antenna
  char     lock_pin[8];            // settings-lock PIN (4-6 digits, NUL-terminated); "" = no PIN set
  uint8_t  notify_enable = 0;      // master new-message notifications; 0=off,1=on (0xFF unset -> on)
  uint8_t  avatar_palette = 0;     // contact avatar color scheme: 0 = curated (default), 1 = iOS-app parity
  char     theme_name[24];         // UI color theme: built-in name ("Dark"...) or an SD /themes file
  uint8_t  mention_user_colors = 0;    // chat: color @mentions by the user's avatar color
  uint8_t  hashtag_channel_colors = 0; // chat: color #hashtags by the channel's color
  uint8_t  notify_mute_default = 0;    // notifications: 1 = muted by default (opt-in per conv)
  uint8_t  channel_sender_colors = 0;  // channel chat: brand+color each sender's name in the bubble
  uint8_t  auto_lock = 0;              // 1 = auto-lock on screen sleep (needs a PIN set)
  // WiFi + MQTT bridge (ESP32 WiFi builds). All default off/empty so a fresh node
  // has no network activity until opted in.
  uint8_t  wifi_enabled = 0;          // 1 = connect to WiFi on boot / when applied
  char     wifi_ssid[33];             // 32-char SSID + NUL
  char     wifi_password[64];         // up to 63-char WPA2 PSK + NUL
  uint8_t  mqtt_enabled = 0;          // 1 = run the MQTT bridge (needs WiFi)
  char     mqtt_host[64];             // broker hostname / IP
  uint16_t mqtt_port = 0;             // 0 = protocol default (1883 plain / 8883 TLS)
  char     mqtt_user[32];             // optional; empty = anonymous
  char     mqtt_password[64];         // optional
  char     mqtt_topic_prefix[48];     // empty = "meshcore/<auto-client-id>"
  uint8_t  mqtt_tls = 0;              // 1 = TLS (insecure, no cert check)
  uint8_t  mqtt_publish_rx = 0;       // publish heard packets to <prefix>/rx (default on)
  uint8_t  mqtt_publish_tx = 0;       // publish sent packets to <prefix>/tx (default off)
  // WiFi addressing
  uint8_t  wifi_dhcp = 0;             // 1 = DHCP (default), 0 = static IP
  uint8_t  wifi_dns_override = 0;     // 1 = use wifi_dns even when on DHCP
  uint32_t wifi_ip = 0;               // static IPv4 (IPAddress raw, host order); 0 = unset
  uint32_t wifi_netmask = 0;
  uint32_t wifi_gateway = 0;
  uint32_t wifi_dns = 0;
  // NTP clock sync (WiFi mode only)
  uint8_t  ntp_enabled = 0;           // 1 = sync the RTC from NTP on connect (default on)
  char     ntp_server[48];            // empty = "pool.ntp.org"
  // Hardware (battery-backed) RTC discipline. 0xFF unset -> on.
  uint8_t  use_rtc_clock = 0;         // 1 = use the hardware RTC, 0 = off (stock bootstrap)
  // Signal-strength meter: peak-hold-with-decay envelope over heard-packet SNR.
  int8_t   sigmeter_snr_min = 0;      // dB: SNR at/below the bottom bar threshold (default -12)
  int8_t   sigmeter_snr_max = 0;      // dB: SNR at/above the top (4th) bar threshold (default 6)
  uint16_t sigmeter_hold_s = 0;       // s: hold the peak before it decays (default 30)
  uint16_t sigmeter_decay_s = 0;      // s: linear decay span after the hold (default 100)
  uint8_t  show_chat_meta = 0;        // 1 = show per-message diagnostics (hops/bytes, ack count)
  char     ota_url[96];               // firmware .bin URL for OTA
  // Optional battery / power monitor (off by default; never required).
  uint8_t  power_monitor = 0;         // 0 = None (feature off), 1 = INA219
  uint8_t  batt_type = 0;             // 0 = 1S Li-ion, 1 = 2S Li-ion, 2 = 1S LiFePO4
  uint16_t batt_capacity_mah = 0;     // pack capacity for coulomb counting (0 -> default 5000)
  uint16_t batt_soc_pmille = 0;       // last fuel-gauge SoC, permille of full (0..1000); 0xFFFF = unset
  uint8_t  gps_uart = 0;              // 0 = UART0 (IO43/44, rear plug), 1 = UART1 (IO17/18, default)
  // GitHub-release OTA options
  uint8_t  ota_prerelease = 0;        // 1 = include pre-releases in the update list / "latest"
  uint8_t  ota_custom_url = 0;        // 1 = manual-URL mode (flash ota_url), 0 = GitHub release mode
  uint8_t  trackball_speed = 0;       // scroll speed = pixels per roll-pulse (4..60); 0 = unset
  uint8_t  trackball_invert = 0;      // 1 = reverse nav-ball scroll direction
  uint8_t  font_scale = 0;            // 0 = auto (by screen size), 1 = Small, 2 = Medium, 3 = Large
  uint16_t touch_suppress_ms = 0;     // swallow touch for N ms after a scroll; 0 = off, capped 1000
  // MQTT advanced. Both empty by default; the bridge then auto-derives them.
  char     mqtt_client_id[24];        // empty = auto (derived from our pubkey). {pubkey} ok.
  char     mqtt_subscribe[80];        // empty = "<topic_prefix>/rf". {client_id}/{pubkey} ok.
  uint8_t  trackball_sel_invert = 0;  // 1 = reverse nav-ball focus/selection direction
  uint8_t  tcp_companion = 0;         // 1 = expose the companion frame protocol over TCP (port 5000)
  // Sound
  uint8_t  buzzer_volume = 0;         // 0 = muted, 1-10 = volume level; 0xFF unset -> 5
  char     ringtone_name[24];         // built-in alert name or SD basename; "" = first built-in
  uint8_t  audio_output = 0;          // 0 = piezo buzzer (default), 1 = I2S speaker; 0xFF unset -> piezo

  // JSON-ONLY config stamp -- never part of the legacy /new_prefs byte layout.
  // Written on every save by this fork. A /prefs.json that does NOT carry the
  // current value was produced by something other than this build (upstream
  // firmware, an older fork build, or a crashed/partial write), and its
  // fork-specific fields cannot be trusted. Loading such a file triggers a
  // rebuild from the legacy blob. This replaces an earlier heuristic that
  // sniffed field VALUES: that misfired on a corrupt file whose garbage
  // happened to be non-zero, leaving a device permanently broken with no way
  // to self-correct.
  uint8_t  cfg_version = 0;

private:
  class RadioPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("freq", _parent->freq);
      def("bw", _parent->bw);
      def("sf", _parent->sf);
      def("cr", _parent->cr);
      //def("cad", _parent->cad_enabled);
      //def("int_thr", _parent->interference_threshold);
      def("rxgain", _parent->rx_boosted_gain);
    #if 0
      // NOTE: these cannot be set (yet) so don't load/save until we can.
      //       also, fem_rxgain WAS mapped to wrong JSON property previously
      def("fem_rxgain", _parent->radio_fem_rxgain);
      def("fem_txgain", _parent->radio_fem_txgain);
    #endif
      def("tx", _parent->tx_power_dbm);
      def("af", _parent->airtime_factor);
      def("rxdelay", _parent->rx_delay_base);
      //def("f_txdelay", _parent->tx_delay_factor);   currently hard-coded
      //def("d_txdelay", _parent->direct_tx_delay_factor);  currently hard-coded
      //def("agc_int", _parent->agc_reset_interval);
      def("hash_mode", _parent->path_hash_mode);
      def("multi_ack", _parent->multi_acks);
      // fork additions
      def("off", _parent->radio_off);
      def("sig_min", _parent->sigmeter_snr_min);
      def("sig_max", _parent->sigmeter_snr_max);
      def("sig_hold", _parent->sigmeter_hold_s);
      def("sig_decay", _parent->sigmeter_decay_s);
    }
  public:
    RadioPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  RadioPrefs radio;

  class GPSPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("en", _parent->gps_enabled); // boolean
      def("int", _parent->gps_interval);   // interval in seconds
      def("adv_loc", _parent->advert_loc_policy);
      def("uart", _parent->gps_uart);      // fork: which UART socket the GPS is on
    }
  public:
    GPSPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  GPSPrefs gps;

  class RepeatPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
  public:
    uint8_t disable_fwd = 1;
  protected:
    void structure() override {
      def("disable", disable_fwd);
      //def("f_max", flood_max);
      //def("f_max_uns", flood_max_unscoped);
      //def("f_max_adv", flood_max_advert);
      //def("loop", loop_detect);
    }
  };
  RepeatPrefs repeat;

  class CompanionPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("auto_max", _parent->autoadd_max_hops);  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
      def("defs_nm", _parent->default_scope_name, sizeof(_parent->default_scope_name));
      def("defs_key", (void *) _parent->default_scope_key, sizeof(_parent->default_scope_key));
      def("pin", _parent->ble_pin);
      def("buzz_q", _parent->buzzer_quiet);
      def("vibe_q", _parent->vibe_quiet);
      def("auto_add", _parent->autoadd_config);    // bitmask for auto-add contacts config
      def("man_add", _parent->manual_add_contacts);
      def("tel_base", _parent->telemetry_mode_base);
      def("tel_loc", _parent->telemetry_mode_loc);
      def("tel_env", _parent->telemetry_mode_env);
      def("tcp", _parent->tcp_companion);          // fork: companion protocol over TCP
    }
  public:
    CompanionPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  CompanionPrefs companion;

  // ---- fork-only groups -----------------------------------------------------
  // !! SERIALIZER KEY CHARSET !!  Keys must contain ONLY [A-Za-z_] -- no digits.
  // ConfigSerializer's tokenizer (src/helpers/ConfigSerializer.cpp, is_key_char) accepts
  // letters and '_' only, while the writer will happily emit any key you give it. A key
  // with a digit therefore round-trips as a file that CANNOT be read back: the reader
  // returns TOK_ERROR at that character and abandons the rest of the file. That is not
  // theoretical -- "clk12" in this group silently truncated every /prefs.json at that
  // point, so "cfgver" (defined last) always read back as 0, rescueAppendedPrefs() then
  // treated the file as foreign and re-applied defaults, and NO fork setting could ever
  // persist across a reboot.
  class UiPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("bright", _parent->display_brightness);
      def("rot", _parent->display_rotation);
      def("c_order", _parent->contacts_order);
      def("c_filter", _parent->contacts_filter);
      def("tz_min", _parent->tz_offset_minutes);
      def("clk_ampm", _parent->clock_12h);   // NOT "clk12": see the key-charset warning above
      def("hist", _parent->persist_history);
      def("timeout", _parent->screen_timeout_s);
      def("pin", _parent->lock_pin, sizeof(_parent->lock_pin));
      def("autolock", _parent->auto_lock);
      def("notify", _parent->notify_enable);
      def("mute_def", _parent->notify_mute_default);
      def("avatar_pal", _parent->avatar_palette);
      def("theme", _parent->theme_name, sizeof(_parent->theme_name));
      def("mention_col", _parent->mention_user_colors);
      def("hashtag_col", _parent->hashtag_channel_colors);
      def("sender_col", _parent->channel_sender_colors);
      def("chat_meta", _parent->show_chat_meta);
      def("font", _parent->font_scale);
      def("rtc", _parent->use_rtc_clock);
      def("tb_speed", _parent->trackball_speed);
      def("tb_inv", _parent->trackball_invert);
      def("tb_sel_inv", _parent->trackball_sel_invert);
      def("touch_supp", _parent->touch_suppress_ms);
    }
  public:
    UiPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  UiPrefs ui;

  class WifiPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("en", _parent->wifi_enabled);
      def("ssid", _parent->wifi_ssid, sizeof(_parent->wifi_ssid));
      def("pass", _parent->wifi_password, sizeof(_parent->wifi_password));
      def("dhcp", _parent->wifi_dhcp);
      def("dns_ovr", _parent->wifi_dns_override);
      def("ip", _parent->wifi_ip);
      def("netmask", _parent->wifi_netmask);
      def("gateway", _parent->wifi_gateway);
      def("dns", _parent->wifi_dns);
      def("ntp_en", _parent->ntp_enabled);
      def("ntp", _parent->ntp_server, sizeof(_parent->ntp_server));
    }
  public:
    WifiPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  WifiPrefs wifi;

  class MqttPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("en", _parent->mqtt_enabled);
      def("host", _parent->mqtt_host, sizeof(_parent->mqtt_host));
      def("port", _parent->mqtt_port);
      def("user", _parent->mqtt_user, sizeof(_parent->mqtt_user));
      def("pass", _parent->mqtt_password, sizeof(_parent->mqtt_password));
      def("prefix", _parent->mqtt_topic_prefix, sizeof(_parent->mqtt_topic_prefix));
      def("tls", _parent->mqtt_tls);
      def("pub_rx", _parent->mqtt_publish_rx);
      def("pub_tx", _parent->mqtt_publish_tx);
      def("client_id", _parent->mqtt_client_id, sizeof(_parent->mqtt_client_id));
      def("sub", _parent->mqtt_subscribe, sizeof(_parent->mqtt_subscribe));
    }
  public:
    MqttPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  MqttPrefs mqtt;

  class BattPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("mon", _parent->power_monitor);
      def("type", _parent->batt_type);
      def("cap", _parent->batt_capacity_mah);
      def("soc", _parent->batt_soc_pmille);
    }
  public:
    BattPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  BattPrefs batt;

  class OtaPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("url", _parent->ota_url, sizeof(_parent->ota_url));
      def("pre", _parent->ota_prerelease);
      def("custom", _parent->ota_custom_url);
    }
  public:
    OtaPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  OtaPrefs ota;

  class AudioPrefs : public ConfigSerializer {
    NodePrefs* _parent;
  protected:
    void structure() override {
      def("vol", _parent->buzzer_volume);
      def("tone", _parent->ringtone_name, sizeof(_parent->ringtone_name));
      def("out", _parent->audio_output);
    }
  public:
    AudioPrefs(NodePrefs* parent) : _parent(parent) { }
  };
  AudioPrefs audio;

protected:
  void structure() override {
    def("name", node_name, sizeof(node_name));
    //def("adv_int", advert_interval);
    //def("f_adv_int", flood_advert_interval);
    def("lat", node_lat);
    def("lon", node_lon);
    def("radio", radio);
    def("gps", gps);
    def("repeat", repeat);
    def("comp", companion);
    // fork groups
    def("ui", ui);
    def("wifi", wifi);
    def("mqtt", mqtt);
    def("batt", batt);
    def("ota", ota);
    def("audio", audio);
    def("cfgver", cfg_version);
  }
public:
  NodePrefs() : radio(this), gps(this), companion(this),
                ui(this), wifi(this), mqtt(this), batt(this), ota(this), audio(this) {
    node_name[0] = 0;
    default_scope_name[0] = 0;
    memset(default_scope_key, 0, sizeof(default_scope_key));
    // fork string fields
    lock_pin[0] = 0;
    theme_name[0] = 0;
    wifi_ssid[0] = 0;
    wifi_password[0] = 0;
    mqtt_host[0] = 0;
    mqtt_user[0] = 0;
    mqtt_password[0] = 0;
    mqtt_topic_prefix[0] = 0;
    mqtt_client_id[0] = 0;
    mqtt_subscribe[0] = 0;
    ntp_server[0] = 0;
    ota_url[0] = 0;
    ringtone_name[0] = 0;
  }
  // new accessor methods
  bool isRepeatEn() const { return repeat.disable_fwd == 0; }
  void setRepeatEn(bool en) { repeat.disable_fwd = en ? 0 : 1; }
};

}  // namespace companion

using companion::NodePrefs;
