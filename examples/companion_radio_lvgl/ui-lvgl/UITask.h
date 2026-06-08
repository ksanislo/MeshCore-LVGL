#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <helpers/ui/LGFXDisplay.h>
#include "../../companion_radio/AbstractUITask.h"
#include "../../companion_radio/NodePrefs.h"
#include "MessageStore.h"
#include "MeshProxy.h"   // mproxy:: snapshot reads + command/event mailboxes
#ifdef HAS_SD_CARD
  #include "SdMessageStore.h"
#endif

#ifndef CHAT_HISTORY_CAP
  #define CHAT_HISTORY_CAP 48
#endif
// Hard cap on bubbles resident at once in the virtualized chat view (bounds widgets/memory/scroll
// cost regardless of conversation length). The live resident window slides within this.
#ifndef CHAT_RES_MAX
  #define CHAT_RES_MAX 64
#endif

struct UiPalette;   // ui_theme.h (full definition); only a reference is used here

class UITask : public AbstractUITask {
  LGFX_Device*    _lgfx;
  NodePrefs*      _node_prefs;       // -> _node_prefs_store (UI-owned working copy)
  NodePrefs       _node_prefs_store; // seeded from the snapshot; edits push CMD_UpdatePrefs
  uint32_t        _last_snap_version;// last mproxy snapshot version the UI rebuilt from
  uint32_t        _send_seq;         // monotonic client send-token (async send correlation)
  SensorManager*  _sensors;
  bool            _started;
  uint32_t        _last_tick_ms;
  int             _msgcount;
  // Power: backlight idle-off. _last_input_ms is reset on every touch; when the
  // idle timeout elapses the backlight is turned off (_display_off) and the next
  // touch wakes it (and is swallowed). The radio/mesh keep running on core 0.
  uint32_t        _last_input_ms;
  bool            _display_off;
  bool            _swallow_touch;       // swallow the whole wake gesture until the finger lifts
  bool            _touch_down;          // finger currently on the panel -> defer heavy rebuilds
  uint8_t         _backlight_duty;      // duty to restore on wake (configured brightness)
  lv_obj_t*       _splash_screen;
  lv_obj_t*       _home_screen;
  lv_obj_t*       _header;              // home logo+clock bar (hidden while a settings pane is open)
  lv_obj_t*       _header_logo;         // MeshCore wordmark (recolorable alpha img)
  lv_obj_t*       _clock_label;         // live device clock in the home header
  uint32_t        _clock_last;          // last shown second (1 Hz throttle)
  char            _clock_text[16];      // last formatted time (colon blinked on top of this)
  uint8_t         _clock_blink;         // colon-blink phase (heartbeat: toggles every 500ms in loop)
  lv_obj_t*       _tabview;
  lv_obj_t*       _tab_contacts;
  lv_obj_t*       _tab_channels;
  lv_obj_t*       _tab_settings;
  // Contacts list: a recycled pool of real row widgets (avatar circle + name +
  // last-seen + unread dot) floating over a spacer that sizes the virtual content.
  // ~pool_n widgets cover the viewport regardless of contact count -> scales + holds
  // scroll perf. Rows are non-floating: they scroll natively with the container and
  // are only rebound (text/avatar) when one recycles past the visible window.
  static const int CONTACT_POOL_MAX = 14;
  struct ContactRow { lv_obj_t* root; lv_obj_t* avatar; lv_obj_t* avatar_lbl;
                      lv_obj_t* name; lv_obj_t* seen; lv_obj_t* dot; };
  // (recycler instance state moved into ContactListView below)
  lv_obj_t*       _contacts_search_ta;  // name search field
  lv_obj_t*       _contacts_kb;         // keyboard for the search field (overlays home screen)
  lv_obj_t*       _contacts_filter_btn; // opens the order/filter pop-out
  lv_obj_t*       _cfilt_popup;         // top-layer pop-out (order + filter radio groups)
  lv_obj_t*       _cfilt_order_grp;     // radio rows: A-Z / Heard Recently / Latest Messages
  lv_obj_t*       _cfilt_filt_grp;      // radio rows: All / Favorites / Users / Repeaters / Rooms / Sensors
  int             _contacts_order;      // 0=A-Z, 1=Heard Recently, 2=Latest Messages
  int             _contacts_filt;       // 0=All,1=Fav,2=Users,3=Repeaters,4=Rooms,5=Sensors
  char            _contacts_filter[40]; // search substring
  lv_obj_t*       _channels_list;       // lv_list inside _tab_channels
  lv_obj_t*       _status_label;        // header status (bottom of contacts tab)
  bool            _contacts_dirty;      // message-store "latest" re-sort pending (set on new msg)
  uint32_t        _contacts_rebuilt_ms; // last rebuild time
  bool            _contacts_pending;    // snapshot changed -> rebuild contacts when its tab is shown
  bool            _channels_pending;    // snapshot changed -> rebuild channels when its tab is shown
  bool            _chat_pending;        // msg arrived for the open chat -> rebuild when the finger lifts

  // Notifications. _unread_keys is an in-RAM set of conv-keys (pubkey/secret hex)
  // with at least one message arrived-but-not-viewed; cleared when the chat opens.
  // (Not persisted across reboot in v1.) The banner is one reused top-layer card.
  static const int UNREAD_MAX = 64;
  char            _unread_keys[UNREAD_MAX][CHAT_PEER_NAME_MAX];
  uint8_t         _unread_count;
  // Per-conversation mute: in-RAM set (seeded from the backend at begin, persisted via
  // CmdKind::SetMute). Muted = no wake/banner/buzzer, but the unread mark still shows.
  static const int MUTE_MAX = 64;
  char            _muted_keys[MUTE_MAX][CHAT_PEER_NAME_MAX];     // explicitly muted
  int             _muted_count;
  char            _unmuted_keys[MUTE_MAX][CHAT_PEER_NAME_MAX];   // explicitly unmuted (override mute-by-default)
  int             _unmuted_count;
  lv_obj_t*       _banner;              // top-layer notification card (reused)
  lv_obj_t*       _banner_avatar;       // sender circle (DM) / channel avatar (channel msg)
  lv_obj_t*       _banner_avatar_lbl;   // sender grapheme (on the circle / inner circle)
  lv_obj_t*       _banner_title;        // sender name
  lv_obj_t*       _banner_body;         // 1-line message preview
  lv_timer_t*     _banner_timer;        // auto-dismiss one-shot
  char            _banner_key[CHAT_PEER_NAME_MAX];  // conv-key the banner opens on tap
  UIEventType     _pending_chime;       // chime deferred to end of loop() (post-draw) so notes don't stretch
#ifdef PIN_BUZZER
  genericBuzzer   _buzzer;              // RTTTL notification chimes (gated by buzzer_quiet)
#endif

  // Sorted/filtered view of the address book (favourites first, then recency).
  // Holds indices only -- no per-contact widgets -- so it scales to a full book.
  static const int CONTACTS_MAX_ROWS = 400;
  struct ContactDispRow { uint16_t idx; uint32_t heard; uint8_t fav; };

  // Reusable virtualized contact-list component: owns its scroll container, recycled
  // row pool, and filtered/sorted display set, plus a per-instance tap behavior.
  // Instantiated for the Contacts tab and the share/insert picker so every contact
  // list looks + scrolls identically. Functions: clistBuild/clistRefresh/clistBind/
  // clistRelayout (operate on a ContactListView&); clist_row_cb/clist_scroll_cb carry
  // the instance via event user_data.
  struct ContactListView {
    enum { LEAD_NONE = 0, LEAD_NEW, LEAD_SELF };  // special first row, per variant
    lv_obj_t* scroll;                       // scrollable container
    lv_obj_t* spacer;                       // transparent child sized to count rows
    lv_obj_t* empty;                        // centered placeholder
    lv_obj_t* sb;                           // draggable scrollbar thumb
    ContactRow pool[CONTACT_POOL_MAX];      // recycled row widgets
    int        pool_n;                      // realized pool size
    int        bound_idx[CONTACT_POOL_MAX]; // display index per slot (-1 = none)
    int        first_visible;               // top realized index (scroll churn guard)
    ContactDispRow rows[CONTACTS_MAX_ROWS]; // filtered/sorted display set (indices only)
    int        count;
    void (*on_tap)(const ContactInfo&);     // per instance: open chat, or pick-for-send
    int        lead;                        // LEAD_*: a special row pinned at display index 0
    void (*on_lead)();                      // tap behavior for the lead row
  };
  ContactListView _clist;                   // the Contacts tab list

  // Full-screen contact picker (top layer), virtualized like the Contacts tab.
  // Used to choose a recipient (share) or a contact to insert into a message.
  lv_obj_t*       _pick_popup;
  lv_obj_t*       _pick_search_ta;
  lv_obj_t*       _pick_kb;
  lv_obj_t*       _pick_title;
  char            _pick_filter[40];
  int             _pick_action;        // 1 = share viewed contact to pick; 2 = insert pick's ref
  ContactListView _pick_list;          // the picker's list (same component as the Contacts tab)

  // Channel picker (full-screen, same chrome as the contact picker) -> insert a link.
  lv_obj_t*       _chpick_popup;
  lv_obj_t*       _chpick_list;
  lv_obj_t*       _chpick_search_ta;
  lv_obj_t*       _chpick_kb;
  char            _chpick_filter[40];

  // Chat (conversation) screen. Three-band: fixed top bar / scrollable history
  // / fixed compose band (compose added with the keyboard step).
  RamMessageStore<CHAT_HISTORY_CAP> _rammsgs;   // session-only fallback
#ifdef HAS_SD_CARD
  SdMessageStore<CHAT_HISTORY_CAP> _sdmsgs;     // persistent (per-conversation files on SD)
#endif
  MessageStore* _msgs;   // -> _rammsgs or _sdmsgs, chosen at begin() from the setting
  uint32_t      _sd_off_ts;  // RTC time saving was disabled (0 = not off; for backfill)
  lv_obj_t*     _sd_btn = nullptr;       // top-bar red SD-card icon (mount when missing)
  lv_obj_t*     _batt_icon = nullptr;    // top-bar battery gauge (icon-only; optional power monitor)
  double        _batt_soc_mah = 0;       // coulomb-counted remaining capacity estimate
  bool          _batt_soc_init = false;  // seeded from voltage yet?
  uint32_t      _batt_last_ms = 0;       // last sample time (for the dt integration)
  uint32_t      _batt_tick_ms = 0;       // 1 Hz estimator tick (runs even while the backlight is off)
  double        _batt_learn_mah = 0;     // coulomb accumulated since the last rest anchor (auto-learn)
  double        _batt_learn_anchor = -1; // voltage-SoC at the last rest anchor (-1 = none)
  uint32_t      _batt_rest_since = 0;    // when the pack entered rest (0 = under load)
  uint32_t      _batt_soc_save_ms = 0;   // last time we checkpointed SoC to prefs (flash-wear throttle)
  volatile int  _batt_raw_mv = 0;        // latest INA sample (mV), published by battSampleTask on core 0
  volatile int  _batt_raw_ma = 0;        // latest INA sample (mA, signed); estimator reads these, no I2C
  bool          _sd_prev_ready = false;  // last-seen SD mounted state (to mark _sd_off_ts on removal)
  // Append to the RAM ring always (keeps recent history for the toggle) and to
  // the SD store when persistence is active.
  void storeAppend(bool outgoing, const char* key, const char* sender,
                   const char* text, uint32_t ts,
                   uint8_t status = 0, uint32_t ack = 0, uint32_t expiry_ms = 0, uint32_t cli = 0,
                   uint8_t hops = 0xFF, uint16_t bytes = 0, const RxMeta* meta = nullptr);
  lv_obj_t*       _chat_screen;
  lv_obj_t*       _chat_title;          // contact name in the chat top bar
  lv_obj_t*       _chat_avatar;         // avatar circle in the chat top bar (branding)
  lv_obj_t*       _chat_avatar_lbl;     // grapheme / type glyph inside the avatar
  lv_obj_t*       _chat_status;         // route line under the name: Direct / Flood / N hops
  lv_obj_t*       _chat_history;        // scrollable message container (the VSA band)
  lv_obj_t*       _chat_sb;             // draggable scrollbar thumb for chat history
  lv_obj_t*       _chat_jump_btn;       // floating "jump to live tail" chevron (shown when scrolled up)
  lv_obj_t*       _chat_compose;        // fixed compose band (textarea + send)
  lv_obj_t*       _chat_input;          // lv_textarea
  lv_obj_t*       _chat_send_btn;       // in-line send button (hidden while the keyboard is up)
  lv_obj_t*       _chat_count_lbl;      // live char count above the compose box
  lv_obj_t*       _chat_keyboard;       // lv_keyboard, shown on input focus
  lv_obj_t*       _insert_popup;        // backdrop for the insert (+) menu
  lv_obj_t*       _insert_list;         // lv_list inside the popup
  char            _chat_peer[CHAT_PEER_NAME_MAX];  // display name of the open chat ("" = none)
  char            _chat_key[CHAT_PEER_NAME_MAX];   // stable storage key (pubkey/secret hex)
  bool            _chat_is_channel;     // send via sendGroupMessage vs sendMessage
  uint8_t         _chat_pubkey[6];      // recipient prefix for contact sends
  int             _chat_channel_idx;    // channel slot for channel sends
  uint8_t         _chat_contact_type;   // ADV_TYPE_* of the open DM contact (repeater/room/chat)

