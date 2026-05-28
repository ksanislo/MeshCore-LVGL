#include "UITask.h"
#include "meshcore_assets.h"
#include "MyMesh.h"
#include <helpers/AdvertDataHelpers.h>  // ADV_TYPE_*
#include <Utils.h>                      // mesh::Utils::toHex
#include <esp_heap_caps.h>
#include <ctype.h>                      // tolower (case-insensitive search)

// main.cpp owns the_mesh; we read contacts/channels via the base class.
extern MyMesh the_mesh;

// Radio param setters live in the variant's target.cpp (same calls the BLE
// CMD_SET_RADIO_* handlers use). Forward-declared to avoid pulling target.h.
void radio_set_params(float freq, float bw, uint8_t sf, uint8_t cr);
void radio_set_tx_power(int8_t dbm);

// Backlight brightness hook. A variant that supports dimming provides a strong
// definition (e.g. CrowPanelBoard's LEDC writer); others fall back to this no-op.
extern "C" __attribute__((weak)) void board_set_backlight(uint8_t duty) { (void)duty; }

static void sanitizeForFont(const char* in, char* out, size_t cap);

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

static constexpr int HEADER_H     = 48;
static constexpr int TABBAR_H     = 56;
static constexpr int COMPOSE_H    = 50;
static constexpr int SEARCH_BAR_H = 44;
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

  // ----- Settings tab -----
  buildSettingsTab(scr);

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

  char cname[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(c.name[0] ? c.name : "(unnamed)", cname, sizeof(cname));
  lv_obj_t* btn = lv_list_add_btn(_contacts_list, contactSymbol(c.type), cname);
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

// Cheap change-detect over the contact set. Captures add/remove (count),
// favourite toggles + edits (flags), renames (name) and identity (pubkey).
// Deliberately excludes lastmod so routine RF adverts don't trigger rebuilds.
uint32_t UITask::contactsSignature() {
  uint32_t sig = 2166136261u;  // FNV-ish
  ContactsIterator it;
  ContactInfo c;
  int n = 0;
  while (it.hasNext(&the_mesh, c)) {
    n++;
    sig = (sig ^ c.flags) * 16777619u;
    for (int i = 0; i < 4; i++) sig = (sig ^ c.id.pub_key[i]) * 16777619u;
    for (const char* p = c.name; *p; p++) sig = (sig ^ (uint8_t)*p) * 16777619u;
  }
  return sig ^ (uint32_t)(n << 1);
}

void UITask::rebuildContactsList() {
  if (!_contacts_list) return;
  _contacts_dirty = false;
  _contacts_rebuilt_ms = millis();
  _contacts_sig = contactsSignature();  // keep poll baseline in sync
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
  if (the_mesh.getChannel(idx, ch) && ch.name[0]) {
    _instance->_chat_is_channel = true;
    _instance->_chat_channel_idx = idx;
    _instance->openChat(ch.name);
  }
}

void UITask::rebuildChannelsList() {
  if (!_channels_list) return;
  lv_obj_clean(_channels_list);

  // getChannel() returns true for any in-range slot incl. empty ones, so skip
  // slots with no name. The Public channel is added on every boot (MyMesh).
  for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
    ChannelDetails ch;
    if (!the_mesh.getChannel(idx, ch) || ch.name[0] == 0) continue;

    char cname[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(ch.name, cname, sizeof(cname));
    char label[48];
    snprintf(label, sizeof(label), "# %s", cname);
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
  if (c) {
    _instance->_chat_is_channel = false;
    memcpy(_instance->_chat_pubkey, c->id.pub_key, 6);  // 6-byte prefix for sendMessage
    _instance->openChat(c->name);
  }
}

void UITask::chat_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->layoutChatBody(false);  // hide keyboard
  _instance->_chat_peer[0] = 0;
  lv_scr_load(_instance->_home_screen);  // keep chat screen allocated for reuse
}

void UITask::chat_input_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) _instance->layoutChatBody(true);
}

void UITask::chat_send_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->sendCurrentMessage();
}

void UITask::chat_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (_instance->_search_active) {
    // While searching the keyboard drives the search field, not compose: just
    // dismiss it on done/close and keep the filtered results showing.
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) _instance->layoutChatBody(false);
    return;
  }
  if (code == LV_EVENT_READY)       _instance->sendCurrentMessage();   // checkmark key
  else if (code == LV_EVENT_CANCEL) _instance->layoutChatBody(false);  // close key
}

void UITask::chat_search_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_chat_keyboard, ta);
    lv_keyboard_set_mode(_instance->_chat_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    _instance->layoutChatBody(true);
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    const char* s = lv_textarea_get_text(ta);
    strncpy(_instance->_search_filter, s ? s : "", sizeof(_instance->_search_filter) - 1);
    _instance->_search_filter[sizeof(_instance->_search_filter) - 1] = 0;
    _instance->rebuildChatHistory();
  }
}

void UITask::chat_search_close_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->_search_active = false;
  _instance->_search_filter[0] = 0;
  if (_instance->_chat_search_ta) lv_textarea_set_text(_instance->_chat_search_ta, "");
  lv_keyboard_set_textarea(_instance->_chat_keyboard, _instance->_chat_input);  // restore compose
  _instance->layoutChatBody(false);  // hide keyboard + search bar
  _instance->rebuildChatHistory();
}

