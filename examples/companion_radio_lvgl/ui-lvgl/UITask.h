#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <helpers/ui/LGFXDisplay.h>
#include "../../companion_radio/AbstractUITask.h"
#include "../../companion_radio/NodePrefs.h"
#include "MessageStore.h"

#ifndef CHAT_HISTORY_CAP
  #define CHAT_HISTORY_CAP 48
#endif

class UITask : public AbstractUITask {
  LGFX_Device*    _lgfx;
  NodePrefs*      _node_prefs;
  SensorManager*  _sensors;
  bool            _started;
  uint32_t        _last_tick_ms;
  int             _msgcount;
  lv_obj_t*       _splash_screen;
  lv_obj_t*       _home_screen;
  lv_obj_t*       _header_label;
  lv_obj_t*       _tabview;
  lv_obj_t*       _tab_contacts;
  lv_obj_t*       _tab_channels;
  lv_obj_t*       _tab_settings;
  lv_obj_t*       _contacts_table;      // lv_table inside _tab_contacts (rows drawn, not objects)
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
  bool            _contacts_dirty;      // set by newMsg(), serviced (throttled) in loop()
  uint32_t        _contacts_rebuilt_ms; // last rebuild time
  uint32_t        _contacts_sig;        // change-detect signature (add/remove/favourite/rename)
  uint32_t        _contacts_check_ms;   // last signature poll

  // Sorted/filtered view of the address book (favourites first, then recency).
  // Holds indices only -- no per-contact widgets -- so it scales to a full book.
  static const int CONTACTS_MAX_ROWS = 400;
  struct ContactDispRow { uint16_t idx; uint32_t heard; uint8_t fav; };
  ContactDispRow  _crows[CONTACTS_MAX_ROWS];
  int             _crow_count;

  // Chat (conversation) screen. Three-band: fixed top bar / scrollable history
  // / fixed compose band (compose added with the keyboard step).
  RamMessageStore<CHAT_HISTORY_CAP> _msgs;
  lv_obj_t*       _chat_screen;
  lv_obj_t*       _chat_title;          // contact name in the chat top bar
  lv_obj_t*       _chat_history;        // scrollable message container (the VSA band)
  lv_obj_t*       _chat_compose;        // fixed compose band (textarea + send)
  lv_obj_t*       _chat_input;          // lv_textarea
  lv_obj_t*       _chat_keyboard;       // lv_keyboard, shown on input focus
  lv_obj_t*       _insert_popup;        // backdrop for the insert (+) menu
  lv_obj_t*       _insert_list;         // lv_list inside the popup
  char            _chat_peer[CHAT_PEER_NAME_MAX];  // peer whose chat is open ("" = none)
  bool            _chat_is_channel;     // send via sendGroupMessage vs sendMessage
  uint8_t         _chat_pubkey[6];      // recipient prefix for contact sends
  int             _chat_channel_idx;    // channel slot for channel sends

  // In-conversation search (reveals a bar under the top bar; filters history).
  lv_obj_t*       _chat_search_bar;
  lv_obj_t*       _chat_search_ta;
  bool            _search_active;
  char            _search_filter[40];

  // Contact Info page (+ Path Editor sub-page). Lazily built, reused.
  lv_obj_t*       _cinfo_screen;
  lv_obj_t*       _cinfo_body;          // scrollable form
  lv_obj_t*       _cinfo_title;
  lv_obj_t*       _cinfo_key;
  lv_obj_t*       _cinfo_fav_lbl;
  lv_obj_t*       _cinfo_name_ta;
  lv_obj_t*       _cinfo_keyfull;       // read-only full-hex label
  lv_obj_t*       _cinfo_lat_ta;
  lv_obj_t*       _cinfo_lon_ta;
  lv_obj_t*       _cinfo_type_dd;
  lv_obj_t*       _cinfo_lastheard;
  lv_obj_t*       _cinfo_hops;
  lv_obj_t*       _cinfo_hops_x;        // clear-to-flood button
  lv_obj_t*       _cinfo_outpath;
  lv_obj_t*       _cinfo_kb;
  lv_obj_t*       _cinfo_active_ta;     // textarea currently being edited
  uint8_t         _cinfo_pubkey[32];    // full key of the contact being viewed
  lv_obj_t*       _cinfo_return_screen; // where back goes

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
  lv_obj_t*       _set_name_ta;
  lv_obj_t*       _set_freq_ta;
  lv_obj_t*       _set_bw_dd;
  lv_obj_t*       _set_sf_dd;
  lv_obj_t*       _set_cr_dd;
  lv_obj_t*       _set_txp_ta;
  lv_obj_t*       _set_path_dd;
  lv_obj_t*       _set_bright_slider;
  lv_obj_t*       _set_rot_dd;
  lv_obj_t*       _set_kb;
  lv_obj_t*       _set_active_ta;       // settings textarea currently being edited
  lv_obj_t*       _set_key_ta;          // read-only self public key (scrolls horizontally)
  lv_obj_t*       _set_lat_ta;
  lv_obj_t*       _set_lon_ta;
  lv_obj_t*       _set_sharepos;        // "share position" checkbox
  lv_obj_t*       _confirm_popup;       // share-position warning modal (top layer)