  // In-conversation search (reveals a bar under the top bar; filters history).
  lv_obj_t*       _chat_search_bar;
  lv_obj_t*       _chat_search_ta;
  bool            _search_active;
  char            _search_filter[40];

  // Outgoing send-status: animated "sending" footer of the latest in-flight msg.
  lv_obj_t*       _sending_lbl;
  int             _dot_frame;
  uint32_t        _anim_ms;

  // Chat incremental-render cursor (Phase 1: append one bubble per new message instead of
  // rebuilding the whole history). rebuildChatHistory() resets these; appendChatBubble()
  // advances them so a later incremental append continues seamlessly. See appendChatBubble().
  int             _chat_rendered_n = 0;            // messages currently rendered as bubbles
  char            _crender_last_sender[CHAT_PEER_NAME_MAX] = "";
  bool            _crender_last_outgoing = false;
  long            _crender_last_day = -999999;
  bool            _crender_first = true;
  uint32_t        _crender_last_ts = 0;
  lv_obj_t*       _crender_last_footer = nullptr;  // previous msg's footer (for burst time-collapse)
  bool            _crender_footer_collapsible = false; // prev footer is a plain time (deletable on burst)
  int             _crender_tzoff = 0;
  long            _crender_now_day = 0;
  lv_coord_t      _crender_bubble_cap = 0;

  // Chat windowed rendering: build only ~a screenful of bubbles (not all CAP), lazy-load older
  // on scroll-up. K is derived from the chat band height / the DENSEST bubble unit (a one-line
  // bubble + row gap -- footers collapse in a burst), so it adapts to screen size/orientation.
  // Virtualized scrollback: the resident bubbles cover FILE-record range [_rfirst, _rlast) of the
  // open conversation (_chat_total = its total record count). The window is BOUNDED (~residentCap
  // bubbles): scrolling up pages an older chunk from SD at the top and trims a chunk off the bottom;
  // scrolling down pages newer and trims the top. _rlast == _chat_total means we're at the live tail.
  int             _chat_total = 0;          // total persisted records in the open conversation
  int             _rfirst = 0;              // file index of the oldest resident bubble
  int             _rlast  = 0;              // file index just past the newest resident bubble
  int             _chat_win_k = 0;          // max bubbles a screen can show (+ overscan); = page chunk
  bool            _chat_win_reset = true;   // next full rebuild snaps the window to the newest screenful
  bool            _chat_want_older = false; // scrolled near top w/ older available -> page older (loop)
  bool            _chat_want_newer = false; // scrolled near bottom w/ newer trimmed -> page newer (loop)
  bool            _chat_want_bottom = false;// chevron tap / drag-to-bottom -> snap to the live tail (loop)
  bool            _chat_prog_scroll = false;// a programmatic scroll/rebuild is in progress -> ignore scroll cb
  bool            _compose_editing = false; // re-entrancy guard for compose mention-encode / byte-cap edits
  int             _compose_prev_len = 0;    // compose byte-length at the last edit (detects paste vs keystroke)
  bool            _compose_pending = false; // a mention match is deferred pending disambiguation (re-check each keystroke)
  int             _compose_wire_len = 0;    // cached post-formatting (mention-encoded) byte length for the count
  ChatMessage*    _gather = nullptr;        // PSRAM scratch (residentCap msgs) for gathering a range/chunk
  int             _rrowChildN[CHAT_RES_MAX * 2];// child-object count per resident message (for edge trim);
                                                // 2x so a page can transiently exceed cap before trimming

  int             _rrowCount = 0;           // resident message count (== _rlast-_rfirst after a render)

  // Contact Info page (+ Path Editor sub-page). Lazily built, reused. The same
  // screen serves two modes: CINFO_VIEW (an existing contact) and CINFO_ADD
  // (assembling a new contact from a typed/pasted/prefilled key).
  enum { CINFO_VIEW = 0, CINFO_ADD = 1 };
  lv_obj_t*       _cinfo_screen;
  lv_obj_t*       _cinfo_body;          // scrollable form
  lv_obj_t*       _cinfo_hero;          // hero card (selectable -> long-press Copy contact)
  lv_obj_t*       _cinfo_title;
  lv_obj_t*       _cinfo_avatar;        // hero avatar circle on the Contact Info page
  lv_obj_t*       _cinfo_avatar_lbl;    // grapheme / type glyph inside it
  lv_obj_t*       _cinfo_realname;       // "(advert name)" hint, shown only when overridden
  lv_obj_t*       _cinfo_key;
  lv_obj_t*       _cinfo_fav_lbl;
  lv_obj_t*       _cinfo_name_ta;
  lv_obj_t*       _cinfo_keyfull;       // read-only full-hex label
  lv_obj_t*       _cinfo_lat_ta;
  lv_obj_t*       _cinfo_lon_ta;
  lv_obj_t*       _cinfo_type_lbl;      // read-only contact type (set from their advert)
  lv_obj_t*       _cinfo_lastheard;
  lv_obj_t*       _cinfo_telem;          // latest telemetry readings line
  lv_obj_t*       _cinfo_hops;
  lv_obj_t*       _cinfo_hops_x;        // clear-to-flood button
  lv_obj_t*       _cinfo_outpath;
  lv_obj_t*       _cinfo_kb;
  lv_obj_t*       _cinfo_active_ta;     // textarea currently being edited
  uint8_t         _cinfo_pubkey[32];    // full key of the contact being viewed
  // Section wrappers, toggled by applyCinfoMode(): present when viewing an
  // existing contact, hidden in add-mode (a not-yet-saved contact has none of
  // these). Kept as members only so the mode switch can show/hide them.
  lv_obj_t*       _cinfo_actions;       // Fav / Telem / Share row
  lv_obj_t*       _cinfo_pos_field;
  lv_obj_t*       _cinfo_type_field;
  lv_obj_t*       _cinfo_lh_field;      // Last Heard
  lv_obj_t*       _cinfo_tel_field;     // Telemetry
  lv_obj_t*       _cinfo_hops_row;
  lv_obj_t*       _cinfo_outpath_row;
  lv_obj_t*       _cinfo_delete_btn;    // "Delete contact" (view mode only)
  lv_obj_t*       _delc_popup;          // delete-contact confirm modal
  lv_obj_t*       _delch_popup;         // delete-channel confirm modal
  // Add-mode extras (shown only when _cinfo_mode == CINFO_ADD): a header Save
  // button, an editable Key field (manual entry; hidden when prefilled), and an
  // inline error line. In view-mode the key lives in the hero (read-only popup).
  lv_obj_t*       _cinfo_header_title;  // header-bar title ("Contact" / "New Contact")
  lv_obj_t*       _cinfo_save_btn;
  lv_obj_t*       _cinfo_key_field;     // makeField wrapper for the key textarea
  lv_obj_t*       _cinfo_key_ta;
  lv_obj_t*       _cinfo_err;
  // UI-owned working copy of the viewed (or prospective) contact. In view-mode
  // it's loaded from the snapshot at openContactInfo and edits push a CMD_*. In
  // add-mode it's the prospective contact being assembled from the form, saved
  // wholesale (AddContact) only when Save is pressed.
  ContactInfo     _cinfo_c;
  bool            _cinfo_valid;
  uint8_t         _cinfo_mode;          // CINFO_VIEW (existing) or CINFO_ADD (new)
  bool            _cinfo_addkey_locked; // add-mode: key is prefilled / read-only
  bool            _cinfo_haskey;        // add-mode: _cinfo_c holds a valid key
  char            _cinfo_override[CHAT_PEER_NAME_MAX];  // local nickname (optimistic)
  lv_obj_t*       _cinfo_return_screen; // where back goes

  // Channel Details page (hexagon hero + editable name + key). Reuses the shared
  // hero/field widgets; the channel is identified by its 32-byte secret.
  lv_obj_t*       _chinfo_screen;
  lv_obj_t*       _chinfo_body;
  lv_obj_t*       _chinfo_hero;         // channel hero card (selectable -> long-press Copy link)
  lv_obj_t*       _chinfo_avatar;
  lv_obj_t*       _chinfo_avatar_lbl;
  lv_obj_t*       _chinfo_title;
  lv_obj_t*       _chinfo_key;
  lv_obj_t*       _chinfo_name_ta;
  lv_obj_t*       _chinfo_kb;
  lv_obj_t*       _chinfo_active_ta;
  uint8_t         _chinfo_secret[32];   // channel identity (preserved across rename)
  char            _chinfo_name[32];     // current name (optimistic)
  bool            _chinfo_is_hashtag;   // #tag channel: key is derived from the name -> rename is blocked
  lv_obj_t*       _chinfo_return_screen;

  // Latest telemetry readings, stashed for the Contact Info page (one contact's
  // worth -- the most recent response). GPS is applied to the contact position.
  uint8_t         _telem_pubkey[6];
  char            _telem_text[160];

  // Shared top-layer popup (kebab menu / share menu / pickers) + toast.
  lv_obj_t*       _menu_popup;
  lv_obj_t*       _menu_list;
  lv_obj_t*       _toast;
  lv_timer_t*     _toast_timer;   // auto-hide timer; a new toast cancels the pending one
  lv_obj_t*       _sig_meter;     // header signal-strength meter container
  lv_obj_t*       _sig_bars[4];   // its 4 bars (low->high)
  int             _sig_bar_count; // bars currently lit (for hysteresis); -1 = uninit
  lv_obj_t*       _path_return_screen;

  lv_obj_t*       _path_screen;
  lv_obj_t*       _path_size_dd;
  lv_obj_t*       _path_ta;
  lv_obj_t*       _path_kb;
  lv_obj_t*       _path_err;

