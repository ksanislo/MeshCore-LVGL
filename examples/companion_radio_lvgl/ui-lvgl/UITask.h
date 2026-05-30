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
  uint8_t         _backlight_duty;      // duty to restore on wake (configured brightness)
  lv_obj_t*       _splash_screen;
  lv_obj_t*       _home_screen;
  lv_obj_t*       _header;              // home logo+clock bar (hidden while a settings pane is open)
  lv_obj_t*       _header_logo;         // MeshCore wordmark (recolorable alpha img)
  lv_obj_t*       _clock_label;         // live device clock in the home header
  uint32_t        _clock_last;          // last shown second (1 Hz throttle)
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

  // Notifications. _unread_keys is an in-RAM set of conv-keys (pubkey/secret hex)
  // with at least one message arrived-but-not-viewed; cleared when the chat opens.
  // (Not persisted across reboot in v1.) The banner is one reused top-layer card.
  static const int UNREAD_MAX = 64;
  char            _unread_keys[UNREAD_MAX][CHAT_PEER_NAME_MAX];
  uint8_t         _unread_count;
  // Per-conversation mute: in-RAM set (seeded from the backend at begin, persisted via
  // CmdKind::SetMute). Muted = no wake/banner/buzzer, but the unread mark still shows.
  static const int MUTE_MAX = 64;
  char            _muted_keys[MUTE_MAX][CHAT_PEER_NAME_MAX];
  int             _muted_count;
  lv_obj_t*       _banner;              // top-layer notification card (reused)
  lv_obj_t*       _banner_avatar;       // colored circle / type-glyph holder
  lv_obj_t*       _banner_avatar_lbl;   // grapheme / glyph inside the circle
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

  // Chat (conversation) screen. Three-band: fixed top bar / scrollable history
  // / fixed compose band (compose added with the keyboard step).
  RamMessageStore<CHAT_HISTORY_CAP> _rammsgs;   // session-only fallback
#ifdef HAS_SD_CARD
  SdMessageStore<CHAT_HISTORY_CAP> _sdmsgs;     // persistent (per-conversation files on SD)
