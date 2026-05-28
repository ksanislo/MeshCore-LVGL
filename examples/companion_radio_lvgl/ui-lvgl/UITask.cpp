#include "UITask.h"
#include "meshcore_assets.h"
#include "MyMesh.h"
#include <esp_heap_caps.h>

// main.cpp owns the_mesh; we read contacts/channels via the base class.
extern MyMesh the_mesh;

UITask* UITask::_instance = NULL;

void UITask::disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (!_instance || !_instance->_lgfx) {
    lv_disp_flush_ready(drv);
    return;
  }
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  LGFX_Device& lcd = *_instance->_lgfx;
  // No endWrite(): leaving the transaction open returns immediately after
  // queuing the DMA, so LVGL can render the next chunk (other buffer) while
  // this one transfers. The next flush's setAddrWindow waits for this DMA via
  // bus serialization, and double-buffering guarantees a buffer's DMA is done
  // before LVGL cycles back to reuse it. SPI2 is display-only (bus_shared=0),
  // so holding CS is fine.
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixelsDMA((lgfx::rgb565_t*)color_p, w * h);
  lv_disp_flush_ready(drv);
}

void UITask::touchpad_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  if (!_instance || !_instance->_lgfx) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  uint16_t tx = 0, ty = 0;
  if (_instance->_lgfx->getTouch(&tx, &ty)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = tx;
    data->point.y = ty;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// meshcore.io page background
static constexpr uint32_t BG_HEX = 0x111827;

static void styleAsDarkScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* UITask::buildSplashScreen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  styleAsDarkScreen(scr);

  lv_obj_t* wordmark = lv_img_create(scr);
  lv_img_set_src(wordmark, &meshcore_logo_img);
  lv_obj_align(wordmark, LV_ALIGN_CENTER, 0, 0);

  return scr;
}

static constexpr int HEADER_H   = 48;
static constexpr int TABBAR_H   = 56;
static constexpr uint32_t FG_HEX = 0xD1D5DB;  // tailwind gray-300
static constexpr uint32_t DIM_HEX = 0x6B7280; // tailwind gray-500