  // Settings tab widgets (live inside _tab_settings; keyboard overlays _home_screen).
  lv_obj_t*       _set_profile_avatar;     // compact owner hero (Settings launcher)
  lv_obj_t*       _set_profile_avatar_lbl;
  lv_obj_t*       _set_profile_name;
  lv_obj_t*       _set_profile_key;
  lv_obj_t*       _prof_hero;              // owner hero card (selectable -> long-press Copy)
  lv_obj_t*       _prof_avatar;            // big owner hero (full-screen Profile page)
  lv_obj_t*       _prof_avatar_lbl;
  lv_obj_t*       _prof_name;
  lv_obj_t*       _prof_key;
  // Owner profile: a full-screen "contact page for yourself" (own screen/keyboard,
  // like Contact Info), reached from the launcher hero or any link to self.
  lv_obj_t*       _profile_screen;
  lv_obj_t*       _profile_body;
  lv_obj_t*       _profile_kb;
  lv_obj_t*       _profile_return_screen;
  lv_obj_t*       _set_name_ta;
  lv_obj_t*       _set_freq_ta;
  lv_obj_t*       _set_bw_dd;
  lv_obj_t*       _set_sf_dd;
  lv_obj_t*       _set_cr_dd;
  lv_obj_t*       _set_txp_ta;
  lv_obj_t*       _set_path_dd;
  lv_obj_t*       _set_bright_slider;
  lv_obj_t*       _set_scroll_slider = nullptr;  // trackball "Scroll speed" (created only on trackball boards)
  lv_obj_t*       _set_scroll_inv_chk = nullptr; // trackball "Reverse scroll direction" (trackball boards only)
  lv_obj_t*       _set_touchlock_slider = nullptr; // trackball "Scroll touch-lock" ms (trackball boards only)
  lv_obj_t*       _set_rot_dd;
  lv_obj_t*       _set_font_dd = nullptr;   // UI "Font size" tier dropdown (Auto/Small/Medium/Large)
  lv_obj_t*       _set_screen_dd;       // screen idle-off timeout dropdown
  lv_obj_t*       _set_tz_ta;           // UTC offset (hours) for local-time display
  lv_obj_t*       _set_clock_chk;       // 12-hour clock toggle
  lv_obj_t*       _set_rtc_chk;         // "use hardware RTC" toggle (discipline clock from the chip)
  lv_obj_t*       _set_time_hh;         // manual set-time: hours box
  lv_obj_t*       _set_time_mm;         // manual set-time: minutes box (seconds aren't shown -- the ↻ button zeroes them)
  lv_obj_t*       _set_time_ampm;       // manual set-time: AM/PM dropdown (12h mode only)
  lv_obj_t*       _set_meta_chk;        // "show metadata in chat" toggle
  lv_obj_t*       _set_autolock_chk;    // auto-lock on sleep toggle
  // Network panes (WiFi + MQTT). Values committed on each pane's Save button.
  lv_obj_t*       _set_wifi_en;         // WiFi enable switch
  lv_obj_t*       _set_wifi_ssid;       // SSID textarea
  lv_obj_t*       _set_wifi_pw;         // password textarea (eye-toggle)
  lv_obj_t*       _set_wifi_dhcp;       // DHCP checkbox (default on)
  lv_obj_t*       _set_wifi_dns_ovr;    // override DNS even on DHCP
  lv_obj_t*       _set_wifi_ip;         // static / live IP
  lv_obj_t*       _set_wifi_mask;       // static / live netmask
  lv_obj_t*       _set_wifi_gw;         // static / live gateway
  lv_obj_t*       _set_wifi_dns;        // DNS (static, or override)
  lv_obj_t*       _set_wifi_status;     // live status label
  lv_obj_t*       _set_ntp_en;          // NTP enable checkbox
  lv_obj_t*       _set_ntp_server;      // NTP server field
  lv_obj_t*       _set_ntp_status;      // NTP status label
  lv_obj_t*       _set_preset_status;   // radio-preset update status label
  char            _preset_status_last[40] = "";  // last-seen status (reload table on change)
  lv_obj_t*       _set_ota_url_ta;      // OTA firmware URL field (lives on the Node Info screen)
  lv_obj_t*       _set_ota_url_field;   // makeField column wrapping the URL textarea (hidden unless custom mode)
  lv_obj_t*       _set_ota_status;      // OTA status label
  lv_obj_t*       _set_ota_btn;         // "Update firmware" button (greyed when no IP)
  lv_obj_t*       _set_ota_lbl;         // its label -> toggles to "Cancel upgrade" while downloading
  lv_obj_t*       _set_ota_curlatest;   // "Current: X" / "Latest: Y" version label (two lines)
  lv_obj_t*       _set_ota_release_field;// makeField column wrapping the release dropdown
  lv_obj_t*       _set_ota_release_dd;  // release picker dropdown (shown when Additional options is on)
  lv_obj_t*       _set_ota_prerel_chk;  // "Include pre-releases" checkbox
  lv_obj_t*       _set_ota_customchk;   // "Custom URL" checkbox (shown only under pre-release)
  int             _ota_dd_map[16];      // dropdown visible index -> backend release index
  int             _ota_dd_count;        // entries currently in the dropdown / _ota_dd_map
  bool            _ota_was_fetching = false;  // edge-detect fetch completion -> force a dropdown rebuild
  uint32_t        _ota_last_check_ms = 0;     // millis of the last release check (0 = never; drives auto-recheck)
  uint8_t         _ota_fetch_tries = 0;       // bounded cold-fetch retries this screen visit (transient-failure self-heal)
  lv_obj_t*       _set_emoji_btn = nullptr;   // "Download emoji" -> morphs to "Cancel" while downloading
  lv_obj_t*       _set_emoji_lbl = nullptr;   // its label
  lv_obj_t*       _set_emoji_status = nullptr;// emoji-pack status/progress line
  lv_obj_t*       _update_body;         // Update screen scroll container (for kb raise)
  lv_obj_t*       _update_kb;           // keyboard for the OTA URL field on the Update screen
  lv_obj_t*       _update_screen = nullptr;  // standalone "Update" screen (firmware + emoji packs)
  lv_timer_t*     _update_timer  = nullptr;  // 1 Hz refresh while the Update screen is shown
  lv_obj_t*       _otafail_popup;       // "firmware update failed" modal (backdrop)
  lv_obj_t*       _otafail_lbl;         // its reason text
  lv_obj_t*       _set_mqtt_en;         // MQTT enable switch
  lv_obj_t*       _set_mqtt_host;       // broker host
  lv_obj_t*       _set_mqtt_port;       // broker port (digits)
  lv_obj_t*       _set_mqtt_user;       // user
  lv_obj_t*       _set_mqtt_pw;         // password (eye-toggle)
  lv_obj_t*       _set_mqtt_topic;      // topic prefix
  lv_obj_t*       _set_mqtt_tls;        // TLS checkbox
  lv_obj_t*       _set_mqtt_rx;         // publish RX checkbox
  lv_obj_t*       _set_mqtt_tx;         // publish TX checkbox
  lv_obj_t*       _set_mqtt_status;     // live status label
  lv_obj_t*       _set_avatar_dd;       // contact avatar color scheme (Default / iOS app)
  lv_obj_t*       _set_theme_dd;        // UI color theme picker (built-ins + SD /themes)
  lv_obj_t*       _set_mention_chk;     // chat: color @mentions by user color
  lv_obj_t*       _set_hashtag_chk;     // chat: color #hashtags by channel color
  lv_obj_t*       _set_chsender_chk;    // channel chat: brand+color each sender's name
  lv_obj_t*       _set_history_chk;     // persist chat history to SD toggle
  lv_obj_t*       _set_notify_chk;      // master new-message notifications toggle
  lv_obj_t*       _set_mutedef_chk;     // "Mute by default" (opt-in per conversation)
  lv_obj_t*       _set_kb;
  lv_obj_t*       _set_active_ta;       // settings textarea currently being edited
  // Settings categories. The pane index is the single source of truth shared by
  // makeSettingsPane(), makeCategoryRow(), and the _set_pane[] arrays; the enum
  // keeps them in sync and CAT_COUNT sizes the arrays + the bounds guard. CAT_ABOUT
  // is a sentinel launcher row that opens Node Info instead of an in-tab pane.
  enum SettingsCat {
    CAT_PROFILE = 0, CAT_RADIO, CAT_TELEMETRY, CAT_NOTIFY,
    CAT_DISPLAY, CAT_POWER, CAT_WIFI, CAT_MQTT, CAT_COUNT,
    CAT_ABOUT = 100,
    CAT_UPDATE = 101,   // standalone "Update" screen (firmware + emoji packs); not a pane
  };
  lv_obj_t*       _set_launcher;             // Settings category launcher (profile hero + rows)
  lv_obj_t*       _set_pane[CAT_COUNT];      // one pane per category, shown one at a time
  lv_obj_t*       _set_pane_body[CAT_COUNT]; // each pane's scrollable body (fields parented here)
  lv_obj_t*       _set_active_pane;     // active pane body = keyboard-raise scroll target
  lv_obj_t*       _set_key_ta;          // read-only self public key (scrolls horizontally)
  lv_obj_t*       _set_lat_ta;
  lv_obj_t*       _set_lon_ta;
  lv_obj_t*       _set_sharepos;        // "share position" checkbox
  // Telemetry policy (who may request our base/location/environment telemetry).
  lv_obj_t*       _set_telem_base_dd;
  lv_obj_t*       _set_telem_loc_dd;
  lv_obj_t*       _set_pwrmon_dd = nullptr;    // Power monitor select (None/INA219)
  lv_obj_t*       _set_batt_type_dd = nullptr; // Battery chemistry/cells
  lv_obj_t*       _set_batt_cap_ta = nullptr;  // Battery capacity (mAh)
  lv_obj_t*       _set_telem_env_dd;
  // Advanced section.
  lv_obj_t*       _set_autoadd_chk;     // auto-add heard contacts (inverse of manual_add_contacts)
  lv_obj_t*       _set_autoadd_hops_dd; // auto-add max hops
  lv_obj_t*       _set_rxboost_chk;     // SX126x RX boosted gain (reboot to apply)
  lv_obj_t*       _set_multiack_ta;     // extra ACK transmit count
  lv_obj_t*       _set_rxdelay_ta;      // RX delay base (seconds)
  lv_obj_t*       _set_airtime_ta;      // airtime budget factor
  lv_obj_t*       _set_gps_chk;         // enable optional UART GPS (reboot to apply)
  lv_obj_t*       _set_gps_uart_dd = nullptr;  // which UART socket the GPS is on (reboot to apply)
  lv_obj_t*       _set_gps_interval_ta; // GPS auto-update interval (seconds)
  lv_obj_t*       _set_gps_status;      // live GPS debug line (detected/fix/sats/coords)
  lv_obj_t*       _set_radio_sw;        // LoRa radio on/off (off = safe to detach antenna)
  // Set-PIN dialog (two boxes, must match). Lazily built.
  lv_obj_t*       _pinset_popup;
  lv_obj_t*       _pinset_ta1;
  lv_obj_t*       _pinset_ta2;
  lv_obj_t*       _pinset_err;
  lv_obj_t*       _pinset_kb;
  // Signal-meter tuning modal (off the Radio pane). Number fields commit on defocus.
  lv_obj_t*       _sigmod_popup;
  lv_obj_t*       _sigmod_kb;
  lv_obj_t*       _sig_min_ta;
  lv_obj_t*       _sig_max_ta;
  lv_obj_t*       _sig_hold_ta;
  lv_obj_t*       _sig_decay_ta;
  // Lock overlay (keypad PIN entry). Lazily built; _locked gates the whole UI.
  static const int LOCK_PIN_MAX = 6;       // PINs are 4-6 digits
  static const intptr_t LOCK_KEY_CLEAR = -1;
  static const intptr_t LOCK_KEY_DEL   = -2;
  lv_obj_t*       _lock_screen;
  lv_obj_t*       _lock_err;
  lv_obj_t*       _lock_dot[LOCK_PIN_MAX]; // entered-digit indicators
  char            _lock_entry[LOCK_PIN_MAX + 1];
  int             _lock_len;
  bool            _locked;
  lv_obj_t*       _confirm_popup;       // share-position warning modal (top layer)
  lv_obj_t*       _joinch_popup;        // "Add channel #name?" confirm modal
  lv_obj_t*       _joinch_lbl;          // its question text (repopulated per tag)
  char            _joinch_name[CHAT_PEER_NAME_MAX];   // pending hashtag name to join
  uint8_t         _joinch_psk[16];                     // its derived PSK

  // Reusable info modal (telemetry response now; repeater status/CLI later).
  lv_obj_t*       _info_popup;
  lv_obj_t*       _info_title_lbl;
  lv_obj_t*       _info_body_lbl;

  // Full-public-key popup (shared by the contact + owner hero key lines).
  lv_obj_t*       _keypop_popup;
  lv_obj_t*       _keypop_lbl;
  char            _keypop_hex[2 * PUB_KEY_SIZE + 1];