#endif
  MessageStore* _msgs;   // -> _rammsgs or _sdmsgs, chosen at begin() from the setting
  uint32_t      _sd_off_ts;  // RTC time saving was disabled (0 = not off; for backfill)
  // Append to the RAM ring always (keeps recent history for the toggle) and to
  // the SD store when persistence is active.
  void storeAppend(bool outgoing, const char* key, const char* sender,
                   const char* text, uint32_t ts,
                   uint8_t status = 0, uint32_t ack = 0, uint32_t expiry_ms = 0, uint32_t cli = 0);
  lv_obj_t*       _chat_screen;
  lv_obj_t*       _chat_title;          // contact name in the chat top bar
  lv_obj_t*       _chat_avatar;         // avatar circle in the chat top bar (branding)
  lv_obj_t*       _chat_avatar_lbl;     // grapheme / type glyph inside the avatar
  lv_obj_t*       _chat_status;         // route line under the name: Direct / Flood / N hops
  lv_obj_t*       _chat_history;        // scrollable message container (the VSA band)
  lv_obj_t*       _chat_sb;             // draggable scrollbar thumb for chat history
  lv_obj_t*       _chat_compose;        // fixed compose band (textarea + send)
  lv_obj_t*       _chat_input;          // lv_textarea
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

  // Contact Info page (+ Path Editor sub-page). Lazily built, reused.
  lv_obj_t*       _cinfo_screen;
  lv_obj_t*       _cinfo_body;          // scrollable form
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
  // UI-owned working copy of the viewed contact (loaded from the snapshot at
  // openContactInfo). The Contact-Info page reads/edits this copy for instant
  // feedback; edits also push a CMD_* the backend applies to the real contact.
  ContactInfo     _cinfo_c;
  bool            _cinfo_valid;
  char            _cinfo_override[CHAT_PEER_NAME_MAX];  // local nickname (optimistic)
  lv_obj_t*       _cinfo_return_screen; // where back goes

  // Latest telemetry readings, stashed for the Contact Info page (one contact's
  // worth -- the most recent response). GPS is applied to the contact position.
  uint8_t         _telem_pubkey[6];
  char            _telem_text[160];

  // Shared top-layer popup (kebab menu / share menu / pickers) + toast.
  lv_obj_t*       _menu_popup;
  lv_obj_t*       _menu_list;
  lv_obj_t*       _toast;
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
  lv_obj_t*       _set_rot_dd;
  lv_obj_t*       _set_screen_dd;       // screen idle-off timeout dropdown
  lv_obj_t*       _set_tz_ta;           // UTC offset (hours) for local-time display
  lv_obj_t*       _set_clock_chk;       // 12-hour clock toggle
  lv_obj_t*       _set_avatar_dd;       // contact avatar color scheme (Default / iOS app)
  lv_obj_t*       _set_history_chk;     // persist chat history to SD toggle
  lv_obj_t*       _set_notify_chk;      // master new-message notifications toggle
  lv_obj_t*       _set_kb;
  lv_obj_t*       _set_active_ta;       // settings textarea currently being edited
  // Settings categories. The pane index is the single source of truth shared by
  // makeSettingsPane(), makeCategoryRow(), and the _set_pane[] arrays; the enum
  // keeps them in sync and CAT_COUNT sizes the arrays + the bounds guard. CAT_ABOUT
  // is a sentinel launcher row that opens Node Info instead of an in-tab pane.
  enum SettingsCat {
    CAT_PROFILE = 0, CAT_RADIO, CAT_TELEMETRY, CAT_NOTIFY,
    CAT_DISPLAY, CAT_POWER, CAT_COUNT,
    CAT_ABOUT = 100,
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
  lv_obj_t*       _set_telem_env_dd;
  // Advanced section.
  lv_obj_t*       _set_autoadd_chk;     // auto-add heard contacts (inverse of manual_add_contacts)
  lv_obj_t*       _set_autoadd_hops_dd; // auto-add max hops
  lv_obj_t*       _set_rxboost_chk;     // SX126x RX boosted gain (reboot to apply)
  lv_obj_t*       _set_multiack_ta;     // extra ACK transmit count
  lv_obj_t*       _set_rxdelay_ta;      // RX delay base (seconds)
  lv_obj_t*       _set_airtime_ta;      // airtime budget factor
  lv_obj_t*       _set_gps_chk;         // enable optional UART GPS (reboot to apply)
  lv_obj_t*       _set_gps_interval_ta; // GPS auto-update interval (seconds)
  lv_obj_t*       _set_radio_sw;        // LoRa radio on/off (off = safe to detach antenna)
  // Set-PIN dialog (two boxes, must match). Lazily built.
  lv_obj_t*       _pinset_popup;
  lv_obj_t*       _pinset_ta1;
  lv_obj_t*       _pinset_ta2;
  lv_obj_t*       _pinset_err;
  lv_obj_t*       _pinset_kb;
  // Lock overlay (PIN entry). Lazily built; _locked gates the whole UI.
  lv_obj_t*       _lock_screen;
  lv_obj_t*       _lock_pin_ta;
  lv_obj_t*       _lock_err;
  lv_obj_t*       _lock_kb;
  bool            _locked;
  lv_obj_t*       _confirm_popup;       // share-position warning modal (top layer)

  // Reusable info modal (telemetry response now; repeater status/CLI later).
  lv_obj_t*       _info_popup;
  lv_obj_t*       _info_title_lbl;
  lv_obj_t*       _info_body_lbl;

  // Full-public-key popup (shared by the contact + owner hero key lines).
  lv_obj_t*       _keypop_popup;
  lv_obj_t*       _keypop_lbl;
  char            _keypop_hex[2 * PUB_KEY_SIZE + 1];

  // Tiny internal clipboard (no OS/LVGL clipboard on-device). Copy fills it;
  // the chat compose "+" menu can paste it.
  char            _clipboard[160];

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
  lv_obj_t*       _newchan_err;
  lv_obj_t*       _newchan_kb;

  // Unified add/import-contact screen: blank from "+ New Contact", or prefilled
  // (key locked) when adding an unknown contact from a DM/channel contact ref.
  lv_obj_t*       _newcon_screen;
  lv_obj_t*       _newcon_name_ta;
  lv_obj_t*       _newcon_key_ta;
  lv_obj_t*       _newcon_err;
  lv_obj_t*       _newcon_kb;
  lv_obj_t*       _newcon_return_screen;
  uint8_t         _newcon_pubkey[PUB_KEY_SIZE];        // set when prefilled
  uint8_t         _newcon_type;
  char            _newcon_advname[CHAT_PEER_NAME_MAX]; // advertised/linked name (override baseline)
  bool            _newcon_prefilled;                   // key is fixed (read-only) when true

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

  // LVGL display + input. Resolution is read from the LGFX device after
  // setRotation, so this UITask doesn't care whether the variant chose
  // portrait or landscape.
  uint16_t _screen_w;
  uint16_t _screen_h;
  static constexpr uint16_t kBufferLines = 64;  // target; falls back if RAM tight

  lv_disp_draw_buf_t _draw_buf;
  lv_color_t*        _buf1;
  lv_color_t*        _buf2;
  lv_disp_drv_t      _disp_drv;
  lv_indev_drv_t     _indev_drv;

  static UITask* _instance;
  static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
  static void touchpad_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data);
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
  bool      isMuted(const char* key) const;
  void      setMuted(const char* key, bool on);   // updates local set + persists via backend
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
  void      layoutChatBody(bool keyboard_shown);
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
  static void contact_card_cb(lv_event_t* e);   // tap inline card -> open / add contact
  static void card_free_cb(lv_event_t* e);       // free the card's heap target on delete
  void      renderRichBody(lv_obj_t* bubble, const ChatMessage* m);  // text + inline rich tokens
  void      buildContactCard(lv_obj_t* parent, const ChatMessage* m,
                             const uint8_t* pubkey, uint8_t type, const char* name);

  // Clickable @mentions in message bubbles
  void      attachMentions(lv_obj_t* bubble, const char* text);
  static void mention_bubble_cb(lv_event_t* e);
  static void mention_pick_cb(lv_event_t* e);
  static void mention_free_cb(lv_event_t* e);

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
  void      populateContactInfo();
  struct ContactInfo* cinfoContact();   // mutable ptr, or NULL
  void      showToast(const char* text);
  void      commitCinfoField(lv_obj_t* ta);
  // Shared top-layer popup + kebab overflow menu
  lv_obj_t* ensureMenuPopup();          // returns the (cleaned) list, popup hidden
  void      showMenuPopup();
  void      closeMenuPopup();
  void      showShareMenu();            // uses _cinfo_pubkey as the target
  static void chat_kebab_cb(lv_event_t* e);
  static void kebab_details_cb(lv_event_t* e);
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
  static void cinfo_editpath_cb(lv_event_t* e);
  static void cinfo_ta_event_cb(lv_event_t* e);
  static void cinfo_kb_event_cb(lv_event_t* e);
  static void cinfo_name_clicked_cb(lv_event_t* e);
  static void cinfo_key_cb(lv_event_t* e);     // contact hero key line -> full key popup
  static void share_sendto_cb(lv_event_t* e);
  static void share_zerohop_cb(lv_event_t* e);
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
  void      commitNodeName();
  void      applyRadioSettings();
  static void set_name_ta_event_cb(lv_event_t* e);
  static void set_radio_ta_event_cb(lv_event_t* e);
  static void set_kb_event_cb(lv_event_t* e);