lv_obj_t* UITask::buildHomeScreen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  styleAsDarkScreen(scr);
  lv_obj_set_style_pad_all(scr, 0, 0);

  // ----- header strip -----
  lv_obj_t* header = lv_obj_create(scr);
  lv_obj_set_size(header, _screen_w, HEADER_H);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x1F2937), 0);  // gray-800
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_all(header, 8, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  _header_label = lv_label_create(header);
  lv_label_set_text(_header_label, "MeshCore");
  lv_obj_set_style_text_color(_header_label, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_header_label, &lv_font_montserrat_20, 0);
  lv_obj_align(_header_label, LV_ALIGN_LEFT_MID, 4, 0);

  // ----- tabbed body (tabs pinned to bottom) -----
  _tabview = lv_tabview_create(scr, LV_DIR_BOTTOM, TABBAR_H);
  lv_obj_set_size(_tabview, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(_tabview, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(_tabview, lv_color_hex(BG_HEX), 0);
  // Instant tab switches: the content scroll otherwise animates, repainting
  // the whole viewport many times per switch.
  lv_obj_set_style_anim_time(lv_tabview_get_content(_tabview), 0, 0);

  // tab bar styling
  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(_tabview);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x1F2937), 0);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_color(tab_btns, lv_color_hex(FG_HEX),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(tab_btns, 0, 0);

  // Three tabs in ~320 px = ~107 px each. Map lives behind context menus
  // off contacts/channels, since it'll be referenced from those views.
  _tab_contacts = lv_tabview_add_tab(_tabview, LV_SYMBOL_LIST     " Contacts");
  _tab_channels = lv_tabview_add_tab(_tabview, LV_SYMBOL_WIFI     " Channels");
  _tab_settings = lv_tabview_add_tab(_tabview, LV_SYMBOL_SETTINGS " Settings");

  // Opaque tab-page backgrounds: LVGL's default theme makes tab pages
  // transparent, which forces a full-screen background recomposite (and thus
  // repaint of the static header + tab bar) whenever a page's content scrolls.
  // Making them opaque confines scroll repaints to the page's own area.
  for (lv_obj_t* page : {_tab_contacts, _tab_channels, _tab_settings}) {
    lv_obj_set_style_bg_color(page, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  }

  // ----- Contacts tab: scrollable list, with an empty-state label when zero -----
  lv_obj_set_style_pad_all(_tab_contacts, 0, 0);

  _contacts_list = lv_list_create(_tab_contacts);
  lv_obj_set_size(_contacts_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(_contacts_list, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_contacts_list, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_contacts_list, 0, 0);
  lv_obj_set_style_pad_row(_contacts_list, 0, 0);

  _contacts_empty = lv_label_create(_tab_contacts);
  lv_label_set_text(_contacts_empty, "No contacts yet.\nWaiting for first advert...");
  lv_label_set_long_mode(_contacts_empty, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_contacts_empty, _screen_w - 24);
  lv_obj_set_style_text_color(_contacts_empty, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_align(_contacts_empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(_contacts_empty, LV_ALIGN_CENTER, 0, 0);

  _status_label = NULL;  // not used here yet; reserved for future status bar

  // ----- Channels tab: scrollable list -----
  lv_obj_set_style_pad_all(_tab_channels, 0, 0);
  _channels_list = lv_list_create(_tab_channels);
  lv_obj_set_size(_channels_list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(_channels_list, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_channels_list, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_channels_list, 0, 0);
  lv_obj_set_style_pad_row(_channels_list, 0, 0);

  // ----- Settings tab placeholder -----
  lv_obj_t* set_placeholder = lv_label_create(_tab_settings);
  lv_label_set_text(set_placeholder, "Settings not implemented yet.");
  lv_obj_set_style_text_color(set_placeholder, lv_color_hex(DIM_HEX), 0);
  lv_obj_center(set_placeholder);

  return scr;
}

static const char* contactSymbol(uint8_t type) {
  // ADV_TYPE_NONE=0, _CHAT=1, _REPEATER=2, _ROOM=3, _SENSOR=4
  switch (type) {
    case 2: return LV_SYMBOL_WIFI;         // repeater
    case 3: return LV_SYMBOL_AUDIO;        // room
    case 4: return LV_SYMBOL_GPS;          // sensor (no better glyph in built-in set)
    default: return LV_SYMBOL_BELL;        // chat node
  }
}

static void formatLastSeen(char* out, size_t cap, uint32_t lastmod_secs, uint32_t now_secs) {
  if (lastmod_secs == 0) { snprintf(out, cap, "never"); return; }
  uint32_t age = now_secs >= lastmod_secs ? (now_secs - lastmod_secs) : 0;
  if (age < 60)         snprintf(out, cap, "%us",   (unsigned)age);
  else if (age < 3600)  snprintf(out, cap, "%um",   (unsigned)(age / 60));
  else if (age < 86400) snprintf(out, cap, "%uh",   (unsigned)(age / 3600));
  else                  snprintf(out, cap, "%ud",   (unsigned)(age / 86400));
}

static constexpr uint8_t CONTACT_FLAG_FAVOURITE = 0x01;  // matches firmware
static constexpr uint32_t FAV_HEX = 0xFBBF24;            // amber-400 accent

void UITask::addContactRow(const ContactInfo& c, uint32_t now_secs) {
  bool fav = (c.flags & CONTACT_FLAG_FAVOURITE) != 0;

  lv_obj_t* btn = lv_list_add_btn(_contacts_list, contactSymbol(c.type),
                                  c.name[0] ? c.name : "(unnamed)");
  lv_obj_set_style_bg_color(btn, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(fav ? FAV_HEX : FG_HEX), 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x374151), 0);  // gray-700
  lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(btn, 1, 0);

  // Tap -> open chat. Stash the 4-byte pubkey prefix in user_data (no
  // allocation, survives list rebuilds); resolve it back to a contact on tap.
  uint32_t key = 0;
  memcpy(&key, c.id.pub_key, 4);
  lv_obj_set_user_data(btn, (void*)(uintptr_t)key);
  lv_obj_add_event_cb(btn, contact_clicked_cb, LV_EVENT_CLICKED, NULL);

  char ago[16];
  formatLastSeen(ago, sizeof(ago), c.lastmod, now_secs);
  lv_obj_t* age = lv_label_create(btn);
  lv_label_set_text(age, ago);
  lv_obj_set_style_text_color(age, lv_color_hex(DIM_HEX), 0);
  lv_obj_align(age, LV_ALIGN_RIGHT_MID, -4, 0);
}

void UITask::rebuildContactsList() {
  if (!_contacts_list) return;
  _contacts_dirty = false;
  _contacts_rebuilt_ms = millis();
  lv_obj_clean(_contacts_list);

  uint32_t now_secs = the_mesh.getRTCClock()->getCurrentTime();
  int count = 0;

  // Pass 1: favourites first, always shown (user-curated, few).
  {
    ContactsIterator it;
    ContactInfo c;
    while (it.hasNext(&the_mesh, c)) {
      if (c.flags & CONTACT_FLAG_FAVOURITE) { addContactRow(c, now_secs); ++count; }
    }
  }

  // Pass 2: fill remaining slots with non-favourites, in storage order.
  // Widget allocs now come from PSRAM, so this cap is just a sanity bound
  // on rebuild time, not a memory limit. Favourites above are uncapped.
  const int MAX_RENDER = 200;
  {
    ContactsIterator it;
    ContactInfo c;
    while (it.hasNext(&the_mesh, c) && count < MAX_RENDER) {
      if (!(c.flags & CONTACT_FLAG_FAVOURITE)) { addContactRow(c, now_secs); ++count; }
    }
  }

  if (count == 0) lv_obj_clear_flag(_contacts_empty, LV_OBJ_FLAG_HIDDEN);
  else            lv_obj_add_flag(_contacts_empty,  LV_OBJ_FLAG_HIDDEN);
}

void UITask::channel_clicked_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* btn = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  ChannelDetails ch;
  if (the_mesh.getChannel(idx, ch) && ch.name[0]) _instance->openChat(ch.name);
}

void UITask::rebuildChannelsList() {
  if (!_channels_list) return;
  lv_obj_clean(_channels_list);

  // getChannel() returns true for any in-range slot incl. empty ones, so skip
  // slots with no name. The Public channel is added on every boot (MyMesh).
  for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
    ChannelDetails ch;
    if (!the_mesh.getChannel(idx, ch) || ch.name[0] == 0) continue;

    char label[40];
    snprintf(label, sizeof(label), "# %s", ch.name);
    lv_obj_t* btn = lv_list_add_btn(_channels_list, NULL, label);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x374151), 0);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_user_data(btn, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(btn, channel_clicked_cb, LV_EVENT_CLICKED, NULL);
  }
}

void UITask::splash_dismiss_cb(lv_timer_t* t) {
  UITask* self = static_cast<UITask*>(t->user_data);
  if (self->_home_screen) {
    lv_scr_load_anim(self->_home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
  }
  lv_timer_del(t);
}

// ---- Chat (conversation) screen ----------------------------------------

void UITask::contact_clicked_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* btn = lv_event_get_target(e);
  uint32_t key = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);
  uint8_t pfx[4];
  memcpy(pfx, &key, 4);
  ContactInfo* c = the_mesh.lookupContactByPubKey(pfx, 4);
  if (c) _instance->openChat(c->name);
}

void UITask::chat_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->_chat_peer[0] = 0;
  lv_scr_load(_instance->_home_screen);  // keep chat screen allocated for reuse
}