void UITask::layoutChatBody(bool keyboard_shown) {
  if (!_chat_history || !_chat_compose) return;

  // Only re-pin to the newest message if we were already at the bottom;
  // otherwise leave the scroll where it is so reading history isn't disrupted.
  bool was_at_bottom = lv_obj_get_scroll_bottom(_chat_history) <= 4;

  // The keyboard sizes itself from its key matrix and bottom-aligns, so don't
  // assume a height -- show it, let it lay out, then read its actual top edge
  // and stack the compose band + history above that.
  int avail_bottom = _screen_h;
  if (_chat_keyboard) {
    if (keyboard_shown) {
      lv_obj_clear_flag(_chat_keyboard, LV_OBJ_FLAG_HIDDEN);
      lv_obj_align(_chat_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);  // re-pin each show
      lv_obj_update_layout(_chat_keyboard);
      // Derive the free area from the keyboard's height (its y can be stale
      // right after un-hiding, before the parent's layout pass re-applies it).
      avail_bottom = _screen_h - lv_obj_get_height(_chat_keyboard);
    } else {
      lv_obj_add_flag(_chat_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // The search bar (when active) sits between the top bar and the history.
  int top = HEADER_H;
  if (_chat_search_bar) {
    if (_search_active) {
      lv_obj_set_size(_chat_search_bar, _screen_w, SEARCH_BAR_H);
      lv_obj_align(_chat_search_bar, LV_ALIGN_TOP_MID, 0, HEADER_H);
      lv_obj_clear_flag(_chat_search_bar, LV_OBJ_FLAG_HIDDEN);
      top = HEADER_H + SEARCH_BAR_H;
    } else {
      lv_obj_add_flag(_chat_search_bar, LV_OBJ_FLAG_HIDDEN);
    }
  }

  int compose_y = avail_bottom - COMPOSE_H;
  lv_obj_set_size(_chat_compose, _screen_w, COMPOSE_H);
  lv_obj_align(_chat_compose, LV_ALIGN_TOP_MID, 0, compose_y);

  lv_obj_set_size(_chat_history, _screen_w, compose_y - top);
  lv_obj_align(_chat_history, LV_ALIGN_TOP_MID, 0, top);

  lv_obj_update_layout(_chat_history);
  if (was_at_bottom) lv_obj_scroll_to_y(_chat_history, LV_COORD_MAX, LV_ANIM_OFF);
}

// ---- Insert (+) menu: share contact refs into the compose field ----------

void UITask::ensureInsertPopup() {
  if (_insert_popup) return;
  _insert_popup = lv_obj_create(_chat_screen);
  lv_obj_set_size(_insert_popup, _screen_w, _screen_h);
  lv_obj_set_pos(_insert_popup, 0, 0);
  lv_obj_set_style_bg_color(_insert_popup, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(_insert_popup, LV_OPA_50, 0);
  lv_obj_set_style_border_width(_insert_popup, 0, 0);
  lv_obj_set_style_radius(_insert_popup, 0, 0);
  lv_obj_set_style_pad_all(_insert_popup, 0, 0);
  lv_obj_add_flag(_insert_popup, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(_insert_popup, insert_backdrop_cb, LV_EVENT_CLICKED, NULL);

  _insert_list = lv_list_create(_insert_popup);
  lv_obj_set_width(_insert_list, LV_PCT(82));
  lv_obj_set_height(_insert_list, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(_insert_list, LV_PCT(72), 0);
  lv_obj_center(_insert_list);
  lv_obj_set_style_bg_color(_insert_list, lv_color_hex(0x1F2937), 0);
  lv_obj_set_style_border_width(_insert_list, 0, 0);

  lv_obj_add_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

static void styleMenuBtn(lv_obj_t* b) {
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1F2937), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(0xF3F4F6), 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x374151), 0);
  lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(b, 1, 0);
}

void UITask::showInsertMenu() {
  ensureInsertPopup();
  lv_obj_clean(_insert_list);
  lv_obj_t* b1 = lv_list_add_btn(_insert_list, LV_SYMBOL_HOME, "My Contact Info");
  styleMenuBtn(b1);
  lv_obj_add_event_cb(b1, insert_myinfo_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* b2 = lv_list_add_btn(_insert_list, LV_SYMBOL_UPLOAD, "Share Contact");
  styleMenuBtn(b2);
  lv_obj_add_event_cb(b2, insert_share_cb, LV_EVENT_CLICKED, NULL);
  if (_clipboard[0]) {  // only when something has been copied
    lv_obj_t* b3 = lv_list_add_btn(_insert_list, LV_SYMBOL_PASTE, "Paste");
    styleMenuBtn(b3);
    lv_obj_add_event_cb(b3, insert_paste_cb, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_clear_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

// Fill `list` with contact buttons (favourites first), each wired to `cb` with
// the 4-byte pubkey prefix in user_data. Capped so a large contact list can't
// exhaust the LVGL heap (allocating a widget per contact across ~350 OOMs).
void UITask::buildContactPicker(lv_obj_t* list, lv_event_cb_t cb, bool styled) {
  const int MAX_PICK = 100;
  int n = 0;
  for (int fav_pass = 1; fav_pass >= 0 && n < MAX_PICK; fav_pass--) {
    ContactsIterator it;
    ContactInfo c;
    while (it.hasNext(&the_mesh, c) && n < MAX_PICK) {
      bool fav = (c.flags & CONTACT_FLAG_FAVOURITE) != 0;
      if (fav != (fav_pass == 1)) continue;  // favourites pass first
      char nm[CHAT_PEER_NAME_MAX + 4];
      sanitizeForFont(c.name[0] ? c.name : "(unnamed)", nm, sizeof(nm));
      lv_obj_t* b = lv_list_add_btn(list, contactSymbol(c.type), nm);
      if (styled) styleMenuBtn(b);
      uint32_t key = 0;
      memcpy(&key, c.id.pub_key, 4);
      lv_obj_set_user_data(b, (void*)(uintptr_t)key);
      lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
      n++;
    }
  }
}

void UITask::showContactPicker() {
  ensureInsertPopup();
  lv_obj_clean(_insert_list);
  buildContactPicker(_insert_list, picker_pick_cb, true);
  lv_obj_clear_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::closeInsertPopup() {
  if (_insert_popup) lv_obj_add_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::insertContactRef(const uint8_t* pubkey, uint8_t type, const char* name) {
  // App-recognized shared-contact format: <pubkey_hex:type_id:name>
  // (pubkey = 64 hex chars; type per ADV_TYPE_*). The app renders a message
  // consisting only of this token as a "shared contact" card.
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  char ref[2 * PUB_KEY_SIZE + 48];
  snprintf(ref, sizeof(ref), "<%s:%u:%s>", hex, (unsigned)type, name ? name : "");
  if (_chat_input) lv_textarea_add_text(_chat_input, ref);
}

void UITask::chat_plus_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->showInsertMenu();
}

void UITask::insert_backdrop_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->closeInsertPopup();
}

void UITask::insert_myinfo_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  const char* nm = (_instance->_node_prefs && _instance->_node_prefs->node_name[0])
                       ? _instance->_node_prefs->node_name : "Me";
  _instance->insertContactRef(the_mesh.self_id.pub_key, ADV_TYPE_CHAT, nm);
  _instance->closeInsertPopup();
}

void UITask::insert_share_cb(lv_event_t* e) {
  (void)e;
  // showContactPicker cleans _insert_list (deleting this button); defer it.
  lv_async_call([](void*) { if (_instance) _instance->showContactPicker(); }, NULL);
}

void UITask::picker_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* btn = lv_event_get_target(e);
  uint32_t key = (uint32_t)(uintptr_t)lv_obj_get_user_data(btn);
  uint8_t pfx[4];
  memcpy(pfx, &key, 4);
  ContactInfo* c = the_mesh.lookupContactByPubKey(pfx, 4);
  if (c) _instance->insertContactRef(c->id.pub_key, c->type, c->name);
  _instance->closeInsertPopup();
}

// Rewrite "@Name" -> "@[Name]" when Name matches a known contact (longest
// match, ending at a word boundary), so other clients render it as a mention.
// Non-matching "@text" and already-bracketed "@[Name]" pass through unchanged.
static void encodeMentions(const char* in, char* out, size_t cap) {
  auto isBoundary = [](char a) {
    return a == 0 || a == ' ' || a == '\n' || a == '\t' || a == ',' || a == '.' ||
           a == '!' || a == '?' || a == ';' || a == ':' || a == ')' || a == '\'';
  };
  size_t o = 0;
  const char* p = in;
  auto emit = [&](const char* s, size_t n) { while (n-- && o + 1 < cap) out[o++] = *s++; };

  while (*p && o + 1 < cap) {
    if (*p == '@' && p[1] != '[') {
      const char* nm = p + 1;
      size_t bestlen = 0;
      ContactsIterator it;
      ContactInfo c;
      while (it.hasNext(&the_mesh, c)) {
        size_t nl = strlen(c.name);
        if (nl > bestlen && strncmp(nm, c.name, nl) == 0 && isBoundary(nm[nl])) bestlen = nl;
      }
      if (bestlen > 0) {
        emit("@[", 2);
        emit(nm, bestlen);
        if (o + 1 < cap) out[o++] = ']';
        p = nm + bestlen;
        continue;
      }
    }
    out[o++] = *p++;
  }
  out[o] = 0;
}

void UITask::sendCurrentMessage() {
  if (!_chat_input) return;
  const char* raw = lv_textarea_get_text(_chat_input);
  if (!raw || !raw[0]) return;

  char encoded[CHAT_MSG_TEXT_MAX + 32];
  encodeMentions(raw, encoded, sizeof(encoded));
  const char* text = encoded;

  const char* me = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "Me";
  uint32_t now = the_mesh.getRTCClock()->getCurrentTimeUnique();
  bool sent = false;

  if (_chat_is_channel) {
    ChannelDetails ch;
    if (the_mesh.getChannel(_chat_channel_idx, ch)) {
      sent = the_mesh.sendGroupMessage(now, ch.channel, me, text, strlen(text));
    }
  } else {
    ContactInfo* c = the_mesh.lookupContactByPubKey(_chat_pubkey, 6);
    if (c) {
      uint32_t ack = 0, timeout = 0;
      sent = the_mesh.sendMessage(*c, now, 0, text, ack, timeout) != MSG_SEND_FAILED;
    }
  }

  if (sent) {
    _msgs.append(true, _chat_peer, me, text, now);
    lv_textarea_set_text(_chat_input, "");
    rebuildChatHistory();
  }
}

// Map common "smart"/typographic UTF-8 punctuation to ASCII so it renders in
// the Montserrat glyph set (which lacks U+2018/19/1C/1D, dashes, ellipsis,
// nbsp). Everything else passes through byte-for-byte, so glyphs the font does
// have still show. Output is always null-terminated.
static void sanitizeForFont(const char* in, char* out, size_t cap) {
  const uint8_t* p = (const uint8_t*)(in ? in : "");
  size_t o = 0;
  auto emit = [&](const char* s) { while (*s && o + 1 < cap) out[o++] = *s++; };
  while (*p && o + 1 < cap) {
    if (p[0] == 0xE2 && p[1] == 0x80) {              // U+2000..U+203F block
      const char* rep = nullptr;
      switch (p[2]) {
        case 0x98: case 0x99: rep = "'";   break;    // ‘ ’
        case 0x9C: case 0x9D: rep = "\"";  break;    // “ ”
        case 0x93: case 0x94: rep = "-";   break;    // – —
        case 0xA6:            rep = "...";  break;   // …
        default: break;
      }
      if (rep) { emit(rep); p += 3; continue; }
    } else if (p[0] == 0xC2 && p[1] == 0xA0) {       // nbsp
      out[o++] = ' '; p += 2; continue;
    }
    out[o++] = *p++;  // copy byte (multi-byte UTF-8 passes through intact)
  }
  out[o] = 0;
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

  char clean[CHAT_MSG_TEXT_MAX + 8];
  sanitizeForFont(text, clean, sizeof(clean));

  const char* p = clean;
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

// Resolved @mention targets, stashed on a bubble's user_data so a tap can open
// the contact(s) without re-parsing. Freed on the bubble's LV_EVENT_DELETE.
static const int MENTION_MAX = 6;
struct MentionTargets {
  int     count;
  uint8_t keys[MENTION_MAX][PUB_KEY_SIZE];
  char    names[MENTION_MAX][CHAT_PEER_NAME_MAX];
};

void UITask::attachMentions(lv_obj_t* bubble, const char* text) {
  MentionTargets t;
  t.count = 0;
  const char* p = text;
  while (*p && t.count < MENTION_MAX) {
    const char* at = strstr(p, "@[");
    if (!at) break;
    const char* close = strchr(at + 2, ']');
    if (!close) break;
    int nlen = (int)(close - (at + 2));
    if (nlen > 0 && nlen < (int)CHAT_PEER_NAME_MAX) {
      char nm[CHAT_PEER_NAME_MAX];
      memcpy(nm, at + 2, nlen);
      nm[nlen] = 0;
      ContactsIterator it;
      ContactInfo c;
      while (it.hasNext(&the_mesh, c)) {
        if ((int)strlen(c.name) == nlen && strncmp(c.name, nm, nlen) == 0) {
          bool dup = false;
          for (int i = 0; i < t.count; i++)
            if (memcmp(t.keys[i], c.id.pub_key, PUB_KEY_SIZE) == 0) { dup = true; break; }
          if (!dup) {
            memcpy(t.keys[t.count], c.id.pub_key, PUB_KEY_SIZE);
            strncpy(t.names[t.count], c.name, CHAT_PEER_NAME_MAX - 1);
            t.names[t.count][CHAT_PEER_NAME_MAX - 1] = 0;
            t.count++;
          }
          break;
        }
      }
    }
    p = close + 1;
  }
  if (t.count == 0) return;
  MentionTargets* heap = (MentionTargets*)lv_mem_alloc(sizeof(MentionTargets));
  if (!heap) return;
  *heap = t;
  lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(bubble, heap);
  lv_obj_add_event_cb(bubble, mention_bubble_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(bubble, mention_free_cb, LV_EVENT_DELETE, NULL);
}

void UITask::mention_free_cb(lv_event_t* e) {
  MentionTargets* t = (MentionTargets*)lv_obj_get_user_data(lv_event_get_target(e));
  if (t) lv_mem_free(t);
}

void UITask::mention_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  uint32_t key = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  uint8_t pfx[4];
  memcpy(pfx, &key, 4);
  ContactInfo* c = the_mesh.lookupContactByPubKey(pfx, 4);
  _instance->closeMenuPopup();
  if (c) _instance->openContactInfo(c->id.pub_key, _instance->_chat_screen);
}

void UITask::mention_bubble_cb(lv_event_t* e) {
  if (!_instance) return;
  MentionTargets* t = (MentionTargets*)lv_obj_get_user_data(lv_event_get_target(e));
  if (!t || t->count == 0) return;
  if (t->count == 1) {
    _instance->openContactInfo(t->keys[0], _instance->_chat_screen);
    return;
  }
  lv_obj_t* list = _instance->ensureMenuPopup();
  for (int i = 0; i < t->count; i++) {
    char nm[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(t->names[i], nm, sizeof(nm));
    lv_obj_t* b = lv_list_add_btn(list, LV_SYMBOL_BELL, nm);
    uint32_t key = 0;
    memcpy(&key, t->keys[i], 4);
    lv_obj_set_user_data(b, (void*)(uintptr_t)key);
    lv_obj_add_event_cb(b, mention_pick_cb, LV_EVENT_CLICKED, NULL);
  }
  _instance->showMenuPopup();
}

// Case-insensitive substring test (ASCII), for in-conversation search.
static bool containsCI(const char* hay, const char* needle) {
  if (!needle || !needle[0]) return true;
  size_t nl = strlen(needle);
  for (const char* h = hay; *h; h++) {
    size_t i = 0;
    while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == nl) return true;
  }
  return false;
}

// Parse a shared-contact token "<pubkey_hex:type:name>" (whole message body).
// Requires a full 64-hex-char pubkey. Returns false if not such a token.
static bool parseContactRef(const char* text, uint8_t* pk_out, uint8_t& type_out,
                            char* name_out, size_t name_cap) {
  if (!text || text[0] != '<') return false;
  size_t len = strlen(text);
  if (len < 4 || text[len - 1] != '>') return false;
  const char* c1 = strchr(text + 1, ':');
  if (!c1) return false;
  const char* c2 = strchr(c1 + 1, ':');
  if (!c2) return false;
  if (c1 - (text + 1) != 2 * PUB_KEY_SIZE) return false;  // need full pubkey
  int t = 0;
  bool any = false;
  for (const char* p = c1 + 1; p < c2; p++) {
    if (*p < '0' || *p > '9') return false;
    t = t * 10 + (*p - '0');
    any = true;
  }
  if (!any || t > 255) return false;
  const char* nend = text + len - 1;  // the '>'
  if (nend <= c2 + 1) return false;    // empty name
  char hexbuf[2 * PUB_KEY_SIZE + 1];
  memcpy(hexbuf, text + 1, 2 * PUB_KEY_SIZE);
  hexbuf[2 * PUB_KEY_SIZE] = 0;
  if (!mesh::Utils::fromHex(pk_out, PUB_KEY_SIZE, hexbuf)) return false;
  type_out = (uint8_t)t;
  size_t nl = nend - (c2 + 1);
  if (nl > name_cap - 1) nl = name_cap - 1;
  memcpy(name_out, c2 + 1, nl);
  name_out[nl] = 0;
  return true;
}

void UITask::buildContactCard(lv_obj_t* bubble, const ChatMessage* m,
                              const uint8_t* pubkey, uint8_t type, const char* name) {
  lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(bubble, 3, 0);

  if (m->outgoing) {
    lv_obj_t* hdr = lv_label_create(bubble);
    lv_label_set_text(hdr, "You shared this contact");
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_12, 0);
  }

  char sname[36];
  sanitizeForFont(name, sname, sizeof(sname));
  lv_obj_t* nm = lv_label_create(bubble);
  lv_label_set_text(nm, sname);
  lv_obj_set_style_text_color(nm, lv_color_hex(0xF3F4F6), 0);

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  char keytrunc[24];
  snprintf(keytrunc, sizeof(keytrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_obj_t* kl = lv_label_create(bubble);
  lv_label_set_text(kl, keytrunc);
  lv_obj_set_style_text_color(kl, lv_color_hex(0x9CA3AF), 0);
  lv_obj_set_style_text_font(kl, &lv_font_montserrat_12, 0);

  // Tap a card for a known contact (either direction) -> open Contact Info.
  if (the_mesh.lookupContactByPubKey(pubkey, PUB_KEY_SIZE)) {
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(bubble, (void*)m);
    lv_obj_add_event_cb(bubble, [](lv_event_t* ev) {
      if (!_instance) return;
      const ChatMessage* mm = (const ChatMessage*)lv_obj_get_user_data(lv_event_get_target(ev));
      uint8_t pk[PUB_KEY_SIZE], ty; char nm[CHAT_PEER_NAME_MAX];
      if (mm && parseContactRef(mm->text, pk, ty, nm, sizeof(nm)))
        _instance->openContactInfo(pk, _instance->_chat_screen);
    }, LV_EVENT_CLICKED, NULL);
  }

  if (!m->outgoing) {
    if (the_mesh.lookupContactByPubKey(pubkey, PUB_KEY_SIZE)) {
      lv_obj_t* in = lv_label_create(bubble);
      lv_label_set_text(in, LV_SYMBOL_OK " In contacts (tap to open)");
      lv_obj_set_style_text_color(in, lv_color_hex(0x34D399), 0);
      lv_obj_set_style_text_font(in, &lv_font_montserrat_12, 0);
    } else {
      lv_obj_t* add = lv_btn_create(bubble);
      lv_obj_set_user_data(add, (void*)m);  // re-parsed on tap
      lv_obj_add_event_cb(add, add_contact_cb, LV_EVENT_CLICKED, NULL);
      lv_obj_t* al = lv_label_create(add);
      lv_label_set_text(al, LV_SYMBOL_PLUS " Add Contact");
      lv_obj_center(al);
    }
  }
}

void UITask::add_contact_cb(lv_event_t* e) {
  if (!_instance) return;
  const ChatMessage* m = (const ChatMessage*)lv_obj_get_user_data(lv_event_get_target(e));
  if (!m) return;
  uint8_t pk[PUB_KEY_SIZE], type;
  char name[CHAT_PEER_NAME_MAX];
  if (!parseContactRef(m->text, pk, type, name, sizeof(name))) return;

  ContactInfo c;
  memset(&c, 0, sizeof(c));
  memcpy(c.id.pub_key, pk, PUB_KEY_SIZE);
  c.type = type;
  strncpy(c.name, name, sizeof(c.name) - 1);
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = the_mesh.getRTCClock()->getCurrentTime();
  the_mesh.addContact(c);
  _instance->rebuildChatHistory();  // re-render: Add button -> "In contacts"
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
  bool filtering = _search_active && _search_filter[0];
  int shown = 0;

  for (int i = 0; i < n; i++) {
    const ChatMessage* m = msgs[i];
    if (filtering && !containsCI(m->text, _search_filter)) continue;
    shown++;

    // Incoming bubbles get a sender-name header, but only at the start of a
    // run of consecutive messages from the same sender. Outgoing bubbles
    // (ours) never show a name.
    bool show_name = !m->outgoing &&
                     (first || last_outgoing ||
                      strncmp(last_sender, m->sender, CHAT_PEER_NAME_MAX) != 0);
    if (show_name) {
      lv_obj_t* name = lv_label_create(_chat_history);  // flex child, left by default
      char sname[CHAT_PEER_NAME_MAX + 4];
      sanitizeForFont(m->sender[0] ? m->sender : "?", sname, sizeof(sname));
      lv_label_set_text(name, sname);
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

    uint8_t cpk[PUB_KEY_SIZE], ctype;
    char cname[CHAT_PEER_NAME_MAX];
    if (parseContactRef(m->text, cpk, ctype, cname, sizeof(cname))) {
      buildContactCard(bubble, m, cpk, ctype, cname);
    } else {
      addMessageText(bubble, m->text);
      attachMentions(bubble, m->text);  // tappable if it @-mentions a known contact
    }

    strncpy(last_sender, m->sender, CHAT_PEER_NAME_MAX - 1);
    last_sender[CHAT_PEER_NAME_MAX - 1] = 0;
    last_outgoing = m->outgoing;
    first = false;
  }

  if (filtering && shown == 0) {
    lv_obj_t* none = lv_label_create(_chat_history);
    lv_label_set_text(none, "No matches.");
    lv_obj_set_style_text_color(none, lv_color_hex(DIM_HEX), 0);
    lv_obj_center(none);
    return;
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
    // Tap the contact name to open Contact Info (no-op for channels).
    lv_obj_add_flag(_chat_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_chat_title, cinfo_name_clicked_cb, LV_EVENT_CLICKED, NULL);

    // Overflow (kebab) menu, right-aligned.
    lv_obj_t* kebab = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(kebab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(kebab, 0, 0);
    lv_obj_align(kebab, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(kebab, chat_kebab_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* kl = lv_label_create(kebab);
    lv_label_set_text(kl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(kl, lv_color_hex(FG_HEX), 0);

    // In-conversation search bar: hidden until the kebab "Search" reveals it.
    // Geometry (position/height) is applied by layoutChatBody when active.
    _chat_search_bar = lv_obj_create(_chat_screen);
    lv_obj_set_style_bg_color(_chat_search_bar, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(_chat_search_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_search_bar, 0, 0);
    lv_obj_set_style_radius(_chat_search_bar, 0, 0);
    lv_obj_set_style_pad_all(_chat_search_bar, 6, 0);
    lv_obj_set_style_pad_column(_chat_search_bar, 6, 0);
    lv_obj_clear_flag(_chat_search_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_chat_search_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_chat_search_bar, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(_chat_search_bar, LV_OBJ_FLAG_HIDDEN);

    _chat_search_ta = lv_textarea_create(_chat_search_bar);
    lv_textarea_set_one_line(_chat_search_ta, true);
    lv_textarea_set_placeholder_text(_chat_search_ta, "Search messages");
    lv_obj_set_flex_grow(_chat_search_ta, 1);
    lv_obj_add_event_cb(_chat_search_ta, chat_search_ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t* sclose = lv_btn_create(_chat_search_bar);
    lv_obj_add_event_cb(sclose, chat_search_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* scl = lv_label_create(sclose);
    lv_label_set_text(scl, LV_SYMBOL_CLOSE);
    lv_obj_center(scl);

    // Scrollable history band (the future hardware-scroll VSA). Geometry set
    // by layoutChatBody() so it adjusts when the keyboard shows/hides.
    _chat_history = lv_obj_create(_chat_screen);
    lv_obj_set_style_bg_color(_chat_history, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(_chat_history, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_history, 0, 0);
    lv_obj_set_style_radius(_chat_history, 0, 0);
    lv_obj_set_style_pad_all(_chat_history, 8, 0);
    lv_obj_set_style_pad_row(_chat_history, 6, 0);
    lv_obj_set_flex_flow(_chat_history, LV_FLEX_FLOW_COLUMN);

    // Fixed compose band: textarea (grows) + send button.
    _chat_compose = lv_obj_create(_chat_screen);
    lv_obj_set_style_bg_color(_chat_compose, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(_chat_compose, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_compose, 0, 0);
    lv_obj_set_style_radius(_chat_compose, 0, 0);
    lv_obj_set_style_pad_all(_chat_compose, 5, 0);
    lv_obj_set_style_pad_column(_chat_compose, 5, 0);
    lv_obj_clear_flag(_chat_compose, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_chat_compose, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_chat_compose, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Insert (+) button: circular, opens a menu to insert contact refs.
    lv_obj_t* plus = lv_btn_create(_chat_compose);
    lv_obj_set_size(plus, COMPOSE_H - 14, COMPOSE_H - 14);
    lv_obj_set_style_radius(plus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(plus, 0, 0);
    lv_obj_add_event_cb(plus, chat_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* plus_lbl = lv_label_create(plus);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(plus_lbl);

    _chat_input = lv_textarea_create(_chat_compose);
    lv_textarea_set_one_line(_chat_input, true);
    lv_textarea_set_placeholder_text(_chat_input, "Message");
    lv_obj_set_flex_grow(_chat_input, 1);
    lv_obj_add_event_cb(_chat_input, chat_input_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t* send = lv_btn_create(_chat_compose);
    lv_obj_add_event_cb(send, chat_send_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* send_lbl = lv_label_create(send);
    lv_label_set_text(send_lbl, LV_SYMBOL_OK);
    lv_obj_center(send_lbl);

    // On-screen keyboard, hidden until the input is focused.
    _chat_keyboard = lv_keyboard_create(_chat_screen);
    lv_keyboard_set_textarea(_chat_keyboard, _chat_input);
    lv_obj_add_event_cb(_chat_keyboard, chat_kb_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(_chat_keyboard, LV_OBJ_FLAG_HIDDEN);
  }

  // Reset any search state from a previously open conversation.
  _search_active = false;
  _search_filter[0] = 0;
  if (_chat_search_ta) lv_textarea_set_text(_chat_search_ta, "");
  if (_chat_keyboard && _chat_input) lv_keyboard_set_textarea(_chat_keyboard, _chat_input);

  lv_textarea_set_text(_chat_input, "");
  layoutChatBody(false);  // keyboard hidden on (re)open

  char tname[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(_chat_peer, tname, sizeof(tname));
  lv_label_set_text(_chat_title, tname);
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
  // The variant chose a default rotation in DISPLAY_CLASS::begin(); a user-set
  // rotation in prefs (1-based; 0 = unset) overrides it and survives reboots.
  uint_fast8_t saved_rotation = _lgfx->getRotation();
  if (_node_prefs && _node_prefs->display_rotation)
    saved_rotation = (_node_prefs->display_rotation - 1) & 0x03;
  _lgfx->init();
  _lgfx->setColorDepth(16);
  _lgfx->setRotation(saved_rotation);
  _lgfx->fillScreen(0x0000);

  // Apply a persisted backlight level (0 = keep the board's boot default).
  if (_node_prefs && _node_prefs->display_brightness)
    board_set_backlight(_node_prefs->display_brightness);

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

void UITask::sentMsg(const char* peer, const char* text) {
  const char* me = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "Me";
  _msgs.append(true, peer, me, text, the_mesh.getRTCClock()->getCurrentTime());

  if (_chat_screen && peer && strncmp(peer, _chat_peer, CHAT_PEER_NAME_MAX) == 0) {
    rebuildChatHistory();
  }
}

void UITask::notify(UIEventType t) {
  (void)t;
}

// ===== Contact Info page =================================================

ContactInfo* UITask::cinfoContact() {
  return the_mesh.lookupContactByPubKey(_cinfo_pubkey, PUB_KEY_SIZE);
}

void UITask::cinfo_toast_timer_cb(lv_timer_t* t) {
  if (_instance && _instance->_toast)
    lv_obj_add_flag(_instance->_toast, LV_OBJ_FLAG_HIDDEN);
  lv_timer_del(t);
}

void UITask::showToast(const char* text) {
  if (!_toast) {  // lazily build on the top layer so it shows from any screen
    _toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(_toast, lv_color_hex(0x374151), 0);
    lv_obj_set_style_bg_opa(_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(_toast, lv_color_hex(0xF3F4F6), 0);
    lv_obj_set_style_pad_all(_toast, 8, 0);
    lv_obj_set_style_radius(_toast, 6, 0);
    lv_obj_align(_toast, LV_ALIGN_BOTTOM_MID, 0, -10);
  }
  lv_label_set_text(_toast, text);
  lv_obj_clear_flag(_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_toast);
  lv_timer_t* t = lv_timer_create(cinfo_toast_timer_cb, 1500, NULL);
  lv_timer_set_repeat_count(t, 1);
}

// caption label + a column container the caller fills with the value widget
static lv_obj_t* makeField(lv_obj_t* parent, const char* caption) {
  lv_obj_t* col = lv_obj_create(parent);
  lv_obj_set_width(col, LV_PCT(100));
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* cap = lv_label_create(col);
  lv_label_set_text(cap, caption);
  lv_obj_set_style_text_color(cap, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
  return col;
}

void UITask::buildContactInfoScreen() {
  if (_cinfo_screen) return;
  _cinfo_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_cinfo_screen);
  lv_obj_set_style_pad_all(_cinfo_screen, 0, 0);

  // fixed top bar
  lv_obj_t* bar = lv_obj_create(_cinfo_screen);
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
  lv_obj_add_event_cb(back, cinfo_back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(FG_HEX), 0);
  lv_obj_t* bt = lv_label_create(bar);
  lv_label_set_text(bt, "Contact");
  lv_obj_set_style_text_color(bt, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(bt, &lv_font_montserrat_20, 0);
  lv_obj_align(bt, LV_ALIGN_LEFT_MID, 40, 0);

  // scrollable body
  _cinfo_body = lv_obj_create(_cinfo_screen);
  lv_obj_set_size(_cinfo_body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(_cinfo_body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(_cinfo_body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_cinfo_body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_cinfo_body, 0, 0);
  lv_obj_set_style_radius(_cinfo_body, 0, 0);
  lv_obj_set_style_pad_all(_cinfo_body, 12, 0);
  lv_obj_set_style_pad_row(_cinfo_body, 8, 0);
  lv_obj_set_flex_flow(_cinfo_body, LV_FLEX_FLOW_COLUMN);

  _cinfo_title = lv_label_create(_cinfo_body);
  lv_obj_set_style_text_color(_cinfo_title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_title, &lv_font_montserrat_28, 0);

  _cinfo_key = lv_label_create(_cinfo_body);
  lv_obj_set_style_text_color(_cinfo_key, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_key, &lv_font_montserrat_12, 0);

  // action row
  lv_obj_t* actions = lv_obj_create(_cinfo_body);
  lv_obj_set_width(actions, LV_PCT(100));
  lv_obj_set_height(actions, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(actions, 0, 0);
  lv_obj_set_style_pad_all(actions, 0, 0);
  lv_obj_set_style_pad_column(actions, 6, 0);
  lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);

  lv_obj_t* fav = lv_btn_create(actions);
  lv_obj_add_event_cb(fav, cinfo_fav_cb, LV_EVENT_CLICKED, NULL);
  _cinfo_fav_lbl = lv_label_create(fav);
  lv_label_set_text(_cinfo_fav_lbl, LV_SYMBOL_OK " Fav");
  lv_obj_t* tele = lv_btn_create(actions);
  lv_obj_add_event_cb(tele, cinfo_telemetry_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* tl = lv_label_create(tele);
  lv_label_set_text(tl, LV_SYMBOL_DOWNLOAD " Telem");
  lv_obj_t* shr = lv_btn_create(actions);
  lv_obj_add_event_cb(shr, cinfo_share_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* sl = lv_label_create(shr);
  lv_label_set_text(sl, LV_SYMBOL_UPLOAD " Share");

  // edit fields
  lv_obj_t* fn = makeField(_cinfo_body, "Name");
  _cinfo_name_ta = lv_textarea_create(fn);
  lv_textarea_set_one_line(_cinfo_name_ta, true);
  lv_obj_set_width(_cinfo_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_cinfo_name_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t* fk = makeField(_cinfo_body, "Public Key");
  _cinfo_keyfull = lv_label_create(fk);
  lv_label_set_long_mode(_cinfo_keyfull, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_cinfo_keyfull, LV_PCT(100));
  lv_obj_set_style_text_color(_cinfo_keyfull, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_keyfull, &lv_font_montserrat_12, 0);

  lv_obj_t* fp = makeField(_cinfo_body, "Position (lat, lon)");
  lv_obj_t* prow = lv_obj_create(fp);
  lv_obj_set_width(prow, LV_PCT(100));
  lv_obj_set_height(prow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(prow, 0, 0);
  lv_obj_set_style_pad_all(prow, 0, 0);
  lv_obj_set_style_pad_column(prow, 6, 0);
  lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
  _cinfo_lat_ta = lv_textarea_create(prow);
  lv_textarea_set_one_line(_cinfo_lat_ta, true);
  lv_obj_set_flex_grow(_cinfo_lat_ta, 1);
  lv_obj_add_event_cb(_cinfo_lat_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);
  _cinfo_lon_ta = lv_textarea_create(prow);
  lv_textarea_set_one_line(_cinfo_lon_ta, true);
  lv_obj_set_flex_grow(_cinfo_lon_ta, 1);
  lv_obj_add_event_cb(_cinfo_lon_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t* ft = makeField(_cinfo_body, "Contact Type");
  _cinfo_type_dd = lv_dropdown_create(ft);
  lv_dropdown_set_options(_cinfo_type_dd, "Chat\nRepeater\nRoom\nSensor");
  lv_obj_set_width(_cinfo_type_dd, LV_PCT(100));
  lv_obj_add_event_cb(_cinfo_type_dd, cinfo_type_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* flh = makeField(_cinfo_body, "Last Heard");
  _cinfo_lastheard = lv_label_create(flh);
  lv_obj_set_style_text_color(_cinfo_lastheard, lv_color_hex(FG_HEX), 0);

  // path: hops row
  lv_obj_t* hrow = lv_obj_create(_cinfo_body);
  lv_obj_set_width(hrow, LV_PCT(100));
  lv_obj_set_height(hrow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(hrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(hrow, 0, 0);
  lv_obj_set_style_pad_all(hrow, 0, 0);
  lv_obj_clear_flag(hrow, LV_OBJ_FLAG_SCROLLABLE);
  _cinfo_hops = lv_label_create(hrow);
  lv_obj_set_style_text_color(_cinfo_hops, lv_color_hex(FG_HEX), 0);
  lv_obj_align(_cinfo_hops, LV_ALIGN_LEFT_MID, 0, 0);
  _cinfo_hops_x = lv_btn_create(hrow);
  lv_obj_align(_cinfo_hops_x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(_cinfo_hops_x, cinfo_clearpath_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* xl = lv_label_create(_cinfo_hops_x);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);

  // path: out path row
  lv_obj_t* orow = lv_obj_create(_cinfo_body);
  lv_obj_set_width(orow, LV_PCT(100));
  lv_obj_set_height(orow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(orow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(orow, 0, 0);
  lv_obj_set_style_pad_all(orow, 0, 0);
  lv_obj_clear_flag(orow, LV_OBJ_FLAG_SCROLLABLE);
  _cinfo_outpath = lv_label_create(orow);
  lv_label_set_long_mode(_cinfo_outpath, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_cinfo_outpath, LV_PCT(80));
  lv_obj_set_style_text_color(_cinfo_outpath, lv_color_hex(FG_HEX), 0);
  lv_obj_align(_cinfo_outpath, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t* ep = lv_btn_create(orow);
  lv_obj_align(ep, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(ep, cinfo_editpath_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* epl = lv_label_create(ep);
  lv_label_set_text(epl, LV_SYMBOL_EDIT);

  // keyboard (hidden) + toast
  _cinfo_kb = lv_keyboard_create(_cinfo_screen);
  lv_obj_add_event_cb(_cinfo_kb, cinfo_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::populateContactInfo() {
  ContactInfo* c = cinfoContact();
  if (!c) { lv_scr_load(_cinfo_return_screen ? _cinfo_return_screen : _home_screen); return; }

  char nm[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(c->name[0] ? c->name : "(unnamed)", nm, sizeof(nm));
  lv_label_set_text(_cinfo_title, nm);
  lv_textarea_set_text(_cinfo_name_ta, c->name);

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
  char ktrunc[24];
  snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_label_set_text(_cinfo_key, ktrunc);
  lv_label_set_text(_cinfo_keyfull, hex);

  bool fav = (c->flags & CONTACT_FLAG_FAVOURITE) != 0;
  lv_label_set_text(_cinfo_fav_lbl, fav ? LV_SYMBOL_OK " Favorited" : "Favorite");
  lv_obj_set_style_text_color(_cinfo_fav_lbl, lv_color_hex(fav ? FAV_HEX : 0xF3F4F6), 0);

  char latbuf[20] = "", lonbuf[20] = "";
  if (c->gps_lat || c->gps_lon) {
    snprintf(latbuf, sizeof(latbuf), "%.6f", c->gps_lat / 1e6);
    snprintf(lonbuf, sizeof(lonbuf), "%.6f", c->gps_lon / 1e6);
  }
  lv_textarea_set_text(_cinfo_lat_ta, latbuf);
  lv_textarea_set_text(_cinfo_lon_ta, lonbuf);

  uint16_t ti = (c->type >= 1 && c->type <= 4) ? c->type - 1 : 0;
  lv_dropdown_set_selected(_cinfo_type_dd, ti);

  char lh[16];
  formatLastSeen(lh, sizeof(lh), c->last_advert_timestamp, the_mesh.getRTCClock()->getCurrentTime());
  lv_label_set_text(_cinfo_lastheard, lh);

  int hashSize = the_mesh.getNodePrefs()->path_hash_mode + 1;
  if (hashSize < 1) hashSize = 1;
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    lv_label_set_text(_cinfo_hops, "Hops Away: flood (unknown)");
    lv_obj_add_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_cinfo_outpath, "Out Path: (flood)");
  } else {
    char hb[32];
    snprintf(hb, sizeof(hb), "Hops Away: %d", c->out_path_len / hashSize);
    lv_label_set_text(_cinfo_hops, hb);
    lv_obj_clear_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    char op[3 * MAX_PATH_SIZE + 16];
    int n = snprintf(op, sizeof(op), "Out Path: ");
    for (int i = 0; i < c->out_path_len && n < (int)sizeof(op) - 3; i++)
      n += snprintf(op + n, sizeof(op) - n, "%02x%s", c->out_path[i], i + 1 < c->out_path_len ? "," : "");
    lv_label_set_text(_cinfo_outpath, op);
  }
}

void UITask::openContactInfo(const uint8_t* pubkey, lv_obj_t* return_screen) {
  memcpy(_cinfo_pubkey, pubkey, PUB_KEY_SIZE);
  _cinfo_return_screen = return_screen;
  buildContactInfoScreen();
  _cinfo_active_ta = NULL;
  lv_obj_add_flag(_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  populateContactInfo();
  lv_obj_scroll_to_y(_cinfo_body, 0, LV_ANIM_OFF);
  lv_scr_load(_cinfo_screen);
}

void UITask::commitCinfoField(lv_obj_t* ta) {
  ContactInfo* c = cinfoContact();
  if (!c || !ta) return;
  if (ta == _cinfo_name_ta) {
    strncpy(c->name, lv_textarea_get_text(ta), sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = 0;
    the_mesh.saveContact(*c);
    char nm[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(c->name[0] ? c->name : "(unnamed)", nm, sizeof(nm));
    lv_label_set_text(_cinfo_title, nm);
  } else if (ta == _cinfo_lat_ta || ta == _cinfo_lon_ta) {
    const char* lats = lv_textarea_get_text(_cinfo_lat_ta);
    const char* lons = lv_textarea_get_text(_cinfo_lon_ta);
    if (lats[0] == 0 && lons[0] == 0) { c->gps_lat = 0; c->gps_lon = 0; }
    else { c->gps_lat = (int32_t)lround(atof(lats) * 1e6); c->gps_lon = (int32_t)lround(atof(lons) * 1e6); }
    the_mesh.saveContact(*c);
  }
}

void UITask::cinfo_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_cinfo_active_ta) { _instance->commitCinfoField(_instance->_cinfo_active_ta); _instance->_cinfo_active_ta = NULL; }
  lv_obj_add_flag(_instance->_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(_instance->_cinfo_return_screen ? _instance->_cinfo_return_screen : _instance->_home_screen);
}

void UITask::cinfo_fav_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  c->flags ^= CONTACT_FLAG_FAVOURITE;
  the_mesh.saveContact(*c);
  _instance->populateContactInfo();
}

void UITask::cinfo_type_cb(lv_event_t* e) {
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  c->type = (uint8_t)(lv_dropdown_get_selected(lv_event_get_target(e)) + 1);
  the_mesh.saveContact(*c);
}

void UITask::cinfo_telemetry_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  uint32_t tag = 0, est = 0;
  the_mesh.sendRequest(*c, 0x03 /*REQ_TYPE_GET_TELEMETRY_DATA*/, tag, est);
  _instance->showToast("Telemetry requested");
}

void UITask::cinfo_clearpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  the_mesh.resetPathTo(*c);
  the_mesh.saveContact(*c);
  _instance->populateContactInfo();
}

void UITask::cinfo_editpath_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->openPathEditor(_instance->_cinfo_screen);
}

// keyboard show/hide for the info-page textareas
void UITask::cinfo_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    _instance->_cinfo_active_ta = ta;
    lv_keyboard_set_textarea(_instance->_cinfo_kb, ta);
    bool num = (ta == _instance->_cinfo_lat_ta || ta == _instance->_cinfo_lon_ta);
    lv_keyboard_set_mode(_instance->_cinfo_kb, num ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_cinfo_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_cinfo_kb);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitCinfoField(ta);
  }
}

void UITask::cinfo_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (_instance->_cinfo_active_ta) {
      _instance->commitCinfoField(_instance->_cinfo_active_ta);
      _instance->_cinfo_active_ta = NULL;
    }
    lv_obj_add_flag(_instance->_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

void UITask::cinfo_name_clicked_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || _instance->_chat_is_channel) return;
  ContactInfo* c = the_mesh.lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (c) _instance->openContactInfo(c->id.pub_key, _instance->_chat_screen);
}

// ----- Share sub-menu (popup parented to the cinfo screen) -----
// Shared top-layer popup (kebab menu / share menu / pickers).
lv_obj_t* UITask::ensureMenuPopup() {
  if (!_menu_popup) {
    _menu_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_menu_popup, _screen_w, _screen_h);
    lv_obj_set_pos(_menu_popup, 0, 0);
    lv_obj_set_style_bg_color(_menu_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_menu_popup, LV_OPA_50, 0);
    lv_obj_set_style_border_width(_menu_popup, 0, 0);
    lv_obj_set_style_pad_all(_menu_popup, 0, 0);
    lv_obj_add_flag(_menu_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_menu_popup, [](lv_event_t* ev) {
      (void)ev; if (_instance) _instance->closeMenuPopup();
    }, LV_EVENT_CLICKED, NULL);
    _menu_list = lv_list_create(_menu_popup);
    lv_obj_set_width(_menu_list, LV_PCT(82));
    lv_obj_set_height(_menu_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(_menu_list, LV_PCT(72), 0);
    lv_obj_center(_menu_list);
    lv_obj_set_style_bg_color(_menu_list, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(_menu_list, 0, 0);
  }
  lv_obj_clean(_menu_list);
  return _menu_list;
}

void UITask::showMenuPopup() {
  if (_menu_popup) { lv_obj_clear_flag(_menu_popup, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(_menu_popup); }
}

void UITask::closeMenuPopup() {
  if (_menu_popup) lv_obj_add_flag(_menu_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::showShareMenu() {
  lv_obj_t* list = ensureMenuPopup();
  lv_obj_t* b1 = lv_list_add_btn(list, LV_SYMBOL_UPLOAD, "Send to a contact");
  lv_obj_add_event_cb(b1, share_sendto_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* b2 = lv_list_add_btn(list, LV_SYMBOL_WIFI, "Broadcast (zero-hop)");
  lv_obj_add_event_cb(b2, share_zerohop_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* b3 = lv_list_add_btn(list, LV_SYMBOL_IMAGE, "Show QR code");
  lv_obj_add_event_cb(b3, share_showqr_cb, LV_EVENT_CLICKED, NULL);
  showMenuPopup();
}

void UITask::cinfo_share_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->showShareMenu();
}

void UITask::share_sendto_cb(lv_event_t* e) {
  (void)e;
  // Defer: this rebuilds _menu_list (deleting the button whose event we're in)
  // and could allocate many widgets -- both unsafe inside the click event.
  lv_async_call([](void*) {
    if (!_instance) return;
    lv_obj_t* list = _instance->ensureMenuPopup();
    _instance->buildContactPicker(list, share_pick_cb, false);
    _instance->showMenuPopup();
  }, NULL);
}

void UITask::share_zerohop_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (c) the_mesh.shareContactZeroHop(*c);
  _instance->closeMenuPopup();
  _instance->showToast("Shared (zero-hop)");
}

void UITask::share_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  UITask* s = _instance;
  ContactInfo* viewed = s->cinfoContact();
  uint32_t key = (uint32_t)(uintptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  uint8_t pfx[4]; memcpy(pfx, &key, 4);
  ContactInfo* recip = the_mesh.lookupContactByPubKey(pfx, 4);
  if (viewed && recip) {
    char hex[2 * PUB_KEY_SIZE + 1];
    mesh::Utils::toHex(hex, viewed->id.pub_key, PUB_KEY_SIZE);
    char ref[2 * PUB_KEY_SIZE + 48];
    snprintf(ref, sizeof(ref), "<%s:%u:%s>", hex, (unsigned)viewed->type, viewed->name);
    uint32_t now = the_mesh.getRTCClock()->getCurrentTimeUnique();
    uint32_t ack = 0, to = 0;
    if (the_mesh.sendMessage(*recip, now, 0, ref, ack, to) != MSG_SEND_FAILED) {
      const char* me = (s->_node_prefs && s->_node_prefs->node_name[0]) ? s->_node_prefs->node_name : "Me";
      s->_msgs.append(true, recip->name, me, ref, now);
    }
  }
  s->closeMenuPopup();
  s->showToast("Contact shared");
}

// ----- Share as QR code -----------------------------------------------------

// Percent-encode a contact name for the meshcore:// URI (RFC 3986 unreserved
// pass through; space -> '+'; everything else -> %XX).
static void urlEncode(const char* in, char* out, size_t cap) {
  static const char* hexd = "0123456789ABCDEF";
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)(in ? in : ""); *p && o + 1 < cap; p++) {
    unsigned char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out[o++] = c;
    } else if (c == ' ') {
      out[o++] = '+';
    } else if (o + 3 < cap) {
      out[o++] = '%'; out[o++] = hexd[c >> 4]; out[o++] = hexd[c & 0xF];
    } else break;
  }
  out[o] = 0;
}

void UITask::buildShareQRScreen() {
  if (_qr_screen) return;
  _qr_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_qr_screen);
  lv_obj_set_style_pad_all(_qr_screen, 0, 0);

  // fixed top bar (back + title)
  lv_obj_t* bar = lv_obj_create(_qr_screen);
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
  lv_obj_add_event_cb(back, qr_back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(FG_HEX), 0);
  lv_obj_t* bt = lv_label_create(bar);
  lv_label_set_text(bt, "Share QR");
  lv_obj_set_style_text_color(bt, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(bt, &lv_font_montserrat_20, 0);
  lv_obj_align(bt, LV_ALIGN_LEFT_MID, 40, 0);

  // body: name + truncated key (top band), QR centered below
  lv_obj_t* body = lv_obj_create(_qr_screen);
  lv_obj_set_size(body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_radius(body, 0, 0);
  lv_obj_set_style_pad_all(body, 12, 0);
  lv_obj_set_style_pad_row(body, 10, 0);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  _qr_name_lbl = lv_label_create(body);
  lv_obj_set_style_text_color(_qr_name_lbl, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_qr_name_lbl, &lv_font_montserrat_28, 0);

  _qr_key_lbl = lv_label_create(body);
  lv_obj_set_style_text_color(_qr_key_lbl, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(_qr_key_lbl, &lv_font_montserrat_12, 0);

  // White-framed QR so scanners get the required light quiet-zone.
  lv_obj_t* frame = lv_obj_create(body);
  lv_obj_set_style_bg_color(frame, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(frame, 0, 0);
  lv_obj_set_style_radius(frame, 6, 0);
  lv_obj_set_style_pad_all(frame, 10, 0);
  lv_obj_set_size(frame, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

  int qsize = (_screen_w < _screen_h ? _screen_w : _screen_h) - 96;
  if (qsize < 120) qsize = 120;
  _qr_code = lv_qrcode_create(frame, qsize, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
}

void UITask::openShareQR(const uint8_t* pubkey, uint8_t type, const char* name, lv_obj_t* return_screen) {
  buildShareQRScreen();
  _qr_return_screen = return_screen ? return_screen : _home_screen;

  char sname[36];
  sanitizeForFont(name && name[0] ? name : "(unnamed)", sname, sizeof(sname));
  lv_label_set_text(_qr_name_lbl, sname);

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  char ktrunc[24];
  snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_label_set_text(_qr_key_lbl, ktrunc);

  // App-recognized contact import URI (see docs/qr_codes.md).
  char ename[3 * 32 + 1];
  urlEncode(name, ename, sizeof(ename));
  char uri[96 + 2 * PUB_KEY_SIZE + sizeof(ename)];
  snprintf(uri, sizeof(uri), "meshcore://contact/add?name=%s&public_key=%s&type=%u",
           ename, hex, (unsigned)type);
  lv_qrcode_update(_qr_code, uri, strlen(uri));

  lv_scr_load(_qr_screen);
}

void UITask::qr_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  lv_scr_load(_instance->_qr_return_screen ? _instance->_qr_return_screen : _instance->_home_screen);
}

void UITask::share_showqr_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();  // target is _cinfo_pubkey
  lv_obj_t* ret = lv_scr_act();
  _instance->closeMenuPopup();
  if (c) _instance->openShareQR(c->id.pub_key, c->type, c->name, ret);
}

// ----- Kebab overflow menu (chat top bar) -----
void UITask::chat_kebab_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  UITask* s = _instance;
  lv_obj_t* list = s->ensureMenuPopup();
  if (!s->_chat_is_channel) {
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_LIST, "Details"), kebab_details_cb, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_EYE_OPEN, "Search"), kebab_search_cb, LV_EVENT_CLICKED, NULL);
  if (!s->_chat_is_channel) {
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_UPLOAD, "Share"), kebab_share_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_EDIT, "Set Path"), kebab_setpath_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_CLOSE, "Reset Path"), kebab_resetpath_cb, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_TRASH, "Clear History"), kebab_clearhistory_cb, LV_EVENT_CLICKED, NULL);
  s->showMenuPopup();
}

void UITask::kebab_details_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  ContactInfo* c = the_mesh.lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (c) _instance->openContactInfo(c->id.pub_key, _instance->_chat_screen);
}

void UITask::kebab_search_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  if (!_instance->_chat_screen) return;
  _instance->_search_active = true;
  _instance->_search_filter[0] = 0;
  if (_instance->_chat_search_ta) lv_textarea_set_text(_instance->_chat_search_ta, "");
  lv_keyboard_set_textarea(_instance->_chat_keyboard, _instance->_chat_search_ta);
  lv_keyboard_set_mode(_instance->_chat_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
  _instance->layoutChatBody(true);   // reveal search bar + keyboard
  _instance->rebuildChatHistory();
}

void UITask::kebab_share_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = the_mesh.lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (!c) { _instance->closeMenuPopup(); return; }
  memcpy(_instance->_cinfo_pubkey, c->id.pub_key, PUB_KEY_SIZE);
  _instance->closeMenuPopup();
  // showShareMenu rebuilds _menu_list (deleting this kebab button); defer it.
  lv_async_call([](void*) { if (_instance) _instance->showShareMenu(); }, NULL);
}

void UITask::kebab_setpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = the_mesh.lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (!c) { _instance->closeMenuPopup(); return; }
  memcpy(_instance->_cinfo_pubkey, c->id.pub_key, PUB_KEY_SIZE);
  _instance->closeMenuPopup();
  _instance->openPathEditor(_instance->_chat_screen);
}

void UITask::kebab_resetpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  ContactInfo* c = the_mesh.lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (!c) return;
  the_mesh.resetPathTo(*c);
  the_mesh.saveContact(*c);
  _instance->showToast("Path reset to flood");
}

void UITask::kebab_clearhistory_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  _instance->_msgs.clearPeer(_instance->_chat_peer);
  _instance->rebuildChatHistory();
  _instance->showToast("History cleared");
}

// ===== Path Editor sub-page ==============================================

static int parseHexCsv(const char* in, uint8_t* out, int cap) {
  int n = 0;
  const char* p = in;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    const char* tok = p;
    while (*p && *p != ',' && *p != ' ') p++;
    int toklen = (int)(p - tok);
    if (toklen == 0) continue;
    if ((toklen % 2) || toklen / 2 > 3) return -1;  // 1-3 bytes per hop
    if (n + toklen / 2 > cap) return -1;
    char hb[8];
    memcpy(hb, tok, toklen); hb[toklen] = 0;
    if (!mesh::Utils::fromHex(out + n, toklen / 2, hb)) return -1;
    n += toklen / 2;
  }
  return n;
}

void UITask::buildPathEditorScreen() {
  if (_path_screen) return;
  _path_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_path_screen);
  lv_obj_set_style_pad_all(_path_screen, 0, 0);

  lv_obj_t* bar = lv_obj_create(_path_screen);
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
  lv_obj_add_event_cb(back, path_back_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(FG_HEX), 0);
  lv_obj_t* bt = lv_label_create(bar);
  lv_label_set_text(bt, "Out Path");
  lv_obj_set_style_text_color(bt, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(bt, &lv_font_montserrat_20, 0);
  lv_obj_align(bt, LV_ALIGN_LEFT_MID, 40, 0);
  lv_obj_t* save = lv_btn_create(bar);
  lv_obj_align(save, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(save, path_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* svl = lv_label_create(save);
  lv_label_set_text(svl, LV_SYMBOL_OK);

  lv_obj_t* body = lv_obj_create(_path_screen);
  lv_obj_set_size(body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 12, 0);
  lv_obj_set_style_pad_row(body, 8, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* fs = makeField(body, "Hash size (bytes per hop)");
  _path_size_dd = lv_dropdown_create(fs);
  lv_dropdown_set_options(_path_size_dd, "1 byte\n2 bytes\n3 bytes");
  lv_obj_set_width(_path_size_dd, LV_PCT(100));

  lv_obj_t* fp = makeField(body, "Path (comma hex, e.g. aa,bb,cc)");
  _path_ta = lv_textarea_create(fp);
  lv_textarea_set_one_line(_path_ta, true);
  lv_obj_set_width(_path_ta, LV_PCT(100));
  lv_obj_add_event_cb(_path_ta, path_ta_event_cb, LV_EVENT_ALL, NULL);

  _path_err = lv_label_create(body);
  lv_label_set_text(_path_err, "");
  lv_obj_set_style_text_color(_path_err, lv_color_hex(0xF87171), 0);
  lv_obj_add_flag(_path_err, LV_OBJ_FLAG_HIDDEN);

  _path_kb = lv_keyboard_create(_path_screen);
  lv_obj_add_event_cb(_path_kb, path_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_path_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::populatePathEditor() {
  lv_dropdown_set_selected(_path_size_dd, the_mesh.getNodePrefs()->path_hash_mode);
  lv_obj_add_flag(_path_err, LV_OBJ_FLAG_HIDDEN);
  ContactInfo* c = cinfoContact();
  char buf[3 * MAX_PATH_SIZE + 1] = "";
  if (c && c->out_path_len != OUT_PATH_UNKNOWN) {
    int n = 0;
    for (int i = 0; i < c->out_path_len && n < (int)sizeof(buf) - 3; i++)
      n += snprintf(buf + n, sizeof(buf) - n, "%02x%s", c->out_path[i], i + 1 < c->out_path_len ? "," : "");
  }
  lv_textarea_set_text(_path_ta, buf);
}

bool UITask::savePathEditor() {
  ContactInfo* c = cinfoContact();
  if (!c) return false;
  const char* txt = lv_textarea_get_text(_path_ta);
  bool empty = true;
  for (const char* p = txt; *p; p++) if (*p != ' ' && *p != ',') { empty = false; break; }
  if (empty) { the_mesh.resetPathTo(*c); the_mesh.saveContact(*c); return true; }
  uint8_t buf[MAX_PATH_SIZE];
  int n = parseHexCsv(txt, buf, MAX_PATH_SIZE);
  if (n < 0) { lv_label_set_text(_path_err, "Bad path (use aa,bb,cc)"); lv_obj_clear_flag(_path_err, LV_OBJ_FLAG_HIDDEN); return false; }
  memcpy(c->out_path, buf, n);
  c->out_path_len = (uint8_t)n;
  the_mesh.saveContact(*c);
  return true;
}

void UITask::openPathEditor(lv_obj_t* return_screen) {
  buildPathEditorScreen();
  _path_return_screen = return_screen ? return_screen : _cinfo_screen;
  lv_obj_add_flag(_path_kb, LV_OBJ_FLAG_HIDDEN);
  populatePathEditor();
  lv_scr_load(_path_screen);
}

void UITask::path_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  lv_obj_add_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(_instance->_path_return_screen ? _instance->_path_return_screen : _instance->_cinfo_screen);
}

void UITask::path_save_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->savePathEditor()) {
    lv_obj_add_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* ret = _instance->_path_return_screen ? _instance->_path_return_screen : _instance->_cinfo_screen;
    lv_scr_load(ret);
    if (ret == _instance->_cinfo_screen) _instance->populateContactInfo();
  }
}

void UITask::path_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_path_kb, lv_event_get_target(e));
    lv_obj_clear_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_path_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_path_kb);
    lv_obj_scroll_to_view(lv_event_get_target(e), LV_ANIM_OFF);
  }
}

void UITask::path_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    lv_obj_add_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
}

// ===== Settings tab ======================================================

// A section heading (accent label with an underline divider) in the settings list.
static void addSettingsSection(lv_obj_t* body, const char* title) {
  lv_obj_t* h = lv_label_create(body);
  lv_label_set_text(h, title);
  lv_obj_set_width(h, LV_PCT(100));
  lv_obj_set_style_text_color(h, lv_color_hex(0x60A5FA), 0);  // blue-400 accent
  lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
  lv_obj_set_style_pad_top(h, 8, 0);
  lv_obj_set_style_pad_bottom(h, 4, 0);
  lv_obj_set_style_border_color(h, lv_color_hex(0x374151), 0);  // gray-700 underline
  lv_obj_set_style_border_side(h, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(h, 1, 0);
}

// Official MeshCore region presets, embedded from https://api.meshcore.nz/api/v1/config
// (suggested_radio_settings). Refresh from that endpoint if the list changes upstream.
struct RadioPreset { const char* title; float freq; float bw; uint8_t sf; uint8_t cr; };
static const RadioPreset RADIO_PRESETS[] = {
  {"Australia",               915.800f, 250.0f, 10, 5},
  {"Australia (Narrow)",      916.575f,  62.5f,  7, 8},
  {"Australia (Mid)",         915.075f, 125.0f,  9, 5},
  {"Australia: SA, WA",       923.125f,  62.5f,  8, 8},
  {"Australia: QLD",          923.125f,  62.5f,  8, 5},
  {"EU/UK (Narrow)",          869.618f,  62.5f,  8, 8},
  {"EU/UK (Deprecated)",      869.525f, 250.0f, 11, 5},
  {"Czech Republic (Narrow)", 869.432f,  62.5f,  7, 5},
  {"EU 433MHz (Long Range)",  433.650f, 250.0f, 11, 5},
  {"EU 433MHz (Narrow)",      433.650f,  62.5f,  8, 8},
  {"New Zealand",             917.375f, 250.0f, 11, 5},
  {"New Zealand (Narrow)",    917.375f,  62.5f,  7, 5},
  {"Portugal 433",            433.375f,  62.5f,  9, 6},
  {"Portugal 868",            869.618f,  62.5f,  7, 6},
  {"Switzerland",             869.618f,  62.5f,  8, 8},
  {"USA/Canada (Recommended)",910.525f,  62.5f,  7, 5},
  {"Vietnam (Narrow)",        920.250f,  62.5f,  8, 5},
  {"Vietnam (Deprecated)",    920.250f, 250.0f, 11, 5},
};
static const int RADIO_PRESETS_N = sizeof(RADIO_PRESETS) / sizeof(RADIO_PRESETS[0]);

// Valid LoRa bandwidths (kHz); index maps 1:1 to the bandwidth dropdown options.
static const float BW_OPTS[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};
static const char* BW_OPTS_STR = "7.8\n10.4\n15.6\n20.8\n31.25\n41.7\n62.5\n125\n250\n500";
static const int BW_OPTS_N = sizeof(BW_OPTS) / sizeof(BW_OPTS[0]);

// caption + a 100%-wide dropdown with the given newline-separated options.
static lv_obj_t* makeDropdownField(lv_obj_t* body, const char* cap, const char* opts) {
  lv_obj_t* f = makeField(body, cap);
  lv_obj_t* dd = lv_dropdown_create(f);
  lv_dropdown_set_options(dd, opts);
  lv_obj_set_width(dd, LV_PCT(100));
  return dd;
}

// caption + a 100%-wide numeric textarea wired to the radio-edit handler.
static lv_obj_t* makeNumberField(lv_obj_t* body, const char* cap, lv_event_cb_t cb) {
  lv_obj_t* f = makeField(body, cap);
  lv_obj_t* ta = lv_textarea_create(f);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_accepted_chars(ta, "0123456789.-");
  lv_obj_set_width(ta, LV_PCT(100));
  lv_obj_add_event_cb(ta, cb, LV_EVENT_ALL, NULL);
  return ta;
}

void UITask::buildSettingsTab(lv_obj_t* parent) {
  lv_obj_t* body = _tab_settings;
  lv_obj_set_style_pad_all(body, 12, 0);
  lv_obj_set_style_pad_row(body, 8, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);  // tab page scrolls by default

  // ===== Public Info =====
  addSettingsSection(body, "Public Info");

  lv_obj_t* fn = makeField(body, "Name");
  _set_name_ta = lv_textarea_create(fn);
  lv_textarea_set_one_line(_set_name_ta, true);
  lv_obj_set_width(_set_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_set_name_ta, set_name_ta_event_cb, LV_EVENT_ALL, NULL);

  // Public key: read-only one-line field (scrolls horizontally) + copy button.
  lv_obj_t* fk = makeField(body, "Public Key");
  lv_obj_t* krow = lv_obj_create(fk);
  lv_obj_set_width(krow, LV_PCT(100));
  lv_obj_set_height(krow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(krow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(krow, 0, 0);
  lv_obj_set_style_pad_all(krow, 0, 0);
  lv_obj_set_style_pad_column(krow, 6, 0);
  lv_obj_clear_flag(krow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(krow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(krow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  _set_key_ta = lv_textarea_create(krow);
  lv_textarea_set_one_line(_set_key_ta, true);   // no wrap; swipe to scroll the hex
  lv_obj_set_flex_grow(_set_key_ta, 1);
  lv_obj_set_style_text_font(_set_key_ta, &lv_font_montserrat_12, 0);
  // No keyboard is ever bound to this field, so it's effectively read-only.
  lv_obj_t* copyk = lv_btn_create(krow);
  lv_obj_add_event_cb(copyk, set_copykey_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ck = lv_label_create(copyk);
  lv_label_set_text(ck, LV_SYMBOL_COPY);
  lv_obj_center(ck);

  // Position: editable lat/lon (degrees). Affects adverts only when sharing is on.
  lv_obj_t* fpos = makeField(body, "Position (lat, lon)");
  lv_obj_t* prow = lv_obj_create(fpos);
  lv_obj_set_width(prow, LV_PCT(100));
  lv_obj_set_height(prow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(prow, 0, 0);
  lv_obj_set_style_pad_all(prow, 0, 0);
  lv_obj_set_style_pad_column(prow, 6, 0);
  lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
  _set_lat_ta = lv_textarea_create(prow);
  lv_textarea_set_one_line(_set_lat_ta, true);
  lv_textarea_set_accepted_chars(_set_lat_ta, "0123456789.-");
  lv_obj_set_flex_grow(_set_lat_ta, 1);
  lv_obj_add_event_cb(_set_lat_ta, set_pos_ta_event_cb, LV_EVENT_ALL, NULL);
  _set_lon_ta = lv_textarea_create(prow);
  lv_textarea_set_one_line(_set_lon_ta, true);
  lv_textarea_set_accepted_chars(_set_lon_ta, "0123456789.-");
  lv_obj_set_flex_grow(_set_lon_ta, 1);
  lv_obj_add_event_cb(_set_lon_ta, set_pos_ta_event_cb, LV_EVENT_ALL, NULL);

  _set_sharepos = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_sharepos, "Share position in adverts");
  lv_obj_set_style_text_color(_set_sharepos, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_sharepos, set_sharepos_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ===== Radio (edit fields, then a single Apply) =====
  // Header row: "Radio" title on the left, a "Preset" button on the right.
  lv_obj_t* rhdr = lv_obj_create(body);
  lv_obj_set_width(rhdr, LV_PCT(100));
  lv_obj_set_height(rhdr, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(rhdr, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(rhdr, 0, 0);
  lv_obj_set_style_pad_all(rhdr, 0, 0);
  lv_obj_set_style_pad_top(rhdr, 8, 0);
  lv_obj_set_style_pad_bottom(rhdr, 4, 0);
  lv_obj_set_style_border_color(rhdr, lv_color_hex(0x374151), 0);
  lv_obj_set_style_border_side(rhdr, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(rhdr, 1, 0);
  lv_obj_clear_flag(rhdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(rhdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rhdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* rtitle = lv_label_create(rhdr);
  lv_label_set_text(rtitle, "Radio");
  lv_obj_set_style_text_color(rtitle, lv_color_hex(0x60A5FA), 0);
  lv_obj_set_style_text_font(rtitle, &lv_font_montserrat_16, 0);
  lv_obj_t* preset = lv_btn_create(rhdr);
  lv_obj_set_style_pad_hor(preset, 10, 0);
  lv_obj_set_style_pad_ver(preset, 4, 0);
  lv_obj_add_event_cb(preset, radio_preset_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* pl = lv_label_create(preset);
  lv_label_set_text(pl, LV_SYMBOL_LIST " Preset");
  lv_obj_center(pl);

  _set_freq_ta = makeNumberField(body, "Frequency (MHz)", set_radio_ta_event_cb);
  _set_bw_dd   = makeDropdownField(body, "Bandwidth (kHz)", BW_OPTS_STR);
  _set_sf_dd   = makeDropdownField(body, "Spreading Factor", "5\n6\n7\n8\n9\n10\n11\n12");
  _set_cr_dd   = makeDropdownField(body, "Coding Rate", "5\n6\n7\n8");
  _set_txp_ta  = makeNumberField(body, "TX Power (dBm)", set_radio_ta_event_cb);

  lv_obj_t* apply = lv_btn_create(body);
  lv_obj_add_event_cb(apply, set_radio_apply_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* al = lv_label_create(apply);
  lv_label_set_text(al, LV_SYMBOL_OK " Apply Radio");

  // ===== Routing =====
  addSettingsSection(body, "Routing");
  lv_obj_t* fp = makeField(body, "Path Hash Mode");
  _set_path_dd = lv_dropdown_create(fp);
  lv_dropdown_set_options(_set_path_dd, "1 byte\n2 bytes\n3 bytes");
  lv_obj_set_width(_set_path_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_path_dd, set_pathmode_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ===== Display =====
  addSettingsSection(body, "Display");
  lv_obj_t* fb = makeField(body, "Brightness");
  // The slider knob is taller than its track and would be clipped by the field
  // cell; vertical padding reserves room for it. (margin styles are compiled
  // out of this LVGL config, so pad the cell instead.)
  lv_obj_set_style_pad_top(fb, 6, 0);
  lv_obj_set_style_pad_bottom(fb, 10, 0);
  lv_obj_set_style_pad_right(fb, 8, 0);  // ~half a knob, so the handle at max clears the scroll bar
  _set_bright_slider = lv_slider_create(fb);
  lv_slider_set_range(_set_bright_slider, 10, 100);
  lv_obj_set_width(_set_bright_slider, LV_PCT(100));
  lv_obj_add_event_cb(_set_bright_slider, set_bright_cb, LV_EVENT_ALL, NULL);

  lv_obj_t* fr = makeField(body, "Screen Rotation (restart to apply)");
  _set_rot_dd = lv_dropdown_create(fr);
  lv_dropdown_set_options(_set_rot_dd, "0\n90\n180\n270");
  lv_obj_set_width(_set_rot_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_rot_dd, set_rot_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Shared on-screen keyboard for the settings textareas. Parented to the home
  // screen so it overlays the tabview; hidden until a field is focused.
  _set_kb = lv_keyboard_create(parent);
  lv_obj_add_event_cb(_set_kb, set_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_set_kb, LV_OBJ_FLAG_HIDDEN);

  populateSettings();
}

void UITask::populateSettings() {
  if (!_node_prefs) return;
  lv_textarea_set_text(_set_name_ta, _node_prefs->node_name);

  if (_set_key_ta) {
    char hex[2 * PUB_KEY_SIZE + 1];
    mesh::Utils::toHex(hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
    lv_textarea_set_text(_set_key_ta, hex);
    lv_textarea_set_cursor_pos(_set_key_ta, 0);  // show the start, not the tail
  }

  if (_set_lat_ta && _set_lon_ta && _sensors) {
    char latb[20] = "", lonb[20] = "";
    if (_sensors->node_lat != 0.0 || _sensors->node_lon != 0.0) {
      snprintf(latb, sizeof(latb), "%.6f", _sensors->node_lat);
      snprintf(lonb, sizeof(lonb), "%.6f", _sensors->node_lon);
    }
    lv_textarea_set_text(_set_lat_ta, latb);
    lv_textarea_set_text(_set_lon_ta, lonb);
  }
  if (_set_sharepos) {
    if (_node_prefs->advert_loc_policy == ADVERT_LOC_SHARE)
      lv_obj_add_state(_set_sharepos, LV_STATE_CHECKED);
    else
      lv_obj_clear_state(_set_sharepos, LV_STATE_CHECKED);
  }
  char b[24];
  snprintf(b, sizeof(b), "%g", _node_prefs->freq);         lv_textarea_set_text(_set_freq_ta, b);
  snprintf(b, sizeof(b), "%d", _node_prefs->tx_power_dbm); lv_textarea_set_text(_set_txp_ta, b);

  int bwi = 0;
  for (int i = 0; i < BW_OPTS_N; i++)
    if (fabs(BW_OPTS[i] - _node_prefs->bw) < 0.05) { bwi = i; break; }
  lv_dropdown_set_selected(_set_bw_dd, bwi);
  lv_dropdown_set_selected(_set_sf_dd, (_node_prefs->sf >= 5 && _node_prefs->sf <= 12) ? _node_prefs->sf - 5 : 0);
  lv_dropdown_set_selected(_set_cr_dd, (_node_prefs->cr >= 5 && _node_prefs->cr <= 8) ? _node_prefs->cr - 5 : 0);

  lv_dropdown_set_selected(_set_path_dd, _node_prefs->path_hash_mode <= 2 ? _node_prefs->path_hash_mode : 0);

  uint8_t duty = _node_prefs->display_brightness;
  int pct = duty ? (duty * 100 + 127) / 255 : 50;
  if (pct < 10) pct = 10;
  if (pct > 100) pct = 100;
  lv_slider_set_value(_set_bright_slider, pct, LV_ANIM_OFF);

  lv_dropdown_set_selected(_set_rot_dd, _lgfx ? (_lgfx->getRotation() & 3) : 0);
}

void UITask::commitNodeName() {
  if (!_set_name_ta || !_node_prefs) return;
  const char* nm = lv_textarea_get_text(_set_name_ta);
  if (!nm || !nm[0]) return;
  if (strncmp(nm, _node_prefs->node_name, sizeof(_node_prefs->node_name)) == 0) return;  // unchanged
  strncpy(_node_prefs->node_name, nm, sizeof(_node_prefs->node_name) - 1);
  _node_prefs->node_name[sizeof(_node_prefs->node_name) - 1] = 0;
  the_mesh.savePrefs();
  the_mesh.advert();  // re-broadcast identity with the new name
  showToast("Name saved & advertised");
}

void UITask::applyRadioSettings() {
  if (!_node_prefs) return;
  float freq = atof(lv_textarea_get_text(_set_freq_ta));
  int   txp  = atoi(lv_textarea_get_text(_set_txp_ta));
  int   bwi  = lv_dropdown_get_selected(_set_bw_dd);
  float bw   = BW_OPTS[(bwi >= 0 && bwi < BW_OPTS_N) ? bwi : 0];
  int   sf   = (int)lv_dropdown_get_selected(_set_sf_dd) + 5;   // dropdown index 0 -> SF5
  int   cr   = (int)lv_dropdown_get_selected(_set_cr_dd) + 5;   // dropdown index 0 -> CR5
  // sf/cr/bw come from fixed dropdowns, so only the free-entry fields need range checks.
  if (freq < 150.0f || freq > 2500.0f) { showToast("Freq must be 150-2500 MHz"); return; }
  if (txp < -9 || txp > MAX_LORA_TX_POWER) { showToast("TX power out of range"); return; }
  _node_prefs->freq = freq;
  _node_prefs->bw = bw;
  _node_prefs->sf = (uint8_t)sf;
  _node_prefs->cr = (uint8_t)cr;
  _node_prefs->tx_power_dbm = (int8_t)txp;
  the_mesh.savePrefs();
  radio_set_params(_node_prefs->freq, _node_prefs->bw, _node_prefs->sf, _node_prefs->cr);
  radio_set_tx_power(_node_prefs->tx_power_dbm);
  showToast("Radio settings applied");
}

void UITask::applyPreset(int idx) {
  if (idx < 0 || idx >= RADIO_PRESETS_N) return;
  const RadioPreset& p = RADIO_PRESETS[idx];
  char b[24];
  snprintf(b, sizeof(b), "%g", p.freq);
  lv_textarea_set_text(_set_freq_ta, b);
  int bwi = 0;
  for (int i = 0; i < BW_OPTS_N; i++) if (fabs(BW_OPTS[i] - p.bw) < 0.05) { bwi = i; break; }
  lv_dropdown_set_selected(_set_bw_dd, bwi);
  lv_dropdown_set_selected(_set_sf_dd, (p.sf >= 5 && p.sf <= 12) ? p.sf - 5 : 0);
  lv_dropdown_set_selected(_set_cr_dd, (p.cr >= 5 && p.cr <= 8) ? p.cr - 5 : 0);
  applyRadioSettings();  // validate + persist + push to the radio
}

void UITask::radio_preset_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  lv_obj_t* list = _instance->ensureMenuPopup();
  for (int i = 0; i < RADIO_PRESETS_N; i++) {
    lv_obj_t* b = lv_list_add_btn(list, NULL, RADIO_PRESETS[i].title);
    lv_obj_set_user_data(b, (void*)(intptr_t)i);
    lv_obj_add_event_cb(b, radio_preset_pick_cb, LV_EVENT_CLICKED, NULL);
  }
  _instance->showMenuPopup();
}

void UITask::radio_preset_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  _instance->closeMenuPopup();
  _instance->applyPreset(idx);
}

void UITask::set_name_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    _instance->_set_active_ta = ta;
    lv_keyboard_set_textarea(_instance->_set_kb, ta);
    lv_keyboard_set_mode(_instance->_set_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_set_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_set_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_set_kb);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitNodeName();
  }
}

void UITask::set_radio_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    _instance->_set_active_ta = ta;
    lv_keyboard_set_textarea(_instance->_set_kb, ta);
    lv_keyboard_set_mode(_instance->_set_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_instance->_set_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_set_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_set_kb);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  }
}

void UITask::set_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (_instance->_set_active_ta == _instance->_set_name_ta) _instance->commitNodeName();
    else if (_instance->_set_active_ta == _instance->_set_lat_ta ||
             _instance->_set_active_ta == _instance->_set_lon_ta) _instance->commitPosition();
    _instance->_set_active_ta = NULL;
    lv_obj_add_flag(_instance->_set_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

void UITask::set_radio_apply_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->applyRadioSettings();
}

void UITask::set_pathmode_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->path_hash_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
  the_mesh.savePrefs();
  _instance->showToast("Path hash mode saved");
}

void UITask::set_bright_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  int pct = (int)lv_slider_get_value(lv_event_get_target(e));
  uint8_t duty = (uint8_t)((pct * 255 + 50) / 100);
  if (duty < 1) duty = 1;
  if (code == LV_EVENT_VALUE_CHANGED) {
    board_set_backlight(duty);  // live preview while dragging
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (_instance->_node_prefs) {
      _instance->_node_prefs->display_brightness = duty;
      the_mesh.savePrefs();
    }
  }
}

void UITask::set_rot_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));  // 0..3
  _instance->_node_prefs->display_rotation = (uint8_t)(sel + 1);    // 1-based; 0 = unset
  the_mesh.savePrefs();
  _instance->showToast("Rotation saved (restart to apply)");
}

void UITask::copyToClipboard(const char* text) {
  if (!text) return;
  strncpy(_clipboard, text, sizeof(_clipboard) - 1);
  _clipboard[sizeof(_clipboard) - 1] = 0;
}

void UITask::set_copykey_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, the_mesh.self_id.pub_key, PUB_KEY_SIZE);
  _instance->copyToClipboard(hex);
  _instance->showToast("Public key copied");
}

void UITask::insert_paste_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_clipboard[0] && _instance->_chat_input)
    lv_textarea_add_text(_instance->_chat_input, _instance->_clipboard);
  _instance->closeInsertPopup();
}

void UITask::commitPosition() {
  if (!_sensors || !_set_lat_ta || !_set_lon_ta) return;
  _sensors->node_lat = atof(lv_textarea_get_text(_set_lat_ta));
  _sensors->node_lon = atof(lv_textarea_get_text(_set_lon_ta));
  the_mesh.savePrefs();
}

void UITask::set_pos_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    _instance->_set_active_ta = ta;
    lv_keyboard_set_textarea(_instance->_set_kb, ta);
    lv_keyboard_set_mode(_instance->_set_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_instance->_set_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_set_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_set_kb);
    lv_obj_scroll_to_view(ta, LV_ANIM_OFF);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitPosition();
  }
}

void UITask::set_sharepos_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  bool checked = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  if (checked) {
    _instance->showSharePosWarning();   // confirm before broadcasting location
  } else {
    _instance->_node_prefs->advert_loc_policy = ADVERT_LOC_NONE;
    the_mesh.savePrefs();
    the_mesh.advert();
    _instance->showToast("Position sharing off");
  }
}

void UITask::showSharePosWarning() {
  if (!_confirm_popup) {
    _confirm_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_confirm_popup, _screen_w, _screen_h);
    lv_obj_set_pos(_confirm_popup, 0, 0);
    lv_obj_set_style_bg_color(_confirm_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_confirm_popup, LV_OPA_70, 0);
    lv_obj_set_style_border_width(_confirm_popup, 0, 0);
    lv_obj_set_style_pad_all(_confirm_popup, 0, 0);
    lv_obj_clear_flag(_confirm_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_confirm_popup, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind the modal

    lv_obj_t* card = lv_obj_create(_confirm_popup);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(86), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* warn = lv_label_create(card);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));
    lv_obj_set_style_text_color(warn, lv_color_hex(0xF3F4F6), 0);
    lv_label_set_text(warn,
      "When enabled, your exact latitude and longitude will be broadcast "
      "publically in your adverts.\n\n"
      "Anyone that receives your advert could share your positon with external "
      "parties, including the internet.\n\n"
      "This must be enabled if you want to upload your node to the internet map.");

    lv_obj_t* btns = lv_obj_create(card);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btns, 0, 0);
    lv_obj_set_style_pad_all(btns, 0, 0);
    lv_obj_set_style_pad_column(btns, 8, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* cancel = lv_btn_create(btns);
    lv_obj_add_event_cb(cancel, sharepos_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");

    lv_obj_t* ok = lv_btn_create(btns);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2563EB), 0);
    lv_obj_add_event_cb(ok, sharepos_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* okl = lv_label_create(ok);
    lv_label_set_text(okl, "Enable");
  }
  lv_obj_clear_flag(_confirm_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_confirm_popup);
}

void UITask::sharepos_cancel_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_set_sharepos) lv_obj_clear_state(_instance->_set_sharepos, LV_STATE_CHECKED);
  if (_instance->_confirm_popup) lv_obj_add_flag(_instance->_confirm_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::sharepos_confirm_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->advert_loc_policy = ADVERT_LOC_SHARE;
  the_mesh.savePrefs();
  the_mesh.advert();
  if (_instance->_confirm_popup) lv_obj_add_flag(_instance->_confirm_popup, LV_OBJ_FLAG_HIDDEN);
  _instance->showToast("Position sharing on");
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

  // Poll for contact-set changes made over BLE/serial (add/remove/favourite/
  // rename) -- the companion frame handler doesn't notify the UI.
  if (now - _contacts_check_ms >= 1500) {
    _contacts_check_ms = now;
    if (contactsSignature() != _contacts_sig) rebuildContactsList();
  }

  lv_timer_handler();
}