  // Tiny internal clipboard (no OS/LVGL clipboard on-device). Copy fills it;
  // the chat compose "+" menu can paste it.
  char            _clipboard[160];

  // Share-as-QR screen (name + truncated key band, QR below). Lazily built.
  lv_obj_t*       _qr_screen;
  lv_obj_t*       _qr_code;
  lv_obj_t*       _qr_name_lbl;
  lv_obj_t*       _qr_key_lbl;
  lv_obj_t*       _qr_return_screen;

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
  static void splash_dismiss_cb(lv_timer_t* t);

  lv_obj_t* buildSplashScreen();
  lv_obj_t* buildHomeScreen();
  void      rebuildContactsList();
  bool      contactPasses(const struct ContactInfo& c);
  uint32_t  contactsSignature();
  void      rebuildChannelsList();
  static void channel_clicked_cb(lv_event_t* e);
  static void contacts_table_cb(lv_event_t* e);
  static void contacts_table_draw_cb(lv_event_t* e);
  static void contacts_search_ta_cb(lv_event_t* e);
  static void contacts_kb_cb(lv_event_t* e);
  static int  crow_cmp(const void* a, const void* b);
  void      showContactsFilter();
  void      refreshContactsFilterChecks();
  static void contacts_filter_btn_cb(lv_event_t* e);
  static void contacts_order_pick_cb(lv_event_t* e);
  static void contacts_filt_pick_cb(lv_event_t* e);