// Render message text into a bubble, turning "@[username]" mentions into
// "@username" with the name highlighted. Brackets hidden, @ kept. The stored
// text keeps the raw @[username] form so compose can round-trip it later.
static void addMessageText(lv_obj_t* bubble, const char* text) {
  static const uint32_t FG_TEXT = 0xF3F4F6;
  static const uint32_t MENTION = 0x34D399;  // emerald-400; reads on gray + blue bubbles

  lv_obj_t* sg = lv_spangroup_create(bubble);
  lv_obj_set_width(sg, LV_PCT(100));
  lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);  // wrap on width

  auto addSpan = [&](const char* s, size_t len, uint32_t color, bool mention) {
    if (len == 0) return;
    char buf[CHAT_MSG_TEXT_MAX + 2];
    size_t off = 0;
    if (mention) buf[off++] = '@';
    if (len > sizeof(buf) - 2 - off) len = sizeof(buf) - 2 - off;
    memcpy(buf + off, s, len);
    buf[off + len] = 0;
    lv_span_t* span = lv_spangroup_new_span(sg);
    lv_span_set_text(span, buf);
    lv_style_set_text_color(&span->style, lv_color_hex(color));
  };

  const char* p = text ? text : "";
  while (*p) {
    const char* at = strstr(p, "@[");
    if (!at) { addSpan(p, strlen(p), FG_TEXT, false); break; }
    addSpan(p, at - p, FG_TEXT, false);          // plain run before mention
    const char* close = strchr(at + 2, ']');
    if (!close) { addSpan(at, strlen(at), FG_TEXT, false); break; }  // malformed
    addSpan(at + 2, close - (at + 2), MENTION, true);  // "@" + username, highlighted
    p = close + 1;
  }
}