  // Private-key popup (owner profile kebab): one box, Export fills it, Import writes it.
  lv_obj_t*       _impkey_popup;
  lv_obj_t*       _impkey_ta;
  lv_obj_t*       _impkey_err;
  lv_obj_t*       _impkey_kb;
  lv_obj_t*       _impkey_confirm;
  uint8_t         _impkey_bytes[64];         // parsed key, held between Import and confirm

  // Internal typed clipboard (no OS/LVGL clipboard on-device). `kind` lets a paste
  // target pull the relevant part (smart paste, Phase 3); for now everything pastes
  // _clip_text verbatim. A lone contact card copies as CLIP_CONTACT_REF (the whole
  // "<hex:type:name>" token + parsed parts); any other selection is CLIP_PLAIN.
  enum ClipKind : uint8_t { CLIP_EMPTY, CLIP_PLAIN, CLIP_CONTACT_REF, CLIP_MENTION };
  char            _crash_note[96];    // one-shot "crash report saved" toast, surfaced from loop()
  static constexpr size_t kClipTextCap = 1024;  // longest copyable today is the ~150B msg body
  char*           _clip_text = nullptr;         // PSRAM (kClipTextCap) -- cold, doesn't need fast RAM
  uint8_t         _clip_kind;
  uint8_t         _clip_pubkey[PUB_KEY_SIZE];   // valid when kind == CLIP_CONTACT_REF
  char            _clip_name[CHAT_PEER_NAME_MAX];
  uint8_t         _clip_type;

  // ----- Universal text-selection controller (long-press -> drag -> context menu).
  // One instance drives selection on every chat-bubble text label and contact card
  // (textareas + smart paste come in later phases). Targets are wired with
  // sel_event_cb; the two triangle handles + the menu float on lv_layer_top().
  enum SelKind  : uint8_t { SEL_NONE, SEL_LABEL, SEL_CARD, SEL_TEXTAREA };
  enum SelState : uint8_t { SS_IDLE, SS_MARKING, SS_DRAGGING, SS_MENU };
  struct SelectionCtl {
    SelState  state;
    SelKind   kind;
    lv_obj_t* target;     // the lv_label or the card lv_obj being selected
    uint32_t  anchor;     // fixed endpoint (char-id) during a drag
    uint32_t  sel_lo, sel_hi;   // ordered selection range (char-id over markup)
    bool      whole_obj;  // card selected atomically
    lv_obj_t* h_start;    // down-triangle handle (tip = selection start / caret line)
    lv_obj_t* h_end;      // up-triangle handle   (tip = selection end)
    lv_obj_t* catcher;    // transparent full-screen tap-catcher (dismiss on tap OUTSIDE the selection)
    lv_obj_t* menu;       // floating horizontal toolbar (Copy / Select All / ...), no dim
    uint32_t  last_tap_ms;   // double-tap (word select) detection
    lv_coord_t last_tap_x, last_tap_y;
    lv_obj_t* last_tap_obj;
  };
  SelectionCtl    _sel;

  // Share-as-QR screen (name + truncated key band, QR below). Lazily built.
  lv_obj_t*       _qr_screen;
  lv_obj_t*       _qr_code;
  lv_obj_t*       _qr_name_lbl;
  lv_obj_t*       _qr_key_lbl;
  lv_obj_t*       _qr_return_screen;

  // New-channel screen (name + hex key / pasted link). Lazily built.
  lv_obj_t*       _newchan_screen;
  lv_obj_t*       _newchan_name_ta;
  lv_obj_t*       _newchan_key_ta;
  lv_obj_t*       _newchan_key_field;   // wrapper, disabled when "Public channel" derives the key
  lv_obj_t*       _newchan_public_chk;  // "Public channel": derive the key from the name
  lv_obj_t*       _newchan_err;
  lv_obj_t*       _newchan_kb;
  lv_obj_t*       _newchan_hero_av;     // live hexagon preview
  lv_obj_t*       _newchan_hero_avl;
  lv_obj_t*       _newchan_hero_nm;
  lv_obj_t*       _newchan_hero_key;
  lv_obj_t*       _newchan_return_screen;   // where Back / Save returns (chat when prefilled)

  // (Adding a contact is not a separate screen: it's the Contact Info screen in
  // CINFO_ADD mode -- see openNewContact / openNewContactPrefilled.)

  // Node-info / status screen (read-only, periodically refreshed). Lazily built.
  lv_obj_t*       _nodeinfo_screen;
  lv_obj_t*       _nodeinfo_lbl;
  lv_timer_t*     _nodeinfo_timer;

  // Repeater/room login screen (password prompt). Lazily built.
  lv_obj_t*       _login_popup;         // dimmed backdrop over the originating screen
  lv_obj_t*       _login_card;          // the login card itself
  lv_obj_t*       _login_title;         // "Login: <name>"
  lv_obj_t*       _login_pw_ta;
  lv_obj_t*       _login_save_chk;      // "Save login"
  lv_obj_t*       _login_auto_chk;      // "Auto-login"
  lv_obj_t*       _login_kb;
  uint8_t         _login_pubkey[6];     // server we're logging into

  // ----- Repeater/room Admin screen (structured CLI-over-mesh editor). Lazily built. -----
  // One screen, parameterized by contact type via a declarative spec table. Field values
  // are fetched on demand (get <key>); edits send set <key> <v>; actions are one-shot CLI.
  static constexpr int ADMIN_MAX_FIELDS = 24;
  lv_obj_t*       _admin_screen = NULL;
  lv_obj_t*       _admin_body = NULL;
  lv_obj_t*       _admin_kb = NULL;
  lv_obj_t*       _admin_return_screen = NULL; // screen to restore on back
  bool            _admin_active = false;        // Admin screen is the active screen (gates reply interception)
  uint8_t         _admin_pubkey[6] = {0};       // node being administered
  uint8_t         _admin_type = 0;              // ADV_TYPE_REPEATER / ADV_TYPE_ROOM (drives the spec mask)
  char            _admin_conv_key[CHAT_PEER_NAME_MAX] = {0};  // conv_key of the admin target (reply matching)
  lv_obj_t*       _admin_widget[ADMIN_MAX_FIELDS] = {0};      // visible-row -> input widget
  lv_obj_t*       _admin_apply_btn[ADMIN_MAX_FIELDS] = {0};   // visible-row -> Apply button (NULL if none)
  uint8_t         _admin_specidx[ADMIN_MAX_FIELDS] = {0};     // visible-row -> spec index
  // Radio tuple is one logical setting (composite set radio) shown as freq field + bw/sf/cr
  // dropdowns; _admin_widget[radio row] aliases _admin_radio_freq for the loading hint.
  lv_obj_t*       _admin_radio_freq = NULL;
  lv_obj_t*       _admin_radio_bw = NULL;
  lv_obj_t*       _admin_radio_sf = NULL;
  lv_obj_t*       _admin_radio_cr = NULL;
  int             _admin_field_count = 0;       // number of visible rows built
  // One outstanding request at a time -- the mesh can't batch, so every get/set is an
  // explicit, single user action (per-item Refresh / Apply buttons). No auto-load, no auto-save.
  int             _admin_pending_row = -1;      // visible-row of the outstanding get (-1 = none)
  bool            _admin_await_ack = false;     // a set/action reply is expected (toast it)
  bool            _admin_action_to_popup = false; // route the next ack to showInfoPopup (e.g. neighbors)
  lv_timer_t*     _admin_reply_timer = NULL;    // single-shot timeout for the outstanding get/ack
  uint8_t         _admin_timeout_streak = 0;
  // Shared confirm dialog (radio edit, reboot, start ota): holds the CLI command until confirmed.
  lv_obj_t*       _admin_confirm_popup = NULL;  // confirm-dialog backdrop
  lv_obj_t*       _admin_confirm_lbl = NULL;
  char            _admin_pending_cmd[80] = {0};
  bool            _admin_confirm_to_popup = false;
  // Blind setperm sub-form
  lv_obj_t*       _admin_perm_popup = NULL;
  lv_obj_t*       _admin_perm_key_ta = NULL;
  lv_obj_t*       _admin_perm_role_dd = NULL;
  lv_obj_t*       _admin_perm_kb = NULL;
  // Admin login modal (explicit every time -- no auto-login). Holds the pending target while
  // the modal is up; a successful admin reply opens the Admin screen for it.
  uint8_t         _admin_pending_pk[6] = {0};
  uint8_t         _admin_pending_type = 0;
  lv_obj_t*       _admin_pending_ret = NULL;
  char            _admin_pending_name[CHAT_PEER_NAME_MAX] = {0};
  bool            _admin_login_active = false;    // modal is up + awaiting a reply
  lv_obj_t*       _admin_login_popup = NULL;
  lv_obj_t*       _admin_login_title = NULL;
  lv_obj_t*       _admin_login_pw = NULL;
  lv_obj_t*       _admin_login_save = NULL;       // "Save password" checkbox
  lv_obj_t*       _admin_login_btn = NULL;
  lv_obj_t*       _admin_login_status = NULL;     // "Authenticating..." / "Timed out" / ...
  lv_obj_t*       _admin_login_kb = NULL;
  lv_timer_t*     _admin_login_to_timer = NULL;   // 6s -> "Timed out"
  lv_timer_t*     _admin_login_btn_timer = NULL;  // 1s -> re-enable the Login button

  // LVGL display + input. Resolution is read from the LGFX device after
  // setRotation, so this UITask doesn't care whether the variant chose
  // portrait or landscape.
  uint16_t _screen_w;
  uint16_t _screen_h;
  static constexpr uint16_t kBufferLines = 64;  // target; falls back if RAM tight

  lv_disp_draw_buf_t _draw_buf;
  lv_color_t*        _buf1;
  lv_color_t*        _buf2;
  size_t             _buf_px = 0;          // pixels in the full (double) buffer; for low-mem restore
  bool               _lvbuf_lowmem = false;// true while shrunk for an OTA (single tiny buffer)
  lv_disp_drv_t      _disp_drv;
  lv_indev_drv_t     _indev_drv;
  lv_indev_drv_t     _kbd_drv;             // physical-keyboard keypad indev (T-Deck); unused otherwise
  lv_group_t*        _kbd_group = nullptr; // group the keypad indev drives (textareas join via makeSelTextarea)
  bool               _tb_pressed_prev = false;  // trackball center-click edge state (T-Deck nav ball)
  uint32_t           _tb_scroll_ms = 0;         // lv_tick of the last trackball scroll (touch swallowed briefly after)
  // Debounced prefs persist: sliders apply live but their RELEASED can be eaten by the settings
  // pane's scroll, so a release-only pushPrefs never fires. Mark dirty on value-change; loop() flushes
  // once the value settles (covers brightness + scroll-speed reliably regardless of the release event).
  bool               _prefs_dirty = false;
  uint32_t           _prefs_dirty_ms = 0;
  void               markPrefsDirty() { _prefs_dirty = true; _prefs_dirty_ms = lv_tick_get(); }
  bool               noteInputWake();      // reset idle timer + wake screen on kbd/ball input (like touch)
  void               pollTrackball();      // nav ball -> scroll the active list/chat (no-op without a ball)
  lv_obj_t*          currentScrollable();  // the container the ball should scroll right now (nullptr = none)

  void allocBigDrawBuf();        // (re)allocate the full DMA double buffer (with the fallback ladder)
  void setLowMemDrawBuf(bool low);  // shrink/restore the draw buffer to free internal RAM during OTA
  static void battSampleTask(void* arg);  // core-0 task: poll the INA219 + publish raw mV/mA (off the UI core)

