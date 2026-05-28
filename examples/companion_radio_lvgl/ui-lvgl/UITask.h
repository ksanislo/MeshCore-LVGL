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
  lv_obj_t*       _contacts_list;       // lv_list inside _tab_contacts
  lv_obj_t*       _contacts_empty;      // "no contacts yet" label, shown when list is empty
  lv_obj_t*       _channels_list;       // lv_list inside _tab_channels
  lv_obj_t*       _status_label;        // header status (bottom of contacts tab)
  bool            _contacts_dirty;      // set by newMsg(), serviced (throttled) in loop()
  uint32_t        _contacts_rebuilt_ms; // last rebuild time
  uint32_t        _contacts_sig;        // change-detect signature (add/remove/favourite/rename)
  uint32_t        _contacts_check_ms;   // last signature poll

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
  void      addContactRow(const struct ContactInfo& c, uint32_t now_secs);
  uint32_t  contactsSignature();
  void      rebuildChannelsList();
  static void channel_clicked_cb(lv_event_t* e);

  void      openChat(const char* peer_name);
  void      rebuildChatHistory();
  void      layoutChatBody(bool keyboard_shown);
  void      sendCurrentMessage();
  void      ensureInsertPopup();
  void      showInsertMenu();
  void      showContactPicker();
  void      closeInsertPopup();
  void      insertContactRef(const uint8_t* pubkey, uint8_t type, const char* name);
  static void contact_clicked_cb(lv_event_t* e);
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

public:
  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
    : AbstractUITask(board, serial),
      _lgfx(NULL), _node_prefs(NULL), _sensors(NULL),
      _started(false), _last_tick_ms(0), _msgcount(0),
      _splash_screen(NULL), _home_screen(NULL),
      _header_label(NULL),
      _tabview(NULL),
      _tab_contacts(NULL), _tab_channels(NULL), _tab_settings(NULL),
      _contacts_list(NULL), _contacts_empty(NULL), _channels_list(NULL), _status_label(NULL),
      _contacts_dirty(false), _contacts_rebuilt_ms(0),
      _contacts_sig(0), _contacts_check_ms(0),
      _chat_screen(NULL), _chat_title(NULL), _chat_history(NULL),
      _chat_compose(NULL), _chat_input(NULL), _chat_keyboard(NULL),
      _insert_popup(NULL), _insert_list(NULL),
      _chat_is_channel(false), _chat_channel_idx(-1),
      _cinfo_screen(NULL), _cinfo_body(NULL), _cinfo_title(NULL), _cinfo_key(NULL),
      _cinfo_fav_lbl(NULL), _cinfo_name_ta(NULL), _cinfo_keyfull(NULL),
      _cinfo_lat_ta(NULL), _cinfo_lon_ta(NULL), _cinfo_type_dd(NULL),
      _cinfo_lastheard(NULL), _cinfo_hops(NULL), _cinfo_hops_x(NULL),
      _cinfo_outpath(NULL), _cinfo_kb(NULL), _cinfo_active_ta(NULL),
      _cinfo_return_screen(NULL),
      _menu_popup(NULL), _menu_list(NULL), _toast(NULL), _path_return_screen(NULL),
      _path_screen(NULL), _path_size_dd(NULL), _path_ta(NULL), _path_kb(NULL), _path_err(NULL),
      _screen_w(0), _screen_h(0),
      _buf1(NULL), _buf2(NULL) {
        _chat_peer[0] = 0;
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