void UITask::rebuildChatHistory() {
  if (!_chat_history) return;
  lv_obj_clean(_chat_history);

  const ChatMessage* msgs[CHAT_HISTORY_CAP];
  int n = _msgs.messagesFor(_chat_peer, msgs, CHAT_HISTORY_CAP);

  if (n == 0) {
    lv_obj_t* empty = lv_label_create(_chat_history);
    lv_label_set_text(empty, "No messages yet.");
    lv_obj_set_style_text_color(empty, lv_color_hex(DIM_HEX), 0);
    lv_obj_center(empty);
    return;
  }

  char last_sender[CHAT_PEER_NAME_MAX] = "";
  bool last_outgoing = false;
  bool first = true;

  for (int i = 0; i < n; i++) {
    const ChatMessage* m = msgs[i];

    // Incoming bubbles get a sender-name header, but only at the start of a
    // run of consecutive messages from the same sender. Outgoing bubbles
    // (ours) never show a name.
    bool show_name = !m->outgoing &&
                     (first || last_outgoing ||
                      strncmp(last_sender, m->sender, CHAT_PEER_NAME_MAX) != 0);
    if (show_name) {
      lv_obj_t* name = lv_label_create(_chat_history);  // flex child, left by default
      lv_label_set_text(name, m->sender[0] ? m->sender : "?");
      lv_obj_set_style_text_color(name, lv_color_hex(0x9CA3AF), 0);  // gray-400
      lv_obj_set_style_text_font(name, &lv_font_montserrat_12, 0);
    }

    // Full-width row wrapper so we can left/right-align the bubble inside it
    // (flex columns can't per-item cross-align).
    lv_obj_t* row = lv_obj_create(_chat_history);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, LV_PCT(80));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    // sent = blue, right-aligned; received = gray-700, left-aligned
    lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? 0x2563EB : 0x374151), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_align(bubble, m->outgoing ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);

    addMessageText(bubble, m->text);

    strncpy(last_sender, m->sender, CHAT_PEER_NAME_MAX - 1);
    last_sender[CHAT_PEER_NAME_MAX - 1] = 0;
    last_outgoing = m->outgoing;
    first = false;
  }

  // Scroll to newest.
  lv_obj_scroll_to_y(_chat_history, LV_COORD_MAX, LV_ANIM_OFF);
}