  static UITask* _instance;
  static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
  static void touchpad_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);
  static void kbd_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);   // physical keyboard -> LVGL keypad
  static void touch_isr();              // GT911 INT edge -> wake the touch task (no I2C in ISR)
  static void touchTaskFn(void* arg);   // high-prio task: read GT911 on INT, latch for read_cb
  static void dismiss_kb_cb(lv_event_t* e);   // tap a background -> hide any open keyboard
  static void splash_dismiss_cb(lv_timer_t* t);

  lv_obj_t* buildSplashScreen();
  lv_obj_t* buildHomeScreen();
  void      rebuildContactsList();
  // Display name = local override if set, else the contact's advert name.
  const char* displayName(const uint8_t* pubkey, const char* realname, char* buf, size_t cap);
  bool      contactPasses(const struct ContactInfo& c, const char* search);
  void      fillContactDisplaySet(ContactListView& lv, const char* search);  // shared filter+sort
  void      rebuildChannelsList();
  // Drain the backend→UI event queue (new/sent msgs, delivery, telemetry) and
  // apply each to the message store / LVGL. Runs at the top of loop(), UI core.
  void      drainEvents();

  // ----- Notifications (Phase B) -----
  bool      notifyEnabled() const { return _node_prefs && _node_prefs->notify_enable != 0; }
  void      markUnread(const char* key);
  void      clearUnread(const char* key);
  bool      isUnread(const char* key) const;
  bool      isMuted(const char* key) const;            // explicitly muted
  bool      isExplicitUnmuted(const char* key) const;  // explicitly unmuted (overrides mute-by-default)
  bool      effectiveMuted(const char* key) const;     // explicit choice, else mute-by-default
  void      setMuted(const char* key, bool on);   // records an explicit choice + persists via backend
  static void kebab_mute_cb(lv_event_t* e);
  // Wake the screen + show the tappable banner + chime for an incoming message.
  void      onIncomingNotify(const char* conv_key, const char* sender,
                             const char* text, bool is_channel);
  void      ensureBanner();   // build the reused banner widgets once (called at startup)
  void      showBanner(const char* conv_key, const char* sender,
                       const char* text, bool is_channel);
  void      hideBanner();
  // Resolve a conv-key ("ch_"+hex => channel by secret; else hex => contact by
  // pubkey) and open that chat. Returns false if it no longer resolves.
  bool      openConversationByKey(const char* conv_key);
  static void banner_body_cb(lv_event_t* e);
  static void banner_close_cb(lv_event_t* e);
  static void banner_timer_cb(lv_timer_t* t);
  // Optimistic async send: append a SENDING bubble with a fresh client token and
  // post CMD_Send; the backend replies EV_SendResult with the real ack. Returns
  // the token. `pubkey6` (contact) or `channel_idx` selects the recipient.
  uint32_t  postSend(bool is_channel, const uint8_t* pubkey6, int channel_idx,
                     const char* conv_key, const char* sender, const char* text);
  // Command-posting helpers (UI → backend). Static so the LVGL event callbacks
  // (which only have _instance) can call them uniformly. All read _instance.
  static void pushPrefs();   // CMD_UpdatePrefs from _node_prefs (persist settings)
  static void pushRadio();   // CMD_ApplyRadio from _node_prefs (apply + persist radio)
  static void pushAdvert();  // CMD_Advert (re-broadcast self)
  static void postPubkeyCmd(mproxy::CmdKind kind, const uint8_t* pk);  // fav/path/telem/share
  static void channel_clicked_cb(lv_event_t* e);
  // Contacts recycler: build the row pool once; bind a slot to a display index;
  // re-window the pool on scroll. contact_row_cb opens the tapped contact's chat.
  // Shared row primitives (reused by the Contacts tab and the share/insert picker
  // so every contact row looks + fills identically -- tweak once, applies everywhere).
  static void makeContactRowSlot(lv_obj_t* parent, ContactRow& w, lv_event_cb_t tap_cb, void* tap_ctx);
  void      fillContactRow(ContactRow& w, const ContactInfo& c);
  // Reusable contact-list component (operate on a ContactListView&).
  void      clistBuild(ContactListView& lv, lv_obj_t* parent);
  void      clistRefresh(ContactListView& lv, const char* empty_text);  // after owner fills rows/count
  void      clistBind(ContactListView& lv, int slot, int disp);
  void      clistRelayout(ContactListView& lv);
  static void clist_row_cb(lv_event_t* e);     // event user_data = the ContactListView*
  static void clist_scroll_cb(lv_event_t* e);
  static void clistOpenContact(const ContactInfo& c);  // Contacts tab tap: open the chat
  static void clistPickSelect(const ContactInfo& c);   // picker tap: run the share/insert action
  void      fillLeadRow(ContactListView& lv, ContactRow& w);  // render the lead row (+New / self)
  static void clistNewContact();   // Contacts tab lead: open the New Contact screen
  static void clistPickSelf();     // picker lead: select our own contact
  void      buildContactRows(lv_obj_t* parent);
  static void contacts_scroll_cb(lv_event_t* e);
  static void contacts_search_ta_cb(lv_event_t* e);
  static void contacts_kb_cb(lv_event_t* e);
  static int  crow_cmp(const void* a, const void* b);
  void      showContactsFilter();
  void      refreshContactsFilterChecks();
  static void contacts_filter_btn_cb(lv_event_t* e);
  static void contacts_order_pick_cb(lv_event_t* e);
  static void contacts_filt_pick_cb(lv_event_t* e);

  void      openChat(const char* peer_name);
  void      updateChatHeader();   // name + avatar + route status (live, from the snapshot)
  // Shared header chrome so every screen matches: a fixed top bar (UI_SURFACE, flex
  // row) with a 36px square back-target + title. Returns the bar so callers can append
  // right-side actions (a Save button flexes to the right of the grown title).
  lv_obj_t* makeBackButton(lv_obj_t* bar, lv_event_cb_t cb);
  lv_obj_t* makeHeaderBar(lv_obj_t* parent, const char* title, lv_event_cb_t back_cb);
  // Standard modal: dimmed full-screen backdrop + a centered surface card (flex
  // column). Returns the card; *backdrop_out gets the backdrop. backdrop_tap_cb
  // (optional) fires when the backdrop outside the card is tapped -- the card swallows
  // its own taps. Caller manages show/hide and may re-align the card (e.g. top, to
  // leave room for a keyboard).
  lv_obj_t* makeBackdrop(lv_event_cb_t tap_cb);   // dim full-screen overlay (shared by all popups)
  lv_obj_t* makeModalCard(lv_obj_t** backdrop_out, lv_event_cb_t backdrop_tap_cb);
  void      rebuildChatHistory();
  void      ensureChatRenderCtx();           // recompute tz/now-day/bubble-cap before (re)rendering
  void      appendChatBubble(const ChatMessage* m);  // render one message, advancing the render cursor
  void      appendPendingChat();             // incremental: append just the new message(s), else rebuild
  bool      chatAtBottom();                  // chat history pinned to the tail (vs scrolled up reading)
  void      snapChatToBottom();              // re-window to the newest screenful + scroll to the live tail
  void      updateChatScrollbar();           // chat-specific thumb (tail-anchored, hides backward depth) + chevron
  int       chatUnitPx();                    // densest per-message row height (line + bubble pad + row gap)
  static void chat_sb_scroll_cb(lv_event_t* e);  // chat scroll -> reposition the tail-anchored thumb
  static void chat_sb_drag_cb(lv_event_t* e);    // drag the chat thumb -> scrub / snap to the live tail
  static void chat_jump_cb(lv_event_t* e);       // chevron tap -> snap to the live tail
  int       computeChatWinK();               // max bubbles that fit the chat band (densest unit) + overscan
  int       residentCap();                   // max resident bubbles before paging trims the far edge
  ChatMessage* ensureGather();               // lazily allocate the PSRAM gather scratch (residentCap)
  int       gatherRange(int first, int last, ChatMessage* out);  // copy file records [first,last) (buf+SD)
  void      expandChatOlder();               // page an older chunk in at the top; trim the bottom; re-anchor
  void      pageChatNewer();                 // page a newer chunk in at the bottom; trim the top; re-anchor
  static void chat_history_scroll_cb(lv_event_t* e);   // detect scroll-to-edge -> request older/newer
  void      layoutChatBody(bool keyboard_shown);
  void      updateCharCount();   // refresh the compose char-count label (shown while typing)
  void      enforceComposeLimit();// hard-cap the compose field at the 160-byte wire limit (UTF-8 safe)
  void      encodeComposeMentionsLive();// rewrite committed "@name" -> "@[name]" in the box as you type
  // When a bottom-docked keyboard `kb` is shown, scroll `scroll` so the focused
  // field `ta` sits just above it (no-op if already clear of the keyboard).
  // resetKbScroll() restores the container when the keyboard hides.
  lv_obj_t*       _kb_scroll;        // container temporarily padded for the keyboard
  lv_coord_t      _kb_scroll_pad;    // its original bottom padding
  void      raiseFieldForKb(lv_obj_t* scroll, lv_obj_t* kb, lv_obj_t* ta);
  void      resetKbScroll();

  void      sendCurrentMessage();
  // Stable per-conversation storage key from a 6-byte id (pubkey or channel secret).
  static void convKey(const uint8_t* key6, bool is_channel, char* out, size_t cap);
  void      resendMessage(const struct ChatMessage* m);
  static void chat_resend_cb(lv_event_t* e);
  void      ensureInsertPopup();
  void      showInsertMenu();
  void      openContactPicker(int action);
  void      buildContactPickerScreen();
  void      rebuildPicker();
  void      closeContactPicker();
  static void pick_search_ta_cb(lv_event_t* e);
  static void pick_kb_cb(lv_event_t* e);
  static void pick_close_cb(lv_event_t* e);
  void      closeInsertPopup();
  void      insertContactRef(const uint8_t* pubkey, uint8_t type, const char* name);
  static void chat_back_cb(lv_event_t* e);
  static void chat_input_event_cb(lv_event_t* e);
  static void chat_send_cb(lv_event_t* e);
  static void chat_kb_event_cb(lv_event_t* e);
  static void chat_plus_cb(lv_event_t* e);
  static void insert_backdrop_cb(lv_event_t* e);
  static void insert_myinfo_cb(lv_event_t* e);
  static void insert_share_cb(lv_event_t* e);
  static void insert_sendchannel_cb(lv_event_t* e);   // (+) -> pick a channel -> insert link
  lv_obj_t*   buildChannelRow(lv_obj_t* parent, int idx, const ChannelDetails& ch,
                              lv_event_cb_t tap_cb, bool show_unread);   // shared channel row
  int         findChannelBySecret(const uint8_t* secret, int seclen);   // -> slot idx or -1
  void        openNewChannelPrefilled(const char* name, const uint8_t* secret, int seclen, lv_obj_t* return_screen);
  void        buildChannelPickerScreen();
  void        openChannelPicker();
  void        closeChannelPicker();
  void        rebuildChannelPicker();
  static void chpick_close_cb(lv_event_t* e);
  static void chpick_search_cb(lv_event_t* e);
  static void chpick_kb_cb(lv_event_t* e);
  static void chanpick_cb(lv_event_t* e);             // a channel row -> insert its meshcore:// link
  static void contact_card_cb(lv_event_t* e);   // tap inline card -> open / add contact
  static void card_free_cb(lv_event_t* e);       // free the card's heap target on delete
  void      renderRichBody(lv_obj_t* bubble, const ChatMessage* m, uint32_t fg);  // text + inline rich tokens
  void      buildContactCard(lv_obj_t* parent, const ChatMessage* m,
                             const uint8_t* pubkey, uint8_t type, const char* name);
  void        buildChannelLinkCard(lv_obj_t* parent, const char* name, const uint8_t* secret, int seclen);
  static void chan_card_cb(lv_event_t* e);      // tap a meshcore:// channel link -> join

  // Clickable chips in message text: @mentions (open a known contact) and
  // #hashtags (open a known public channel, or offer to join it). Resolved
  // per-tap by hit-testing the touched character (resolveChip, from sel_event_cb).
  enum ChipKind : uint8_t { CHIP_MENTION, CHIP_HASHTAG };
  void      resolveChip(uint8_t kind, const char* name);   // act on the tapped chip
  static void chanSenderTapCb(lv_event_t* e);              // tap a channel sender avatar -> open contact
  void      openOrJoinHashtag(const char* name);   // open the channel, or offer to join it
  void      showJoinChannel(const char* name);     // "Add channel #name?" confirm
  static void joinch_join_cb(lv_event_t* e);
  static void joinch_cancel_cb(lv_event_t* e);

  // In-conversation search
  static void chat_search_ta_event_cb(lv_event_t* e);
  static void chat_search_close_cb(lv_event_t* e);

  // Contact Info page
  void      openContactInfo(const uint8_t* pubkey, lv_obj_t* return_screen);
  void      buildContactInfoScreen();
  // Owner profile = a contact page for yourself. openProfile() is the single entry
  // point (launcher hero, or any link to our own pubkey).
  void      buildProfileScreen();
  void      openProfile(lv_obj_t* return_screen = NULL);
  static void profile_back_cb(lv_event_t* e);
  static void profile_kb_event_cb(lv_event_t* e);
  // Channel Details page (kebab -> Details on a channel chat) + rename.
  void      buildChannelInfoScreen();
  void      openChannelInfo(int channel_idx, lv_obj_t* return_screen);
  void      populateChannelInfo();
  void      commitChannelName();        // rename: post RenameChannel if the name changed
  static void chinfo_back_cb(lv_event_t* e);
  static void chinfo_ta_event_cb(lv_event_t* e);
  static void chinfo_kb_event_cb(lv_event_t* e);
  static void chinfo_key_cb(lv_event_t* e);   // hero key -> full secret popup
  static void chinfo_delete_cb(lv_event_t* e);
  void        showDeleteChannelConfirm();
  static void delch_cancel_cb(lv_event_t* e);
  static void delch_confirm_cb(lv_event_t* e);
  void      populateContactInfo();
  void      applyCinfoMode();           // show/hide widgets for view vs add mode
  void      refreshCinfoHero();         // hero avatar/title/key from current state
  struct ContactInfo* cinfoContact();   // mutable ptr, or NULL
  void      showToast(const char* text);
  void      reportCrashIfAny();   // boot: if a coredump is stored, save a decodable report to SD/SPIFFS
  void      commitCinfoField(lv_obj_t* ta);
  // Shared top-layer popup + kebab overflow menu
  lv_obj_t* ensureMenuPopup();          // returns the (cleaned) list, popup hidden
  void      showMenuPopup();
  void      closeMenuPopup();
  void      showShareMenu();            // uses _cinfo_pubkey as the target
  static void chat_kebab_cb(lv_event_t* e);
  static void kebab_details_cb(lv_event_t* e);
  static void kebab_chdetails_cb(lv_event_t* e);   // channel chat kebab "Details"
  static void kebab_search_cb(lv_event_t* e);
  static void kebab_share_cb(lv_event_t* e);
  static void kebab_setpath_cb(lv_event_t* e);
  static void kebab_resetpath_cb(lv_event_t* e);
  static void kebab_clearhistory_cb(lv_event_t* e);
  static void cinfo_back_cb(lv_event_t* e);
  static void cinfo_fav_cb(lv_event_t* e);
  static void cinfo_telemetry_cb(lv_event_t* e);
  static void cinfo_share_cb(lv_event_t* e);
  static void cinfo_clearpath_cb(lv_event_t* e);
  static void cinfo_delete_cb(lv_event_t* e);
  void        showDeleteContactConfirm();
  static void delc_cancel_cb(lv_event_t* e);
  static void delc_confirm_cb(lv_event_t* e);
  static void cinfo_editpath_cb(lv_event_t* e);
  static void cinfo_ta_event_cb(lv_event_t* e);
  static void cinfo_kb_event_cb(lv_event_t* e);
  static void cinfo_save_cb(lv_event_t* e);    // add-mode header Save button
  static void cinfo_name_clicked_cb(lv_event_t* e);
  static void cinfo_key_cb(lv_event_t* e);     // contact hero key line -> full key popup
  static void share_sendto_cb(lv_event_t* e);
  static void share_zerohop_cb(lv_event_t* e);
  void        showChannelShareMenu();          // channel Share submenu (send-to / QR)
  static void chshare_sendto_cb(lv_event_t* e);
  static void chshare_qr_cb(lv_event_t* e);
  static void cinfo_toast_timer_cb(lv_timer_t* t);

  // Path Editor sub-page
  void      openPathEditor(lv_obj_t* return_screen);
  void      buildPathEditorScreen();
  void      populatePathEditor();
  bool      savePathEditor();
  static void path_back_cb(lv_event_t* e);
  static void path_save_cb(lv_event_t* e);
  static void path_ta_event_cb(lv_event_t* e);
  static void path_kb_event_cb(lv_event_t* e);

  // Settings tab
  void      buildSettingsTab(lv_obj_t* parent);
  void      populateSettings();
  void      updateOwnerProfile();   // fill the Settings owner-profile hero
  void      refreshProfilePosition();  // fill the profile lat/lon fields from the live node position
  void      commitNodeName();
  void      applyRadioSettings();
  static void set_name_ta_event_cb(lv_event_t* e);
  static void set_radio_ta_event_cb(lv_event_t* e);
  static void set_kb_event_cb(lv_event_t* e);
