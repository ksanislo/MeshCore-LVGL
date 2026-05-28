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
  char            _chat_peer[CHAT_PEER_NAME_MAX];  // peer whose chat is open ("" = none)

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
  static void contact_clicked_cb(lv_event_t* e);
  static void chat_back_cb(lv_event_t* e);

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
      _screen_w(0), _screen_h(0),
      _buf1(NULL), _buf2(NULL) { _chat_peer[0] = 0; }

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