public:
  static void ta_done_cb(lv_event_t* e);   // one-line field: Enter commits + drops keyboard
private:
  static void set_radio_apply_cb(lv_event_t* e);
  static void set_pathmode_cb(lv_event_t* e);
  static void set_bright_cb(lv_event_t* e);
  static void set_rot_cb(lv_event_t* e);
  static void set_screen_cb(lv_event_t* e);
  void      copyToClipboard(const char* text);
  void      applyPreset(int idx);
  static void radio_preset_cb(lv_event_t* e);
  static void radio_preset_pick_cb(lv_event_t* e);
  void      commitPosition();
  void      commitTz();
  static void set_tz_ta_event_cb(lv_event_t* e);
  static void set_clock_cb(lv_event_t* e);
  static void set_avatar_cb(lv_event_t* e);
  static void set_history_cb(lv_event_t* e);
  static void category_row_cb(lv_event_t* e);
  static void settings_back_cb(lv_event_t* e);
  static void settings_tab_changed_cb(lv_event_t* e);
  lv_obj_t* makeSettingsPane(int idx, const char* title);
  lv_obj_t* makeCategoryRow(lv_obj_t* parent, const char* icon, const char* title, const char* desc, int cat);
  void showSettingsCategory(int cat);
  void settingsBackToLauncher();
  void setHomeChrome(bool show);   // show/hide the logo header (+resize tabview) for pane drill-in
  static void set_notify_cb(lv_event_t* e);
  // Phase-1 additions: telemetry policy + advanced toggles + share-me.
  static void set_telem_cb(lv_event_t* e);          // user_data 0/1/2 = base/loc/env
  static void set_autoadd_cb(lv_event_t* e);
  static void set_autoadd_hops_cb(lv_event_t* e);
  static void set_rxboost_cb(lv_event_t* e);
  static void set_advnum_ta_event_cb(lv_event_t* e); // multiack / rxdelay / airtime / gps-interval fields
  void      commitAdvNumbers();
  static void set_gps_cb(lv_event_t* e);             // enable optional UART GPS
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
  void      showLock();                              // lock now (overlay PIN entry)
  static void lock_now_cb(lv_event_t* e);
  static void lock_unlock_cb(lv_event_t* e);
  static void lock_ta_event_cb(lv_event_t* e);
  static void lock_kb_event_cb(lv_event_t* e);
  static void set_shareme_cb(lv_event_t* e);         // export own contact as QR
  void      showSharePosWarning();
  static void profile_key_cb(lv_event_t* e);   // owner hero key line -> full key popup
  // Shared full-public-key popup (full hex + copy), opened from either key line.
  void        showKeyPopup(const char* hex);
  static void keypop_copy_cb(lv_event_t* e);
  static void keypop_close_cb(lv_event_t* e);
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
  void      buildNewContactScreen();
  void      openNewContact(lv_obj_t* return_screen);   // blank manual entry
  void      openNewContactPrefilled(const uint8_t* pubkey, uint8_t type, const char* advname,
                                     lv_obj_t* return_screen);   // from a contact ref (key locked)
  bool      saveContactFromForm();
  static void newcon_back_cb(lv_event_t* e);
  static void newcon_save_cb(lv_event_t* e);
  static void newcon_ta_event_cb(lv_event_t* e);
  static void newcon_kb_event_cb(lv_event_t* e);
  void      openShareChannelQR(int idx);        // channel -> meshcore://channel/add QR
  static void kebab_chanshare_cb(lv_event_t* e); // channel chat kebab "Share"

  // Node-info / status screen
  void      buildNodeInfoScreen();
  void      openNodeInfo();
  void      refreshNodeInfo();
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