public:
  static void ta_done_cb(lv_event_t* e);   // one-line field: Enter commits + drops keyboard
  static lv_obj_t* makeSelTextarea(lv_obj_t* parent);  // textarea + selection wiring (all field sites)
  static void ta_longpress_cb(lv_event_t* e);          // long-press a field -> Cut/Copy/Paste toolbar
private:
  static void set_radio_apply_cb(lv_event_t* e);
  static void set_pathmode_cb(lv_event_t* e);
  static void set_bright_cb(lv_event_t* e);
  static void set_scrollspeed_cb(lv_event_t* e);   // trackball scroll-speed slider (T-Deck)
  static void set_scrollinvert_cb(lv_event_t* e);  // trackball reverse-direction checkbox (T-Deck)
  static void set_touchlock_cb(lv_event_t* e);     // trackball scroll touch-lock slider (T-Deck)
  static void set_rot_cb(lv_event_t* e);
  static void set_font_cb(lv_event_t* e);   // UI font-size tier dropdown (restart to apply)
  static void set_screen_cb(lv_event_t* e);
  void      copyToClipboard(const char* text);
  void      clipSet(uint8_t kind, const char* text, const uint8_t* pubkey,
                    const char* name, uint8_t type);
  void      copyContactRef(const uint8_t* pubkey, uint8_t type, const char* name);  // copy as <hex:type:name>
  // Smart paste: a target field's semantic kind decides which part of a typed
  // clipboard payload gets inserted (contact-ref -> hex field = key, name field =
  // name, chat compose = whole <...> token; plain text -> verbatim everywhere).
  enum FieldKind : uint8_t { FK_PLAIN, FK_NAME, FK_HEX, FK_CHAT_COMPOSE };
  uint8_t      fieldKindOf(lv_obj_t* ta);
  const char*  pasteTextFor(uint8_t field_kind);   // clipboard text adapted to the field
  // ----- Text-selection controller -----
  void      makeLabelSelectable(lv_obj_t* lbl);   // wire a chat-text label for selection
  void      makeCardSelectable(lv_obj_t* card);   // wire a contact card for atomic selection
  void      makeHeroCopyable(lv_obj_t* hero);      // hero -> selectable contact card (long-press Copy)
  void      setHeroTarget(lv_obj_t* hero, const uint8_t* pubkey, uint8_t type, const char* name);
  void      setHeroChannelTarget(lv_obj_t* hero, const char* name, const uint8_t* secret, int seclen);
  void      ensureSelHandles();                    // build the two triangle handles (once)
  void      beginLabelSel(lv_obj_t* lbl, lv_point_t abs_pt);
  void      selectWordAt(lv_obj_t* lbl, lv_point_t abs_pt);   // double-tap word select
  void      beginCardSel(lv_obj_t* card);
  void      updateSelDrag(lv_point_t abs_pt);      // extend the dragged endpoint
  void      applyLabelSel();                        // push sel_lo/hi to the native highlight
  void      positionSelHandles();
  void      finishSel();                            // freeze + show the context menu
  void      endSelection();                         // clear everything, back to idle
  void      showSelMenu();                          // floating horizontal toolbar over the selection
  void      selAnchorRect(lv_area_t* out);          // screen bbox of the current selection
  void      selStart(lv_point_t* tip, lv_coord_t* line_h);  // screen point of the selection start / cursor
  bool      selPointInside(lv_point_t p);           // is a touch point within the selection?
  void      copySelection();
  static uint32_t labelCharAt(lv_obj_t* lbl, lv_point_t abs_pt);  // touch point -> char-id
  static void sel_event_cb(lv_event_t* e);          // long-press/drag/release on a target
  static void sel_handle_cb(lv_event_t* e);         // drag a handle to re-adjust an endpoint
  static void sel_catcher_cb(lv_event_t* e);        // tap outside selection -> dismiss
  // Editable text fields: long-press -> toolbar (Cut/Copy/Paste/Select All).
  void      beginTextareaSel(lv_obj_t* ta);
  bool      taSelRange(lv_obj_t* ta, uint32_t* s, uint32_t* e);  // current native selection range
  static void selmenu_cut_cb(lv_event_t* e);
  static void selmenu_paste_cb(lv_event_t* e);
  static void selmenu_copy_cb(lv_event_t* e);
  static void selmenu_selectall_cb(lv_event_t* e);
  void      applyPreset(int idx);
  static void radio_preset_cb(lv_event_t* e);
  static void radio_preset_pick_cb(lv_event_t* e);
  void      commitPosition();
  void      commitTz();
  static void set_tz_ta_event_cb(lv_event_t* e);
  static void set_clock_cb(lv_event_t* e);
  static void set_rtc_cb(lv_event_t* e);
  static void set_meta_cb(lv_event_t* e);           // "show metadata in chat" toggle
  static void set_time_ta_event_cb(lv_event_t* e);  // HH/MM/SS boxes: kb on focus, commit on defocus
  static void set_time_ampm_cb(lv_event_t* e);      // AM/PM dropdown change -> commit
  static void set_time_reset_cb(lv_event_t* e);     // ↻ button -> re-apply shown HH:MM:00 (zero seconds)
  void        commitManualTime();                   // shown HH:MM -> set clock to HH:MM:00 (local -> UTC)
  void        refreshTimeFields();                  // live-tick the boxes not being edited
  void        refreshSignalMeter();                 // 1Hz: decay SNR envelope -> header bars (hysteretic)
  void        renderClock(bool colon_hidden);       // clock w/ fixed-width recolored ':' (blink w/o jiggle)
  void        buildSignalPopup();                   // lazily build the signal-meter tuning modal
  void        openSignalMeter();                    // populate from prefs + show
  void        commitSigMeter();                     // parse the 4 fields -> prefs + pushPrefs
  static void sig_meter_btn_cb(lv_event_t* e);      // Radio pane button -> openSignalMeter
  static void sigmod_ta_event_cb(lv_event_t* e);    // field: kb on focus, commit on defocus
  static void sigmod_dismiss_cb(lv_event_t* e);     // backdrop tap -> hide
  static void set_autolock_cb(lv_event_t* e);
  static void net_ta_event_cb(lv_event_t* e);   // MQTT text-field keyboard
  static void wifi_ta_event_cb(lv_event_t* e);  // WiFi text field: kb on focus, apply on defocus
  static void wifi_apply_cb(lv_event_t* e);     // WiFi DHCP/override changed -> apply
  static void wifi_enable_cb(lv_event_t* e);    // WiFi enable toggled -> apply + reboot prompt
  static void mqtt_save_cb(lv_event_t* e);
  static void ntp_sync_cb(lv_event_t* e);       // "Sync clock now"
  static void presets_update_cb(lv_event_t* e); // "Update radio presets"
  static void set_ota_url_event_cb(lv_event_t* e); // OTA URL field: kb on focus, commit on defocus
  static void ota_update_cb(lv_event_t* e);     // "Update firmware" -> resolve source -> post OtaUpdate
  static void ota_prerelease_cb(lv_event_t* e);    // include pre-releases (saved) -> re-fetch + rebuild list
  static void ota_custom_url_cb(lv_event_t* e);    // custom-URL mode (saved) -> reveal URL field
  static void ota_release_dd_cb(lv_event_t* e);    // release picked -> refresh current/latest hint
  static void emoji_dl_cb(lv_event_t* e);          // "Download emoji" / Cancel
  void        updateOtaFieldStates();           // show/hide the disclosure controls; grey when no IP
  void        rebuildOtaReleaseList();          // (re)populate the dropdown from the backend cache
  void        wifiApplyFromForm();              // gather WiFi fields -> prefs -> ApplyWifi
  void        updateWifiFieldStates();          // grey/enable IP fields per DHCP/override
  void        refreshNetStatus();               // update status labels + live IP fields
  void        refreshGpsStatus();               // update the GPS debug line
  void        updateBatteryGauge();             // sample the power monitor + drive the top-bar gauge
  static void set_avatar_cb(lv_event_t* e);
  static void set_theme_cb(lv_event_t* e);
  static void theme_async_cb(void* unused);   // deferred theme rebuild (lv_async_call)
  static void set_mention_colors_cb(lv_event_t* e);
  static void set_hashtag_colors_cb(lv_event_t* e);
  static void set_chsender_colors_cb(lv_event_t* e);
  void applyThemeByName(const char* name);   // resolve a built-in / SD theme by name + apply live
  void applyTheme(const UiPalette& pal);      // swap g_ui_palette + rebuild the UI in place
  int  buildThemeOptions(char* out, size_t cap, const char* sel, int* sel_idx);  // dropdown options + selected index
  static void set_history_cb(lv_event_t* e);
  static void sd_mount_cb(lv_event_t* e);   // top-bar SD icon -> user-initiated (re)mount
  bool sdMountQuiesced();                    // (re)mount with the radio paused (boot condition); returns ready()
  static void category_row_cb(lv_event_t* e);
  static void settings_back_cb(lv_event_t* e);
  static void settings_tab_changed_cb(lv_event_t* e);
  lv_obj_t* makeSettingsPane(int idx, const char* title);
  lv_obj_t* makeCategoryRow(lv_obj_t* parent, const char* icon, const char* title, const char* desc, int cat);
  void showSettingsCategory(int cat);
  void settingsBackToLauncher();
  void setHomeChrome(bool show);   // show/hide the logo header (+resize tabview) for pane drill-in
  static void set_notify_cb(lv_event_t* e);
  static void set_mutedef_cb(lv_event_t* e);
  // Phase-1 additions: telemetry policy + advanced toggles + share-me.
  static void set_telem_cb(lv_event_t* e);          // user_data 0/1/2 = base/loc/env
  static void set_pwrmon_cb(lv_event_t* e);         // power-monitor select
  static void set_batt_type_cb(lv_event_t* e);      // battery type select
  void        commitBattCapacity();                 // save the capacity field
  static void set_autoadd_cb(lv_event_t* e);
  static void set_autoadd_hops_cb(lv_event_t* e);
  static void set_rxboost_cb(lv_event_t* e);
  static void set_advnum_ta_event_cb(lv_event_t* e); // multiack / rxdelay / airtime / gps-interval fields
  void      commitAdvNumbers();
  static void set_gps_cb(lv_event_t* e);             // enable optional UART GPS
  static void set_gps_uart_cb(lv_event_t* e);        // pick the GPS UART socket (UART0/UART1)
  static void set_radio_sw_cb(lv_event_t* e);        // LoRa radio on/off kill-switch
  static void pw_eye_cb(lv_event_t* e);              // generic inline show/hide (user_data = textarea)
  static lv_obj_t* attachInlineEye(lv_obj_t* ta);    // hidden-by-default + inline eye toggle
  // Set-PIN dialog (enter twice, must match)
  void      buildPinSetPopup();
  void      openPinSet();
  static void set_pin_btn_cb(lv_event_t* e);
  static void pinset_save_cb(lv_event_t* e);
  static void pinset_dismiss_cb(lv_event_t* e);
  static void pinset_ta_event_cb(lv_event_t* e);
  static void pinset_kb_event_cb(lv_event_t* e);
  // PIN lock screen
  void      buildLockScreen();
  void      showLock();                              // lock now (overlay keypad entry)
  void      updateLockDots();                        // refresh the entered-digit indicators
  lv_obj_t* makeLockKey(lv_obj_t* grid, const char* text, intptr_t tag, lv_coord_t w, lv_coord_t h);
  static void lock_now_cb(lv_event_t* e);
  static void lock_key_cb(lv_event_t* e);            // a keypad key press
  static void set_shareme_cb(lv_event_t* e);         // export own contact as QR
  void      showSharePosWarning();
  static void profile_key_cb(lv_event_t* e);   // owner hero key line -> long-press copy
  static void profile_kebab_cb(lv_event_t* e);       // owner profile overflow menu
  static void profile_share_cb(lv_event_t* e);       // kebab -> Share submenu
  void        showOwnerShareMenu();                  // QR + adverts (owner)
  static void profile_advert_zhop_cb(lv_event_t* e);
  static void profile_advert_flood_cb(lv_event_t* e);
  static void profile_shareqr_cb(lv_event_t* e);
  // Shared full-public-key popup (full hex + copy), opened from either key line.
  void        showKeyPopup(const char* hex);
  static void keypop_copy_cb(lv_event_t* e);
  static void keypop_close_cb(lv_event_t* e);
  // Private-key popup (owner profile kebab): one box; Export fills it, Import writes it.
  static void profile_keypopup_cb(lv_event_t* e);
  void        buildKeyPopup();
  void        openKeyPopup();
  static void impkey_export_cb(lv_event_t* e);
  static void impkey_dismiss_cb(lv_event_t* e);
  static void impkey_cancel_cb(lv_event_t* e);
  static void impkey_ta_event_cb(lv_event_t* e);
  static void impkey_kb_event_cb(lv_event_t* e);
  static void impkey_import_cb(lv_event_t* e);
  void        showImportConfirm();
  static void impkey_confirm_cb(lv_event_t* e);
  static void impkey_confirm_cancel_cb(lv_event_t* e);
  static void set_pos_ta_event_cb(lv_event_t* e);
  static void set_sharepos_cb(lv_event_t* e);
  static void sharepos_confirm_cb(lv_event_t* e);
  static void sharepos_cancel_cb(lv_event_t* e);
  void      showInfoPopup(const char* title, const char* body);
  static void info_close_cb(lv_event_t* e);
  static void insert_paste_cb(lv_event_t* e);

  // Share-as-QR screen
  void      openShareQR(const uint8_t* pubkey, uint8_t type, const char* name, lv_obj_t* return_screen);
  void      buildShareQRScreen();
  static void qr_back_cb(lv_event_t* e);
  static void share_showqr_cb(lv_event_t* e);

  // New-channel screen (create/join by name + base64 key)
  void      buildNewChannelScreen();
  void      openNewChannel();
  bool      createChannelFromForm();
  void      refreshNewChannelHero();    // live hexagon preview + derived-key prefill
  static void newchan_public_cb(lv_event_t* e);  // "Public channel" checkbox toggled
  static void newchan_key_cb(lv_event_t* e);     // hero key tap -> full key popup
  // Adding a contact reuses the Contact Info screen in CINFO_ADD mode.
  void      openNewContact(lv_obj_t* return_screen);   // blank manual entry (typed/pasted hex key)
  void      openNewContactPrefilled(const uint8_t* pubkey, uint8_t type, const char* advname,
                                     lv_obj_t* return_screen);   // from a contact ref (key locked)
  bool      saveContactFromForm();     // add-mode: AddContact (+ SetNameOvr) from the form
  bool      resolveContactKey();       // add-mode: parse the bare-hex key field into _cinfo_c
  void      openShareChannelQR(int idx);        // channel -> meshcore://channel/add QR
  static void kebab_chanshare_cb(lv_event_t* e); // channel chat kebab "Share"

  // Node-info / status screen
  void      buildNodeInfoScreen();
  void      openNodeInfo();
  void      refreshNodeInfo();
  void      buildUpdateScreen();   // standalone "Update" screen: firmware-update UI + emoji-pack section
  void      openUpdate();
  void      refreshUpdate();       // 1 Hz: current/latest, dropdown rebuild, status, emoji-pack state
  void      buildUpdateEmojiSection(lv_obj_t* body);  // lower "Emoji pack" section (Phase C)
  void      refreshUpdateEmoji();  // emoji-pack state line + button/progress (Phase C)
  static void update_timer_cb(lv_timer_t* t);
  static void update_back_cb(lv_event_t* e);
  static void open_nodeinfo_cb(lv_event_t* e);
  static void profile_hero_cb(lv_event_t* e);   // owner hero -> Profile settings pane
  static void nodeinfo_back_cb(lv_event_t* e);
  static void nodeinfo_timer_cb(lv_timer_t* t);

  // Repeater/room-server console: login + CLI command send
  void      postCliCommand(const uint8_t* pubkey6, const char* conv_key, const char* text);
  void      buildLoginPopup();
  void      openLogin(const uint8_t* pubkey6, const char* name);
  static void kebab_login_cb(lv_event_t* e);
  static void login_dismiss_cb(lv_event_t* e);
  static void login_go_cb(lv_event_t* e);
  static void login_ta_event_cb(lv_event_t* e);
  static void login_kb_event_cb(lv_event_t* e);
  static void newchan_open_cb(lv_event_t* e);   // "+ New channel" list entry
  static void newchan_back_cb(lv_event_t* e);
  static void newchan_save_cb(lv_event_t* e);
  static void newchan_ta_event_cb(lv_event_t* e);
  static void newchan_kb_event_cb(lv_event_t* e);

  // Repeater/room Admin screen
  void      buildAdminScreen();
  void      openAdmin(const uint8_t* pubkey6, uint8_t type, const char* name, lv_obj_t* ret);
  void      adminRebuildFields();       // (re)create the spec rows for _admin_type
  bool      adminBusy();                // a request is in flight -> reject + toast (one at a time)
  void      adminFetch(int row);        // send one "get <key>" for this field (Refresh button)
  void      adminApply(int row);        // send one "set <key> <v>" for this field (Apply button)
  bool      adminConsumeReply(const char* text);  // returns true if the reply was consumed
  void      adminSendAction(const char* cli, bool to_popup);  // send a CLI command, await its ack
  void      adminConfirm(const char* msg, const char* cmd, bool to_popup);  // confirm dialog -> adminSendAction
  void      adminSetFieldValue(int row, const char* value);  // parsed "> v" -> widget
  void      adminSetFieldEnabled(int row, bool en);  // grey out / activate a field (+ its Apply)
  int       adminRowOfWidget(lv_obj_t* w);   // visible-row owning a field widget, or -1
  void      openAdminLogin(const uint8_t* pubkey6, uint8_t type, const char* name, lv_obj_t* ret);
  void      buildAdminLogin();
  void      adminLoginSend();           // post the login + arm 1s button-grey + 6s timeout + status
  void      adminLoginResult(bool ok, bool is_admin);  // route a LoginResult to the modal
  static void kebab_admin_cb(lv_event_t* e);
  static void admin_login_go_cb(lv_event_t* e);
  static void admin_login_dismiss_cb(lv_event_t* e);
  static void admin_login_pw_event_cb(lv_event_t* e);
  static void admin_login_timeout_cb(lv_timer_t* t);
  static void admin_login_btn_reenable_cb(lv_timer_t* t);
  static void admin_back_cb(lv_event_t* e);
  static void admin_field_event_cb(lv_event_t* e);   // textarea focus -> keyboard
  static void admin_field_refresh_cb(lv_event_t* e); // per-field Refresh (get)
  static void admin_field_apply_cb(lv_event_t* e);   // per-field Apply (set)
  static void admin_action_cb(lv_event_t* e);        // action button -> CLI command
  static void admin_reply_timeout_cb(lv_timer_t* t);
  static void admin_confirm_go_cb(lv_event_t* e);
  static void admin_confirm_cancel_cb(lv_event_t* e);
  static void admin_perm_open_cb(lv_event_t* e);
  static void admin_perm_go_cb(lv_event_t* e);
  static void admin_perm_dismiss_cb(lv_event_t* e);