void UITask::openChat(const char* peer_name) {
  strncpy(_chat_peer, peer_name ? peer_name : "", CHAT_PEER_NAME_MAX - 1);
  _chat_peer[CHAT_PEER_NAME_MAX - 1] = 0;

  // Build the screen skeleton once; reuse it across contacts.
  if (!_chat_screen) {
    _chat_screen = lv_obj_create(NULL);
    styleAsDarkScreen(_chat_screen);
    lv_obj_set_style_pad_all(_chat_screen, 0, 0);

    // Fixed top bar (back + title).
    lv_obj_t* bar = lv_obj_create(_chat_screen);
    lv_obj_set_size(bar, _screen_w, HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(back, chat_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(FG_HEX), 0);

    _chat_title = lv_label_create(bar);
    lv_obj_set_style_text_color(_chat_title, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_text_font(_chat_title, &lv_font_montserrat_20, 0);
    lv_obj_align(_chat_title, LV_ALIGN_LEFT_MID, 40, 0);

    // Scrollable history band (the future hardware-scroll VSA).
    _chat_history = lv_obj_create(_chat_screen);
    lv_obj_set_size(_chat_history, _screen_w, _screen_h - HEADER_H);
    lv_obj_align(_chat_history, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_color(_chat_history, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(_chat_history, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_history, 0, 0);
    lv_obj_set_style_radius(_chat_history, 0, 0);
    lv_obj_set_style_pad_all(_chat_history, 8, 0);
    lv_obj_set_style_pad_row(_chat_history, 6, 0);
    lv_obj_set_flex_flow(_chat_history, LV_FLEX_FLOW_COLUMN);
  }

  lv_label_set_text(_chat_title, _chat_peer);
  rebuildChatHistory();
  lv_scr_load(_chat_screen);
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _sensors = sensors;
  _node_prefs = node_prefs;
  _instance = this;

  // Every variant that uses this LVGL UITask must derive its DISPLAY_CLASS
  // from LGFXDisplay so we can reach the underlying LovyanGFX device.
  LGFXDisplay* lgfx_disp = static_cast<LGFXDisplay*>(display);
  if (!lgfx_disp) return;
  _lgfx = &lgfx_disp->getLgfx();

  // Re-init the panel. On some boards (e.g. CrowPanel Advance 3.5) the TFT
  // RST line is shared with another peripheral that may have pulsed it
  // between display.begin() and now. init() is safe to call again.
  // The variant chose its preferred rotation in DISPLAY_CLASS::begin();
  // preserve that across the re-init. Color depth must be 16-bit to match
  // LVGL's RGB565 framebuffer -- the LGFXDisplay base configures the panel
  // for 8-bit packed mode, which causes pixel interleaving under LVGL.
  uint_fast8_t saved_rotation = _lgfx->getRotation();
  _lgfx->init();
  _lgfx->setColorDepth(16);
  _lgfx->setRotation(saved_rotation);
  _lgfx->fillScreen(0x0000);

  _screen_w = _lgfx->width();
  _screen_h = _lgfx->height();

  lv_init();

  // Two DMA-capable internal buffers for double-buffered, pipelined flush.
  // Bigger buffers => fewer flush chunks => less per-transaction overhead.
  // Fall back to fewer lines if internal DMA RAM is tight.
  size_t buf_pixels = 0;
  for (uint16_t lines = kBufferLines; lines >= 24; lines -= 8) {
    size_t px = (size_t)_screen_w * lines;
    size_t bytes = px * sizeof(lv_color_t);
    _buf1 = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    _buf2 = (lv_color_t*)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (_buf1 && _buf2) { buf_pixels = px; break; }
    if (_buf1) { heap_caps_free(_buf1); _buf1 = NULL; }
    if (_buf2) { heap_caps_free(_buf2); _buf2 = NULL; }
  }
  lv_disp_draw_buf_init(&_draw_buf, _buf1, _buf2, buf_pixels);

  lv_disp_drv_init(&_disp_drv);
  _disp_drv.hor_res  = _screen_w;
  _disp_drv.ver_res  = _screen_h;
  _disp_drv.flush_cb = disp_flush_cb;
  _disp_drv.draw_buf = &_draw_buf;
  lv_disp_drv_register(&_disp_drv);

  lv_indev_drv_init(&_indev_drv);
  _indev_drv.type    = LV_INDEV_TYPE_POINTER;
  _indev_drv.read_cb = touchpad_read_cb;
  lv_indev_drv_register(&_indev_drv);

  _splash_screen = buildSplashScreen();
  _home_screen   = buildHomeScreen();
  rebuildContactsList();
  rebuildChannelsList();
  lv_scr_load(_splash_screen);

  // Auto-dismiss splash after a brief dwell. Single-shot via lv_timer_del.
  lv_timer_t* t = lv_timer_create(splash_dismiss_cb, 2500, this);
  lv_timer_set_repeat_count(t, 1);

  _started = true;
  _last_tick_ms = millis();
}

void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
}

// True if `name` matches a configured channel (vs a contact).
static bool nameIsChannel(const char* name) {
  if (!name) return false;
  for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
    ChannelDetails ch;
    if (the_mesh.getChannel(idx, ch) && ch.name[0] &&
        strncmp(ch.name, name, sizeof(ch.name)) == 0) return true;
  }
  return false;
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  (void)path_len;
  _msgcount = msgcount;

  // For channel messages the wire text is "<sender>: <msg>" and from_name is
  // the channel name. Split out the real sender. For DMs the sender is the
  // contact (from_name) and the text is already clean.
  const char* sender = from_name;
  const char* body   = text;
  char sender_buf[CHAT_PEER_NAME_MAX];
  if (text && nameIsChannel(from_name)) {
    const char* sep = strstr(text, ": ");
    if (sep) {
      size_t slen = sep - text;
      if (slen >= sizeof(sender_buf)) slen = sizeof(sender_buf) - 1;
      memcpy(sender_buf, text, slen);
      sender_buf[slen] = 0;
      sender = sender_buf;
      body   = sep + 2;
    }
  }

  _msgs.append(false, from_name, sender, body, the_mesh.getRTCClock()->getCurrentTime());

  // If this peer's chat is currently open, refresh it live.
  if (_chat_screen && from_name && strncmp(from_name, _chat_peer, CHAT_PEER_NAME_MAX) == 0) {
    rebuildChatHistory();
  }

  // Don't repaint the list on every packet -- mark dirty and let loop()
  // rebuild at most once every few seconds. Avoids repaint storms under
  // mesh traffic. (Selective per-row updates are the eventual fix.)
  _contacts_dirty = true;
}

void UITask::notify(UIEventType t) {
  (void)t;
}

void UITask::loop() {
  if (!_started) return;
  uint32_t now = millis();
  uint32_t delta = now - _last_tick_ms;
  if (delta > 0) {
    lv_tick_inc(delta);
    _last_tick_ms = now;
  }

  // Throttled contacts refresh (set dirty by newMsg).
  if (_contacts_dirty && now - _contacts_rebuilt_ms >= 3000) {
    rebuildContactsList();
  }

  lv_timer_handler();
}