public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial),
      _lgfx(NULL), _node_prefs(NULL), _sensors(NULL),
      _started(false), _last_tick_ms(0), _msgcount(0),
      _last_input_ms(0), _display_off(false), _swallow_touch(false), _backlight_duty(153),
      _splash_screen(NULL), _home_screen(NULL),
      _header(NULL), _header_logo(NULL), _clock_label(NULL), _clock_last(0),
      _tabview(NULL),
      _tab_contacts(NULL), _tab_channels(NULL), _tab_settings(NULL),
      _clist{},
      _contacts_search_ta(NULL), _contacts_kb(NULL),
      _contacts_filter_btn(NULL), _cfilt_popup(NULL), _cfilt_order_grp(NULL),
      _cfilt_filt_grp(NULL), _contacts_order(1), _contacts_filt(0),
      _channels_list(NULL), _status_label(NULL),
      _contacts_dirty(false), _contacts_rebuilt_ms(0),
      _contacts_pending(false), _channels_pending(false),
      _unread_count(0), _muted_count(0), _banner(NULL), _banner_avatar(NULL), _banner_avatar_lbl(NULL),
      _banner_title(NULL), _banner_body(NULL), _banner_timer(NULL),
      _pending_chime(UIEventType::none),
      _pick_popup(NULL), _pick_search_ta(NULL), _pick_kb(NULL),
      _pick_title(NULL), _pick_action(0), _pick_list{},
      _chat_screen(NULL), _chat_title(NULL), _chat_avatar(NULL), _chat_avatar_lbl(NULL),
      _chat_status(NULL), _chat_history(NULL), _chat_sb(NULL),
      _chat_compose(NULL), _chat_input(NULL), _chat_keyboard(NULL),
      _insert_popup(NULL), _insert_list(NULL),
      _chat_is_channel(false), _chat_channel_idx(-1), _chat_contact_type(0),
      _chat_search_bar(NULL), _chat_search_ta(NULL), _search_active(false),
      _sending_lbl(NULL), _dot_frame(0), _anim_ms(0),
      _cinfo_screen(NULL), _cinfo_body(NULL), _cinfo_title(NULL),
      _cinfo_avatar(NULL), _cinfo_avatar_lbl(NULL), _cinfo_realname(NULL), _cinfo_key(NULL),
      _cinfo_fav_lbl(NULL), _cinfo_name_ta(NULL), _cinfo_keyfull(NULL),
      _cinfo_lat_ta(NULL), _cinfo_lon_ta(NULL), _cinfo_type_lbl(NULL),
      _cinfo_lastheard(NULL), _cinfo_telem(NULL), _cinfo_hops(NULL), _cinfo_hops_x(NULL),
      _cinfo_outpath(NULL), _cinfo_kb(NULL), _cinfo_active_ta(NULL),
      _cinfo_return_screen(NULL),
      _menu_popup(NULL), _menu_list(NULL), _toast(NULL), _path_return_screen(NULL),
      _path_screen(NULL), _path_size_dd(NULL), _path_ta(NULL), _path_kb(NULL), _path_err(NULL),
      _set_profile_avatar(NULL), _set_profile_avatar_lbl(NULL), _set_profile_name(NULL), _set_profile_key(NULL),
      _prof_avatar(NULL), _prof_avatar_lbl(NULL), _prof_name(NULL), _prof_key(NULL),
      _profile_screen(NULL), _profile_body(NULL), _profile_kb(NULL), _profile_return_screen(NULL),
      _set_name_ta(NULL), _set_freq_ta(NULL), _set_bw_dd(NULL), _set_sf_dd(NULL),
      _set_cr_dd(NULL), _set_txp_ta(NULL), _set_path_dd(NULL), _set_bright_slider(NULL),
      _set_rot_dd(NULL), _set_screen_dd(NULL), _set_tz_ta(NULL), _set_clock_chk(NULL), _set_avatar_dd(NULL), _set_history_chk(NULL), _set_notify_chk(NULL), _set_kb(NULL),
      _set_active_ta(NULL),
      _set_launcher(NULL), _set_pane{}, _set_pane_body{}, _set_active_pane(NULL),
      _set_key_ta(NULL),
      _set_lat_ta(NULL), _set_lon_ta(NULL), _set_sharepos(NULL),
      _set_telem_base_dd(NULL), _set_telem_loc_dd(NULL), _set_telem_env_dd(NULL),
      _set_autoadd_chk(NULL), _set_autoadd_hops_dd(NULL), _set_rxboost_chk(NULL),
      _set_multiack_ta(NULL), _set_rxdelay_ta(NULL), _set_airtime_ta(NULL),
      _set_gps_chk(NULL), _set_gps_interval_ta(NULL),
      _set_radio_sw(NULL),
      _pinset_popup(NULL), _pinset_ta1(NULL), _pinset_ta2(NULL), _pinset_err(NULL), _pinset_kb(NULL),
      _lock_screen(NULL), _lock_pin_ta(NULL), _lock_err(NULL), _lock_kb(NULL), _locked(false),
      _confirm_popup(NULL),
      _info_popup(NULL), _info_title_lbl(NULL), _info_body_lbl(NULL),
      _keypop_popup(NULL), _keypop_lbl(NULL), _keypop_hex{},
      _qr_screen(NULL), _qr_code(NULL), _qr_name_lbl(NULL), _qr_key_lbl(NULL),
      _qr_return_screen(NULL),
      _newchan_screen(NULL), _newchan_name_ta(NULL), _newchan_key_ta(NULL),
      _newchan_err(NULL), _newchan_kb(NULL),
      _newcon_screen(NULL), _newcon_name_ta(NULL), _newcon_key_ta(NULL),
      _newcon_err(NULL), _newcon_kb(NULL), _newcon_return_screen(NULL),
      _newcon_pubkey{}, _newcon_type(0), _newcon_advname{}, _newcon_prefilled(false),
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
        _clipboard[0] = 0;
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
  void loginResult(const uint8_t* pubkey, bool ok, uint8_t is_admin, uint16_t keep_alive_secs) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