public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial),
      _lgfx(NULL), _node_prefs(NULL), _sensors(NULL),
      _started(false), _last_tick_ms(0), _msgcount(0),
      _last_input_ms(0), _display_off(false), _swallow_touch(false), _touch_down(false), _backlight_duty(153),
      _splash_screen(NULL), _home_screen(NULL),
      _header(NULL), _header_logo(NULL), _clock_label(NULL), _clock_last(0), _clock_blink(0),
      _tabview(NULL),
      _tab_contacts(NULL), _tab_channels(NULL), _tab_settings(NULL),
      _clist{},
      _contacts_search_ta(NULL), _contacts_kb(NULL),
      _contacts_filter_btn(NULL), _cfilt_popup(NULL), _cfilt_order_grp(NULL),
      _cfilt_filt_grp(NULL), _contacts_order(1), _contacts_filt(0),
      _channels_list(NULL), _status_label(NULL),
      _contacts_dirty(false), _contacts_rebuilt_ms(0),
      _contacts_pending(false), _channels_pending(false), _chat_pending(false),
      _unread_count(0), _muted_count(0), _unmuted_count(0), _banner(NULL), _banner_avatar(NULL), _banner_avatar_lbl(NULL),
      _banner_title(NULL), _banner_body(NULL), _banner_timer(NULL),
      _pending_chime(UIEventType::none),
      _pick_popup(NULL), _pick_search_ta(NULL), _pick_kb(NULL),
      _pick_title(NULL), _pick_action(0), _pick_list{},
      _chpick_popup(NULL), _chpick_list(NULL), _chpick_search_ta(NULL), _chpick_kb(NULL), _chpick_filter{},
      _chat_screen(NULL), _chat_title(NULL), _chat_avatar(NULL), _chat_avatar_lbl(NULL),
      _chat_status(NULL), _chat_history(NULL), _chat_sb(NULL), _chat_jump_btn(NULL),
      _chat_compose(NULL), _chat_input(NULL), _chat_send_btn(NULL), _chat_count_lbl(NULL), _chat_keyboard(NULL),
      _insert_popup(NULL), _insert_list(NULL),
      _chat_is_channel(false), _chat_channel_idx(-1), _chat_contact_type(0),
      _chat_search_bar(NULL), _chat_search_ta(NULL), _search_active(false),
      _sending_lbl(NULL), _dot_frame(0), _anim_ms(0),
      _cinfo_screen(NULL), _cinfo_body(NULL), _cinfo_hero(NULL), _cinfo_title(NULL),
      _cinfo_avatar(NULL), _cinfo_avatar_lbl(NULL), _cinfo_realname(NULL), _cinfo_key(NULL),
      _cinfo_fav_lbl(NULL), _cinfo_name_ta(NULL), _cinfo_keyfull(NULL),
      _cinfo_lat_ta(NULL), _cinfo_lon_ta(NULL), _cinfo_type_lbl(NULL),
      _cinfo_lastheard(NULL), _cinfo_telem(NULL), _cinfo_hops(NULL), _cinfo_hops_x(NULL),
      _cinfo_outpath(NULL), _cinfo_kb(NULL), _cinfo_active_ta(NULL),
      _cinfo_actions(NULL), _cinfo_pos_field(NULL), _cinfo_type_field(NULL),
      _cinfo_lh_field(NULL), _cinfo_tel_field(NULL), _cinfo_hops_row(NULL), _cinfo_outpath_row(NULL),
      _cinfo_delete_btn(NULL), _delc_popup(NULL), _delch_popup(NULL),
      _cinfo_header_title(NULL), _cinfo_save_btn(NULL), _cinfo_key_field(NULL),
      _cinfo_key_ta(NULL), _cinfo_err(NULL),
      _chinfo_screen(NULL), _chinfo_body(NULL), _chinfo_hero(NULL), _chinfo_avatar(NULL), _chinfo_avatar_lbl(NULL),
      _chinfo_title(NULL), _chinfo_key(NULL), _chinfo_name_ta(NULL), _chinfo_kb(NULL),
      _chinfo_active_ta(NULL), _chinfo_return_screen(NULL),
      _cinfo_mode(CINFO_VIEW), _cinfo_addkey_locked(false), _cinfo_haskey(false),
      _cinfo_return_screen(NULL),
      _menu_popup(NULL), _menu_list(NULL), _toast(NULL), _toast_timer(NULL),
      _sig_meter(NULL), _sig_bar_count(-1), _path_return_screen(NULL),
      _path_screen(NULL), _path_size_dd(NULL), _path_ta(NULL), _path_kb(NULL), _path_err(NULL),
      _set_profile_avatar(NULL), _set_profile_avatar_lbl(NULL), _set_profile_name(NULL), _set_profile_key(NULL),
      _prof_hero(NULL), _prof_avatar(NULL), _prof_avatar_lbl(NULL), _prof_name(NULL), _prof_key(NULL),
      _profile_screen(NULL), _profile_body(NULL), _profile_kb(NULL), _profile_return_screen(NULL),
      _set_name_ta(NULL), _set_freq_ta(NULL), _set_bw_dd(NULL), _set_sf_dd(NULL),
      _set_cr_dd(NULL), _set_txp_ta(NULL), _set_path_dd(NULL), _set_bright_slider(NULL),
      _set_rot_dd(NULL), _set_screen_dd(NULL), _set_tz_ta(NULL), _set_clock_chk(NULL), _set_rtc_chk(NULL),
      _set_time_hh(NULL), _set_time_mm(NULL), _set_time_ampm(NULL), _set_meta_chk(NULL), _set_autolock_chk(NULL),
      _set_wifi_en(NULL), _set_wifi_ssid(NULL), _set_wifi_pw(NULL),
      _set_wifi_dhcp(NULL), _set_wifi_dns_ovr(NULL), _set_wifi_ip(NULL), _set_wifi_mask(NULL),
      _set_wifi_gw(NULL), _set_wifi_dns(NULL), _set_wifi_status(NULL),
      _set_ntp_en(NULL), _set_ntp_server(NULL), _set_ntp_status(NULL), _set_preset_status(NULL),
      _set_ota_url_ta(NULL), _set_ota_url_field(NULL), _set_ota_status(NULL), _set_ota_btn(NULL), _set_ota_lbl(NULL),
      _set_ota_curlatest(NULL), _set_ota_release_field(NULL), _set_ota_release_dd(NULL),
      _set_ota_prerel_chk(NULL), _set_ota_customchk(NULL), _ota_dd_count(0),
      _update_body(NULL), _update_kb(NULL),
      _otafail_popup(NULL), _otafail_lbl(NULL),
      _set_mqtt_en(NULL), _set_mqtt_host(NULL), _set_mqtt_port(NULL), _set_mqtt_user(NULL), _set_mqtt_pw(NULL),
      _set_mqtt_topic(NULL), _set_mqtt_tls(NULL), _set_mqtt_rx(NULL), _set_mqtt_tx(NULL), _set_mqtt_status(NULL),
      _set_avatar_dd(NULL), _set_theme_dd(NULL), _set_mention_chk(NULL), _set_hashtag_chk(NULL), _set_chsender_chk(NULL), _set_history_chk(NULL), _set_notify_chk(NULL), _set_mutedef_chk(NULL), _set_kb(NULL),
      _set_active_ta(NULL),
      _set_launcher(NULL), _set_pane{}, _set_pane_body{}, _set_active_pane(NULL),
      _set_key_ta(NULL),
      _set_lat_ta(NULL), _set_lon_ta(NULL), _set_sharepos(NULL),
      _set_telem_base_dd(NULL), _set_telem_loc_dd(NULL), _set_telem_env_dd(NULL),
      _set_autoadd_chk(NULL), _set_autoadd_hops_dd(NULL), _set_rxboost_chk(NULL),
      _set_multiack_ta(NULL), _set_rxdelay_ta(NULL), _set_airtime_ta(NULL),
      _set_gps_chk(NULL), _set_gps_interval_ta(NULL), _set_gps_status(NULL),
      _set_radio_sw(NULL),
      _pinset_popup(NULL), _pinset_ta1(NULL), _pinset_ta2(NULL), _pinset_err(NULL), _pinset_kb(NULL),
      _sigmod_popup(NULL), _sigmod_kb(NULL), _sig_min_ta(NULL), _sig_max_ta(NULL), _sig_hold_ta(NULL), _sig_decay_ta(NULL),
      _lock_screen(NULL), _lock_err(NULL), _lock_len(0), _locked(false),
      _confirm_popup(NULL), _joinch_popup(NULL), _joinch_lbl(NULL),
      _info_popup(NULL), _info_title_lbl(NULL), _info_body_lbl(NULL),
      _keypop_popup(NULL), _keypop_lbl(NULL), _keypop_hex{},
      _impkey_popup(NULL), _impkey_ta(NULL), _impkey_err(NULL), _impkey_kb(NULL), _impkey_confirm(NULL), _impkey_bytes{},
      _qr_screen(NULL), _qr_code(NULL), _qr_name_lbl(NULL), _qr_key_lbl(NULL),
      _qr_return_screen(NULL),
      _newchan_screen(NULL), _newchan_name_ta(NULL), _newchan_key_ta(NULL),
      _newchan_key_field(NULL), _newchan_public_chk(NULL), _newchan_err(NULL), _newchan_kb(NULL),
      _newchan_hero_av(NULL), _newchan_hero_avl(NULL), _newchan_hero_nm(NULL), _newchan_hero_key(NULL), _newchan_return_screen(NULL),
      _nodeinfo_screen(NULL), _nodeinfo_lbl(NULL), _nodeinfo_timer(NULL),
      _login_popup(NULL), _login_card(NULL), _login_title(NULL), _login_pw_ta(NULL),
      _login_save_chk(NULL), _login_auto_chk(NULL), _login_kb(NULL),
      _screen_w(0), _screen_h(0),
      _kb_scroll(NULL), _kb_scroll_pad(0),
      _buf1(NULL), _buf2(NULL), _msgs(&_rammsgs), _sd_off_ts(0),
      _last_snap_version(0), _send_seq(0), _cinfo_valid(false) {
        _chat_peer[0] = 0;
        _cinfo_override[0] = 0;
        _chat_key[0] = 0;
        _banner_key[0] = 0;
        _search_filter[0] = 0;
        _crash_note[0] = 0;   // _clip_text is PSRAM-allocated in begin()
        _joinch_name[0] = 0;
        _chinfo_name[0] = 0;
        memset(_chinfo_secret, 0, sizeof(_chinfo_secret));
        _clip_kind = CLIP_EMPTY;
        memset(&_sel, 0, sizeof(_sel));
        _contacts_filter[0] = 0;
        _pick_filter[0] = 0;
        _telem_text[0] = 0;
        memset(_telem_pubkey, 0, sizeof(_telem_pubkey));
        memset(_chat_pubkey, 0, sizeof(_chat_pubkey));
        memset(_cinfo_pubkey, 0, sizeof(_cinfo_pubkey));
      }

  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  bool hasDisplay() const { return _started; }
#ifdef PIN_BUZZER
  bool isBuzzerQuiet() { return _buzzer.isQuiet(); }
#else
  bool isBuzzerQuiet() { return true; }
#endif
  void shutdown(bool restart = false) { (void)restart; }

  // AbstractUITask overrides
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void sentMsg(const char* peer, const char* text) override;
  void telemetryResponse(const uint8_t* pubkey, const char* from_name, const uint8_t* lpp, uint8_t lpp_len) override;
  void msgDelivered(uint32_t ack) override;
  void noteRxSnr(float snr_db, uint32_t hold_ms, uint32_t tau_ms) override;  // -> signal-meter envelope
  void otaFailed(const char* reason) override;       // -> OtaFailed event -> wake + modal
  void otaRebooting() override;                      // -> OtaRebooting event -> wake + toast
  void showOtaFailedModal(const char* reason);       // build/show the "update failed" modal
  void loginResult(const uint8_t* pubkey, bool ok, uint8_t is_admin, uint16_t keep_alive_secs) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
