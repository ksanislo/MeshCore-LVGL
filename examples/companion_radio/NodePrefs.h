#pragma once
#include <cstdint> // For uint8_t, uint32_t

#define TELEM_MODE_DENY            0
#define TELEM_MODE_ALLOW_FLAGS     1     // use contact.flags
#define TELEM_MODE_ALLOW_ALL       2

#define ADVERT_LOC_NONE       0
#define ADVERT_LOC_SHARE      1

struct NodePrefs {  // persisted to file
  float airtime_factor;
  char node_name[32];
  float freq;
  uint8_t sf;
  uint8_t cr;
  uint8_t multi_acks;
  uint8_t manual_add_contacts;
  float bw;
  int8_t tx_power_dbm;
  uint8_t telemetry_mode_base;
  uint8_t telemetry_mode_loc;
  uint8_t telemetry_mode_env;
  float rx_delay_base;
  uint32_t ble_pin;
  uint8_t  advert_loc_policy;
  uint8_t  buzzer_quiet;
  uint8_t  gps_enabled;      // GPS enabled flag (0=disabled, 1=enabled)
  uint32_t gps_interval;     // GPS read interval in seconds
  uint8_t autoadd_config;    // bitmask for auto-add contacts config
  uint8_t rx_boosted_gain; // SX126x RX boosted gain mode (0=power saving, 1=boosted)
  uint8_t client_repeat;
  uint8_t path_hash_mode;    // which path mode to use when sending
  uint8_t autoadd_max_hops;  // 0 = no limit, 1 = direct (0 hops), N = up to N-1 hops (max 64)
  char default_scope_name[31];
  uint8_t default_scope_key[16];
  // Appended UI-only fields (persisted at the tail of /new_prefs). A file written
  // by older firmware lacks these bytes, so they load as 0 = "unset, use default".
  uint8_t display_brightness;  // 8-bit LEDC backlight duty (1-255); 0 = board default
  uint8_t display_rotation;    // rotation index + 1 (1=rot0 .. 4=rot3); 0 = compile default
  uint8_t contacts_order;      // contacts list sort: 0=A-Z,1=Heard,2=Latest; 0xFF = unset
  uint8_t contacts_filter;     // contacts list filter: 0=All,1=Fav,2=Users,3=Rptr,4=Room,5=Sensor; 0xFF = unset
  int16_t tz_offset_minutes;   // local-time display offset from UTC, in minutes (0 = UTC)
  uint8_t clock_12h;           // 0 = 24-hour clock display, 1 = 12-hour (AM/PM)
  uint8_t persist_history;     // 0 = session-only chat history, 1 = save to SD (0xFF = unset -> on)
  uint16_t screen_timeout_s;   // backlight idle-off after N s of no touch; 0 = never (also old-firmware default)
  uint8_t  radio_off;          // 1 = LoRa radio disabled (no TX/RX); safe to detach the antenna. Persists.
  char     lock_pin[8];        // settings-lock PIN (4-6 digits, NUL-terminated); "" = no PIN set
  uint8_t  notify_enable;      // master new-message notifications (wake+banner+buzzer); 0=off,1=on (0xFF unset -> on)
  uint8_t  avatar_palette;     // contact avatar color scheme: 0 = curated (default), 1 = iOS-app parity
  char     theme_name[24];     // UI color theme: built-in name ("Dark"...) or an SD /themes file; "" = default (Dark)
  uint8_t  mention_user_colors;   // chat: color @mentions by the user's avatar color (1=on default, 0=off -> theme accent)
  uint8_t  hashtag_channel_colors;// chat: color #hashtags by the channel's color   (1=on default, 0=off -> theme accent)
  uint8_t  notify_mute_default;   // notifications: 1 = muted by default (opt-in per conv), 0 = opt-out (default)
  uint8_t  channel_sender_colors; // channel chat: brand+color each sender's name in the bubble (1=on default, 0=off -> dim)
  uint8_t  auto_lock;             // 1 = auto-lock on screen sleep (needs a PIN set); 0 = manual lock only (default)
  // WiFi + MQTT bridge (ESP32 WiFi builds; pushed into the MQTT shim at apply-time).
  // All default off/empty so a fresh node has no network activity until opted in.
  uint8_t  wifi_enabled;          // 1 = connect to WiFi on boot / when applied
  char     wifi_ssid[33];         // 32-char SSID + NUL
  char     wifi_password[64];     // up to 63-char WPA2 PSK + NUL
  uint8_t  mqtt_enabled;          // 1 = run the MQTT bridge (needs WiFi)
  char     mqtt_host[64];         // broker hostname / IP
  uint16_t mqtt_port;             // 0 = protocol default (1883 plain / 8883 TLS)
  char     mqtt_user[32];         // optional; empty = anonymous
  char     mqtt_password[64];     // optional
  char     mqtt_topic_prefix[48]; // empty = "meshcore/<auto-client-id>"
  uint8_t  mqtt_tls;              // 1 = TLS (insecure, no cert check)
  uint8_t  mqtt_publish_rx;       // publish heard packets to <prefix>/rx (default on)
  uint8_t  mqtt_publish_tx;       // publish sent packets to <prefix>/tx (default off)
  // WiFi addressing
  uint8_t  wifi_dhcp;            // 1 = DHCP (default), 0 = static IP
  uint8_t  wifi_dns_override;    // 1 = use wifi_dns even when on DHCP
  uint32_t wifi_ip;             // static IPv4 (IPAddress raw, host order); 0 = unset
  uint32_t wifi_netmask;
  uint32_t wifi_gateway;
  uint32_t wifi_dns;
  // NTP clock sync (WiFi mode only; SNTP is built into the WiFi stack, no extra RAM)
  uint8_t  ntp_enabled;         // 1 = sync the RTC from NTP on connect (default on)
  char     ntp_server[48];      // empty = "pool.ntp.org"
  // Hardware (battery-backed) RTC discipline. Boards with a real RTC chip trust it:
  // skip the contact-time bootstrap at boot and periodically re-seed the system
  // clock from it (the chip's crystal outlasts/outperforms the MCU's). 0xFF unset -> on.
  uint8_t  use_rtc_clock;       // 1 = use the hardware RTC, 0 = off (stock bootstrap)
  // Signal-strength meter: a peak-hold-with-decay envelope over heard-packet SNR drives
  // header bars. Tunable via Settings > Radio > "Signal meter". Defaulted before read, so
  // old/short prefs files keep the defaults.
  int8_t   sigmeter_snr_min;    // dB: SNR at/below the bottom bar threshold (default -12)
  int8_t   sigmeter_snr_max;    // dB: SNR at/above the top (4th) bar threshold (default 6)
  uint16_t sigmeter_hold_s;     // s: hold the peak before it decays (default 30)
  uint16_t sigmeter_decay_s;    // s: linear decay span after the hold (default 100)
  uint8_t  show_chat_meta;      // 1 = show per-message diagnostics (hops/bytes, ack count) in chat (default off)
  char     ota_url[96];         // firmware .bin URL for OTA (dev: http://<laptop>:port/firmware.bin)
};