  void      openChat(const char* peer_name);
  void      rebuildChatHistory();
  void      layoutChatBody(bool keyboard_shown);
  void      sendCurrentMessage();
  void      ensureInsertPopup();
  void      showInsertMenu();
  void      showContactPicker();
  void      buildContactPicker(lv_obj_t* list, lv_event_cb_t cb, bool styled);
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
  static void picker_pick_cb(lv_event_t* e);
  static void add_contact_cb(lv_event_t* e);
  static void buildContactCard(lv_obj_t* bubble, const ChatMessage* m,
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
  static void cinfo_type_cb(lv_event_t* e);
  static void cinfo_ta_event_cb(lv_event_t* e);
  static void cinfo_kb_event_cb(lv_event_t* e);
  static void cinfo_name_clicked_cb(lv_event_t* e);
  static void share_sendto_cb(lv_event_t* e);
  static void share_zerohop_cb(lv_event_t* e);
  static void share_pick_cb(lv_event_t* e);
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
  void      commitNodeName();
  void      applyRadioSettings();
  static void set_name_ta_event_cb(lv_event_t* e);
  static void set_radio_ta_event_cb(lv_event_t* e);
  static void set_kb_event_cb(lv_event_t* e);
  static void set_radio_apply_cb(lv_event_t* e);
  static void set_pathmode_cb(lv_event_t* e);
  static void set_bright_cb(lv_event_t* e);
  static void set_rot_cb(lv_event_t* e);
  void      copyToClipboard(const char* text);
  void      applyPreset(int idx);
  static void radio_preset_cb(lv_event_t* e);
  static void radio_preset_pick_cb(lv_event_t* e);
  void      commitPosition();
  void      showSharePosWarning();
  static void set_copykey_cb(lv_event_t* e);
  static void set_pos_ta_event_cb(lv_event_t* e);
  static void set_sharepos_cb(lv_event_t* e);
  static void sharepos_confirm_cb(lv_event_t* e);
  static void sharepos_cancel_cb(lv_event_t* e);
  static void insert_paste_cb(lv_event_t* e);

  // Share-as-QR screen
  void      openShareQR(const uint8_t* pubkey, uint8_t type, const char* name, lv_obj_t* return_screen);
  void      buildShareQRScreen();
  static void qr_back_cb(lv_event_t* e);
  static void share_showqr_cb(lv_event_t* e);

public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial),
      _lgfx(NULL), _node_prefs(NULL), _sensors(NULL),
      _started(false), _last_tick_ms(0), _msgcount(0),
      _splash_screen(NULL), _home_screen(NULL),
      _header_label(NULL),
      _tabview(NULL),
      _tab_contacts(NULL), _tab_channels(NULL), _tab_settings(NULL),
      _contacts_table(NULL), _contacts_search_ta(NULL), _contacts_kb(NULL),
      _contacts_filter_btn(NULL), _cfilt_popup(NULL), _cfilt_order_grp(NULL),
      _cfilt_filt_grp(NULL), _contacts_order(1), _contacts_filt(0),
      _channels_list(NULL), _status_label(NULL),
      _contacts_dirty(false), _contacts_rebuilt_ms(0),
      _contacts_sig(0), _contacts_check_ms(0), _crow_count(0),
      _chat_screen(NULL), _chat_title(NULL), _chat_history(NULL),
      _chat_compose(NULL), _chat_input(NULL), _chat_keyboard(NULL),
      _insert_popup(NULL), _insert_list(NULL),
      _chat_is_channel(false), _chat_channel_idx(-1),
      _chat_search_bar(NULL), _chat_search_ta(NULL), _search_active(false),
      _cinfo_screen(NULL), _cinfo_body(NULL), _cinfo_title(NULL), _cinfo_key(NULL),
      _cinfo_fav_lbl(NULL), _cinfo_name_ta(NULL), _cinfo_keyfull(NULL),
      _cinfo_lat_ta(NULL), _cinfo_lon_ta(NULL), _cinfo_type_dd(NULL),
      _cinfo_lastheard(NULL), _cinfo_hops(NULL), _cinfo_hops_x(NULL),
      _cinfo_outpath(NULL), _cinfo_kb(NULL), _cinfo_active_ta(NULL),
      _cinfo_return_screen(NULL),
      _menu_popup(NULL), _menu_list(NULL), _toast(NULL), _path_return_screen(NULL),
      _path_screen(NULL), _path_size_dd(NULL), _path_ta(NULL), _path_kb(NULL), _path_err(NULL),
      _set_name_ta(NULL), _set_freq_ta(NULL), _set_bw_dd(NULL), _set_sf_dd(NULL),
      _set_cr_dd(NULL), _set_txp_ta(NULL), _set_path_dd(NULL), _set_bright_slider(NULL),
      _set_rot_dd(NULL), _set_kb(NULL), _set_active_ta(NULL), _set_key_ta(NULL),
      _set_lat_ta(NULL), _set_lon_ta(NULL), _set_sharepos(NULL), _confirm_popup(NULL),
      _qr_screen(NULL), _qr_code(NULL), _qr_name_lbl(NULL), _qr_key_lbl(NULL),
      _qr_return_screen(NULL),
      _screen_w(0), _screen_h(0),
      _buf1(NULL), _buf2(NULL) {
        _chat_peer[0] = 0;
        _search_filter[0] = 0;
        _clipboard[0] = 0;
        _contacts_filter[0] = 0;
        memset(_chat_pubkey, 0, sizeof(_chat_pubkey));
        memset(_cinfo_pubkey, 0, sizeof(_cinfo_pubkey));
      }

  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  bool hasDisplay() const { return _started; }
  bool isBuzzerQuiet() { return true; }
  void shutdown(bool restart = false) { (void)restart; }

  // AbstractUITask overrides
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void sentMsg(const char* peer, const char* text) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
};
