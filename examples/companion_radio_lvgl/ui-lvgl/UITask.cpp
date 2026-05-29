#include "UITask.h"
#include "meshcore_assets.h"
#include "MeshProxy.h"                  // UI talks to the backend only through this
#include <helpers/AdvertDataHelpers.h>  // ADV_TYPE_*
#include <helpers/ContactInfo.h>        // ContactInfo (read-model copies)
#include <Utils.h>                      // mesh::Utils::toHex
#include <esp_heap_caps.h>
#include <ctype.h>                      // tolower (case-insensitive search)
#include <strings.h>                    // strcasecmp (A-Z contact sort)

// Dual-core shared-nothing boundary: the UI NEVER calls `the_mesh` directly.
// Reads come from a published snapshot, writes go out as commands, and the mesh
// callbacks arrive as events -- all via mproxy (MeshProxy.h). See that header.

// Upper TX-power bound for the radio-settings validation (was pulled in via
// MyMesh.h, which the UI no longer includes). LORA_TX_POWER is a build flag.
#ifndef MAX_LORA_TX_POWER
  #define MAX_LORA_TX_POWER LORA_TX_POWER
#endif

// Backlight brightness hook. A variant that supports dimming provides a strong
// definition (e.g. CrowPanelBoard's LEDC writer); others fall back to this no-op.
extern "C" __attribute__((weak)) void board_set_backlight(uint8_t duty) { (void)duty; }

static void sanitizeForFont(const char* in, char* out, size_t cap);
static bool containsCI(const char* hay, const char* needle);
static const lv_font_t* withEmoji(const lv_font_t* base);  // base font + emoji/unicode fallback

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
  // text_font is inherited, so this gives every default-font field on the screen
  // (compose box, name/nickname inputs, channel list, search, etc.) emoji/unicode
  // support. Widgets with an explicit larger font wrap it via withEmoji() too.
  lv_obj_set_style_text_font(scr, withEmoji(&lv_font_montserrat_14), 0);
}

// ---- draggable scrollbar handle ------------------------------------------
// LVGL's native scrollbar is draw-only. On this slow-to-repaint panel, a long
// list (hundreds of contacts) is tedious to flick; a grabbable thumb turns a big
// jump into one gesture. A thin floating thumb overlays the right edge of a
// scrollable object: dragging it maps finger Y -> scroll offset, and the object's
// own scrolling (flick) moves the thumb to match. content<->thumb are linked via
// the thumb's user_data (content) and the content event's user_data (thumb), so
// the content's own user_data is left untouched.
static void sb_update(lv_obj_t* content, lv_obj_t* thumb) {
  if (!content || !thumb) return;
  lv_coord_t view_h = lv_obj_get_height(content);
  lv_coord_t sy = lv_obj_get_scroll_y(content);
  lv_coord_t total = sy + lv_obj_get_scroll_bottom(content);   // full scroll range
  if (total <= 0 || view_h <= 0) { lv_obj_add_flag(thumb, LV_OBJ_FLAG_HIDDEN); return; }
  lv_obj_clear_flag(thumb, LV_OBJ_FLAG_HIDDEN);
  lv_coord_t content_h = view_h + total;
  lv_coord_t th = (lv_coord_t)((int32_t)view_h * view_h / content_h);
  if (th < 24) th = 24;
  lv_coord_t max_y = view_h - th;
  if (max_y < 0) max_y = 0;
  lv_coord_t ty = (max_y > 0) ? (lv_coord_t)((int32_t)max_y * sy / total) : 0;
  lv_obj_set_height(thumb, th);
  lv_obj_set_pos(thumb, lv_obj_get_x(content) + lv_obj_get_width(content) - lv_obj_get_width(thumb) - 2,
                 lv_obj_get_y(content) + ty);
}
static void sb_scroll_cb(lv_event_t* e) {
  sb_update(lv_event_get_target(e), (lv_obj_t*)lv_event_get_user_data(e));
}
static void sb_drag_cb(lv_event_t* e) {
  lv_obj_t* thumb = lv_event_get_target(e);
  lv_obj_t* content = (lv_obj_t*)lv_obj_get_user_data(thumb);
  lv_indev_t* indev = lv_indev_get_act();
  if (!content || !indev) return;
  lv_point_t p; lv_indev_get_point(indev, &p);
  lv_area_t pa; lv_obj_get_coords(lv_obj_get_parent(thumb), &pa);
  lv_coord_t view_h = lv_obj_get_height(content);
  lv_coord_t th = lv_obj_get_height(thumb);
  lv_coord_t max_y = view_h - th;
  if (max_y <= 0) return;
  lv_coord_t rel = (p.y - pa.y1) - lv_obj_get_y(content) - th / 2;   // center the thumb on the finger
  if (rel < 0) rel = 0;
  if (rel > max_y) rel = max_y;
  lv_coord_t total = lv_obj_get_scroll_y(content) + lv_obj_get_scroll_bottom(content);
  if (total <= 0) return;
  lv_obj_scroll_to_y(content, (lv_coord_t)((int32_t)total * rel / max_y), LV_ANIM_OFF);
  // scroll_to_y emits LV_EVENT_SCROLL -> sb_update repositions the thumb.
}
static lv_obj_t* attachScrollHandle(lv_obj_t* content) {
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);   // hide the draw-only native bar
  lv_obj_t* thumb = lv_obj_create(lv_obj_get_parent(content));
  lv_obj_add_flag(thumb, LV_OBJ_FLAG_FLOATING);               // ignored by parent layout
  lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(thumb, 10, 24);
  lv_obj_set_style_radius(thumb, 5, 0);
  lv_obj_set_style_bg_color(thumb, lv_color_hex(0x6B7280), 0);
  lv_obj_set_style_bg_opa(thumb, LV_OPA_60, 0);
  lv_obj_set_style_border_width(thumb, 0, 0);
  lv_obj_set_style_pad_all(thumb, 0, 0);
  lv_obj_add_flag(thumb, LV_OBJ_FLAG_HIDDEN);                 // shown once there's range to scroll
  lv_obj_set_user_data(thumb, content);
  lv_obj_add_event_cb(thumb, sb_drag_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(content, sb_scroll_cb, LV_EVENT_SCROLL, thumb);
  lv_obj_add_event_cb(content, sb_scroll_cb, LV_EVENT_SIZE_CHANGED, thumb);
  return thumb;
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

  // Live device clock (right side of the header), updated once a second in loop().
  _clock_label = lv_label_create(header);
  lv_label_set_text(_clock_label, "--");
  lv_obj_set_style_text_color(_clock_label, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(_clock_label, &lv_font_montserrat_14, 0);
  lv_obj_align(_clock_label, LV_ALIGN_RIGHT_MID, -4, 0);

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

  // ----- Contacts tab: fixed search/filter band + a virtualized lv_table -----
  lv_obj_set_style_pad_all(_tab_contacts, 0, 0);
  lv_obj_clear_flag(_tab_contacts, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(_tab_contacts, LV_FLEX_FLOW_COLUMN);

  // Search row: text field (grows) + a filter/menu button on the right that
  // opens the order/filter pop-out.
  lv_obj_t* cctl = lv_obj_create(_tab_contacts);
  lv_obj_set_width(cctl, LV_PCT(100));
  lv_obj_set_height(cctl, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(cctl, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(cctl, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cctl, 0, 0);
  lv_obj_set_style_radius(cctl, 0, 0);
  lv_obj_set_style_pad_all(cctl, 6, 0);
  lv_obj_set_style_pad_column(cctl, 6, 0);
  lv_obj_clear_flag(cctl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cctl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  _contacts_search_ta = lv_textarea_create(cctl);
  lv_textarea_set_one_line(_contacts_search_ta, true);
  lv_textarea_set_placeholder_text(_contacts_search_ta, LV_SYMBOL_EYE_OPEN " Search");
  lv_obj_set_flex_grow(_contacts_search_ta, 1);
  lv_obj_add_event_cb(_contacts_search_ta, contacts_search_ta_cb, LV_EVENT_ALL, NULL);

  _contacts_filter_btn = lv_btn_create(cctl);
  lv_obj_add_event_cb(_contacts_filter_btn, contacts_filter_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* fbl = lv_label_create(_contacts_filter_btn);
  lv_label_set_text(fbl, LV_SYMBOL_LIST);
  lv_obj_center(fbl);

  _contacts_table = lv_table_create(_tab_contacts);
  lv_obj_set_width(_contacts_table, LV_PCT(100));
  lv_obj_set_flex_grow(_contacts_table, 1);            // fill remaining height -> internal scroll
  lv_obj_set_style_bg_color(_contacts_table, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_contacts_table, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_contacts_table, 0, 0);
  lv_obj_set_style_radius(_contacts_table, 0, 0);
  lv_obj_set_style_bg_color(_contacts_table, lv_color_hex(BG_HEX), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(_contacts_table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(_contacts_table, lv_color_hex(FG_HEX), LV_PART_ITEMS);
  lv_obj_set_style_text_font(_contacts_table, withEmoji(&lv_font_montserrat_14), LV_PART_ITEMS);
  lv_obj_set_style_pad_top(_contacts_table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(_contacts_table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(_contacts_table, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_color(_contacts_table, lv_color_hex(0x374151), LV_PART_ITEMS);
  lv_obj_set_style_border_width(_contacts_table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_side(_contacts_table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
  lv_table_set_col_cnt(_contacts_table, 2);
  lv_table_set_col_width(_contacts_table, 0, _screen_w - 64);
  lv_table_set_col_width(_contacts_table, 1, 56);
  // lv_table reports the tapped cell via VALUE_CHANGED on release (and only when
  // not scrolling), then clears the selection -- so CLICKED would see CELL_NONE.
  lv_obj_add_event_cb(_contacts_table, contacts_table_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(_contacts_table, contacts_table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);
  _contacts_sb = attachScrollHandle(_contacts_table);

  // Search keyboard (hidden; overlays the home screen, like the settings one).
  _contacts_kb = lv_keyboard_create(scr);
  lv_keyboard_set_textarea(_contacts_kb, _contacts_search_ta);
  lv_obj_add_event_cb(_contacts_kb, contacts_kb_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_contacts_kb, LV_OBJ_FLAG_HIDDEN);

  _status_label = NULL;

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
  // Keep it to one unit and at most 2 digits, rolling up so old contacts read
  // "2y" rather than "760d": s -> m -> h -> d(<1wk) -> w(<1yr) -> y(capped 99).
  if (age < 60)            snprintf(out, cap, "%us", (unsigned)age);
  else if (age < 3600)     snprintf(out, cap, "%um", (unsigned)(age / 60));
  else if (age < 86400)    snprintf(out, cap, "%uh", (unsigned)(age / 3600));
  else if (age < 604800)   snprintf(out, cap, "%ud", (unsigned)(age / 86400));    // 1-6 d
  else if (age < 31536000) snprintf(out, cap, "%uw", (unsigned)(age / 604800));   // 1-52 w
  else { unsigned y = age / 31536000; if (y > 99) y = 99; snprintf(out, cap, "%uy", y); }
}

static constexpr uint8_t CONTACT_FLAG_FAVOURITE = 0x01;  // matches firmware
static constexpr uint32_t FAV_HEX = 0xFBBF24;            // amber-400 accent

// (contactsSignature() moved to the backend — see mproxy::publishIfChanged in
// MeshProxy.cpp. The UI now rebuilds when the snapshot version changes.)

// Local nickname if one is set for this contact, else the advert name.
const char* UITask::displayName(const uint8_t* pubkey, const char* realname, char* buf, size_t cap) {
  if (pubkey && mproxy::getNameOverride(pubkey, buf, cap) && buf[0]) return buf;
  strncpy(buf, realname ? realname : "", cap - 1);
  buf[cap - 1] = 0;
  return buf;
}

// Filter predicate: the pop-out's filter choice + the name search box.
// _contacts_filt: 0=All,1=Favorites,2=Users,3=Repeaters,4=Room Servers,5=Sensors.
bool UITask::contactPasses(const ContactInfo& c) {
  switch (_contacts_filt) {
    case 1: if (!(c.flags & CONTACT_FLAG_FAVOURITE)) return false; break;
    case 2: if (c.type != ADV_TYPE_CHAT)     return false; break;
    case 3: if (c.type != ADV_TYPE_REPEATER) return false; break;
    case 4: if (c.type != ADV_TYPE_ROOM)     return false; break;
    case 5: if (c.type != ADV_TYPE_SENSOR)   return false; break;
    default: break;  // All
  }
  if (_contacts_filter[0]) {
    char dn[CHAT_PEER_NAME_MAX];
    displayName(c.id.pub_key, c.name, dn, sizeof(dn));
    if (!containsCI(dn, _contacts_filter) && !containsCI(c.name, _contacts_filter)) return false;
  }
  return true;
}

// Name collation: order by character class first (letters, then numbers, then
// symbols), then case-insensitively by value -- so "Alice" < "Zoe" < "7th" < "_hub".
static int nameClass(unsigned char c) {
  if (isalpha(c)) return 0;
  if (isdigit(c)) return 1;
  return 2;
}
static int nameCmpLNS(const char* a, const char* b) {
  const unsigned char* pa = (const unsigned char*)a;
  const unsigned char* pb = (const unsigned char*)b;
  while (*pa && *pb) {
    int ka = nameClass(*pa), kb = nameClass(*pb);
    if (ka != kb) return ka - kb;
    unsigned char la = (unsigned char)tolower(*pa), lb = (unsigned char)tolower(*pb);
    if (la != lb) return (int)la - (int)lb;
    pa++; pb++;
  }
  return (int)*pa - (int)*pb;  // shorter string sorts first
}

// qsort comparator reads the active order from this file-static (single-threaded
// LVGL). 0=A-Z (by name), 1=Heard Recently, 2=Latest Messages (both newest-first
// using the row's `heard` key populated per the chosen order).
static int s_contacts_order = 1;
int UITask::crow_cmp(const void* pa, const void* pb) {
  const ContactDispRow* a = (const ContactDispRow*)pa;
  const ContactDispRow* b = (const ContactDispRow*)pb;
  if (a->fav != b->fav) return (int)b->fav - (int)a->fav;  // favourites first, then order within each group
  if (s_contacts_order == 0) {
    ContactInfo ca, cb;
    mproxy::getContactByIdx(a->idx, ca);
    mproxy::getContactByIdx(b->idx, cb);
    return nameCmpLNS(ca.name, cb.name);
  }
  if (a->heard != b->heard) return (b->heard > a->heard) ? 1 : -1;
  return 0;
}

void UITask::rebuildContactsList() {
  if (!_contacts_table) return;
  _contacts_dirty = false;
  _contacts_rebuilt_ms = millis();

  // Build the filtered display set (indices only -- no per-contact widgets).
  _crow_count = 0;
  int total = mproxy::getNumContacts();
  for (int i = 0; i < total && _crow_count < CONTACTS_MAX_ROWS; i++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (!contactPasses(c)) continue;
    _crows[_crow_count].idx = (uint16_t)i;
    // Sort key: latest message time for "Latest Messages", else last-heard by OUR
    // clock (lastmod) -- not last_advert_timestamp, which is their (untrusted) clock.
    _crows[_crow_count].heard = (_contacts_order == 2)
        ? _msgs->latestTimestampFor(c.name) : c.lastmod;
    _crows[_crow_count].fav = (c.flags & CONTACT_FLAG_FAVOURITE) ? 1 : 0;
    _crow_count++;
  }
  s_contacts_order = _contacts_order;
  qsort(_crows, _crow_count, sizeof(ContactDispRow), crow_cmp);

  if (_crow_count == 0) {
    lv_table_set_row_cnt(_contacts_table, 1);
    lv_table_set_cell_value(_contacts_table, 0, 0,
        mproxy::getNumContacts() > 0 ? "No contacts match." : "No contacts yet. Waiting for adverts...");
    lv_table_set_cell_value(_contacts_table, 0, 1, "");
    sb_update(_contacts_table, _contacts_sb);   // nothing to scroll -> hide thumb
    return;
  }

  uint32_t now = mproxy::rtcSeconds();
  lv_table_set_row_cnt(_contacts_table, _crow_count);
  for (int r = 0; r < _crow_count; r++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(_crows[r].idx, c)) continue;
    char dn[CHAT_PEER_NAME_MAX];
    displayName(c.id.pub_key, c.name, dn, sizeof(dn));
    char clean[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(dn[0] ? dn : "(unnamed)", clean, sizeof(clean));
    char cell[CHAT_PEER_NAME_MAX + 12];
    snprintf(cell, sizeof(cell), "%s %s", contactSymbol(c.type), clean);
    lv_table_set_cell_value(_contacts_table, r, 0, cell);
    char ago[16];
    // lastmod = when WE last heard them (our clock). last_advert_timestamp is
    // THEIR clock and can't be trusted (bad RTCs report wild times).
    formatLastSeen(ago, sizeof(ago), c.lastmod, now);
    lv_table_set_cell_value(_contacts_table, r, 1, ago);
  }
  lv_obj_scroll_to_y(_contacts_table, 0, LV_ANIM_OFF);
  lv_obj_update_layout(_contacts_table);   // refresh scroll range -> resize/show the drag thumb
  sb_update(_contacts_table, _contacts_sb);
}

void UITask::contacts_table_cb(lv_event_t* e) {
  if (!_instance) return;
  uint16_t row = 0, col = 0;
  lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
  if (row == LV_TABLE_CELL_NONE || (int)row >= _instance->_crow_count) return;
  ContactInfo c;
  if (!mproxy::getContactByIdx(_instance->_crows[row].idx, c)) return;
  _instance->_chat_is_channel = false;
  memcpy(_instance->_chat_pubkey, c.id.pub_key, 6);
  _instance->openChat(c.name);
}

void UITask::contacts_table_draw_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || dsc->label_dsc == NULL) return;
  uint16_t cols = lv_table_get_col_cnt(lv_event_get_target(e));
  uint32_t row = dsc->id / cols;
  uint32_t col = dsc->id % cols;
  if (col == 1) {
    dsc->label_dsc->color = lv_color_hex(DIM_HEX);                 // age column
  } else if ((int)row < _instance->_crow_count && _instance->_crows[row].fav) {
    dsc->label_dsc->color = lv_color_hex(FAV_HEX);                 // favourite name
  } else {
    dsc->label_dsc->color = lv_color_hex(FG_HEX);
  }
}

void UITask::contacts_search_ta_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_contacts_kb, ta);
    lv_keyboard_set_mode(_instance->_contacts_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_contacts_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_contacts_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_contacts_kb);
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    const char* s = lv_textarea_get_text(ta);
    strncpy(_instance->_contacts_filter, s ? s : "", sizeof(_instance->_contacts_filter) - 1);
    _instance->_contacts_filter[sizeof(_instance->_contacts_filter) - 1] = 0;
    _instance->rebuildContactsList();
  }
}

void UITask::contacts_kb_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    lv_obj_add_flag(_instance->_contacts_kb, LV_OBJ_FLAG_HIDDEN);
}

// A pick-one row: [indicator][label], clickable, value stashed in user_data.
// Indicator (child 0) shows a check when selected; refreshed in place (no clean).
static lv_obj_t* addRadioRow(lv_obj_t* grp, const char* text, int value, lv_event_cb_t cb) {
  lv_obj_t* row = lv_obj_create(grp);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 6, 0);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(row, (void*)(intptr_t)value);
  lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ind = lv_label_create(row);
  lv_obj_set_width(ind, 16);
  lv_label_set_text(ind, " ");
  lv_obj_set_style_text_color(ind, lv_color_hex(0x34D399), 0);
  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xF3F4F6), 0);
  return row;
}

static lv_obj_t* addFilterHeader(lv_obj_t* card, const char* text) {
  lv_obj_t* h = lv_label_create(card);
  lv_label_set_text(h, text);
  lv_obj_set_style_text_color(h, lv_color_hex(0x60A5FA), 0);
  lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
  lv_obj_set_style_pad_top(h, 4, 0);
  return h;
}

void UITask::showContactsFilter() {
  if (!_cfilt_popup) {
    _cfilt_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_cfilt_popup, _screen_w, _screen_h);
    lv_obj_set_pos(_cfilt_popup, 0, 0);
    lv_obj_set_style_bg_color(_cfilt_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_cfilt_popup, LV_OPA_50, 0);
    lv_obj_set_style_border_width(_cfilt_popup, 0, 0);
    lv_obj_set_style_pad_all(_cfilt_popup, 0, 0);
    lv_obj_add_flag(_cfilt_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_cfilt_popup, [](lv_event_t* ev) {  // tap backdrop to close
      if (_instance && lv_event_get_target(ev) == _instance->_cfilt_popup)
        lv_obj_add_flag(_instance->_cfilt_popup, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* card = lv_obj_create(_cfilt_popup);
    lv_obj_set_width(card, LV_PCT(80));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(90), 0);
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -6, HEADER_H + 6);  // drop down from the filter button
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    addFilterHeader(card, "Order");
    _cfilt_order_grp = lv_obj_create(card);
    lv_obj_set_width(_cfilt_order_grp, LV_PCT(100));
    lv_obj_set_height(_cfilt_order_grp, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(_cfilt_order_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_cfilt_order_grp, 0, 0);
    lv_obj_set_style_pad_all(_cfilt_order_grp, 0, 0);
    lv_obj_clear_flag(_cfilt_order_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_cfilt_order_grp, LV_FLEX_FLOW_COLUMN);
    addRadioRow(_cfilt_order_grp, "A-Z", 0, contacts_order_pick_cb);
    addRadioRow(_cfilt_order_grp, "Heard Recently", 1, contacts_order_pick_cb);
    addRadioRow(_cfilt_order_grp, "Latest Messages", 2, contacts_order_pick_cb);

    addFilterHeader(card, "Filter");
    _cfilt_filt_grp = lv_obj_create(card);
    lv_obj_set_width(_cfilt_filt_grp, LV_PCT(100));
    lv_obj_set_height(_cfilt_filt_grp, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(_cfilt_filt_grp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_cfilt_filt_grp, 0, 0);
    lv_obj_set_style_pad_all(_cfilt_filt_grp, 0, 0);
    lv_obj_clear_flag(_cfilt_filt_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_cfilt_filt_grp, LV_FLEX_FLOW_COLUMN);
    addRadioRow(_cfilt_filt_grp, "All", 0, contacts_filt_pick_cb);
    addRadioRow(_cfilt_filt_grp, "Favorites", 1, contacts_filt_pick_cb);
    addRadioRow(_cfilt_filt_grp, "Users", 2, contacts_filt_pick_cb);
    addRadioRow(_cfilt_filt_grp, "Repeaters", 3, contacts_filt_pick_cb);
    addRadioRow(_cfilt_filt_grp, "Room Servers", 4, contacts_filt_pick_cb);
    addRadioRow(_cfilt_filt_grp, "Sensors", 5, contacts_filt_pick_cb);
  }
  refreshContactsFilterChecks();
  lv_obj_clear_flag(_cfilt_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_cfilt_popup);
}

void UITask::refreshContactsFilterChecks() {
  if (_cfilt_order_grp) {
    uint32_t n = lv_obj_get_child_cnt(_cfilt_order_grp);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t* row = lv_obj_get_child(_cfilt_order_grp, i);
      int val = (int)(intptr_t)lv_obj_get_user_data(row);
      lv_label_set_text(lv_obj_get_child(row, 0), val == _contacts_order ? LV_SYMBOL_OK : " ");
    }
  }
  if (_cfilt_filt_grp) {
    uint32_t n = lv_obj_get_child_cnt(_cfilt_filt_grp);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t* row = lv_obj_get_child(_cfilt_filt_grp, i);
      int val = (int)(intptr_t)lv_obj_get_user_data(row);
      lv_label_set_text(lv_obj_get_child(row, 0), val == _contacts_filt ? LV_SYMBOL_OK : " ");
    }
  }
}

void UITask::contacts_filter_btn_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->showContactsFilter();
}

void UITask::contacts_order_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  _instance->_contacts_order = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if (_instance->_node_prefs) {
    _instance->_node_prefs->contacts_order = (uint8_t)_instance->_contacts_order;
    pushPrefs();
  }
  _instance->refreshContactsFilterChecks();
  _instance->rebuildContactsList();
}

void UITask::contacts_filt_pick_cb(lv_event_t* e) {
  if (!_instance) return;
  _instance->_contacts_filt = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
  if (_instance->_node_prefs) {
    _instance->_node_prefs->contacts_filter = (uint8_t)_instance->_contacts_filt;
    pushPrefs();
  }
  _instance->refreshContactsFilterChecks();
  _instance->rebuildContactsList();
}

void UITask::channel_clicked_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* btn = lv_event_get_target(e);
  int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
  ChannelDetails ch;
  if (mproxy::getChannel(idx, ch) && ch.name[0]) {
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
    if (!mproxy::getChannel(idx, ch) || ch.name[0] == 0) continue;

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
  sb_update(_chat_history, _chat_sb);
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

// ===== Virtualized contact picker (share recipient / insert reference) =======
// A full-screen top-layer panel with a search field + lv_table -- same scalable
// model as the Contacts tab, so it shows the whole address book without a cap.

int UITask::prow_cmp(const void* pa, const void* pb) {
  const ContactDispRow* a = (const ContactDispRow*)pa;
  const ContactDispRow* b = (const ContactDispRow*)pb;
  if (a->fav != b->fav) return (int)b->fav - (int)a->fav;  // favourites first, then A-Z
  ContactInfo ca, cb;
  mproxy::getContactByIdx(a->idx, ca);
  mproxy::getContactByIdx(b->idx, cb);
  return nameCmpLNS(ca.name, cb.name);
}

void UITask::buildContactPickerScreen() {
  if (_pick_popup) return;
  _pick_popup = lv_obj_create(lv_layer_top());
  lv_obj_set_size(_pick_popup, _screen_w, _screen_h);
  lv_obj_set_pos(_pick_popup, 0, 0);
  styleAsDarkScreen(_pick_popup);
  lv_obj_set_style_pad_all(_pick_popup, 0, 0);
  lv_obj_set_flex_flow(_pick_popup, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* bar = lv_obj_create(_pick_popup);
  lv_obj_set_width(bar, LV_PCT(100));
  lv_obj_set_height(bar, HEADER_H);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x1F2937), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  _pick_title = lv_label_create(bar);
  lv_obj_set_style_text_color(_pick_title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_pick_title, &lv_font_montserrat_20, 0);
  lv_obj_align(_pick_title, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_t* close = lv_btn_create(bar);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(close, 0, 0);
  lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(close, pick_close_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* cl = lv_label_create(close);
  lv_label_set_text(cl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(cl, lv_color_hex(FG_HEX), 0);

  lv_obj_t* srow = lv_obj_create(_pick_popup);
  lv_obj_set_width(srow, LV_PCT(100));
  lv_obj_set_height(srow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(srow, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(srow, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(srow, 0, 0);
  lv_obj_set_style_radius(srow, 0, 0);
  lv_obj_set_style_pad_all(srow, 6, 0);
  lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
  _pick_search_ta = lv_textarea_create(srow);
  lv_textarea_set_one_line(_pick_search_ta, true);
  lv_textarea_set_placeholder_text(_pick_search_ta, LV_SYMBOL_EYE_OPEN " Search");
  lv_obj_set_width(_pick_search_ta, LV_PCT(100));
  lv_obj_add_event_cb(_pick_search_ta, pick_search_ta_cb, LV_EVENT_ALL, NULL);

  _pick_table = lv_table_create(_pick_popup);
  lv_obj_set_width(_pick_table, LV_PCT(100));
  lv_obj_set_flex_grow(_pick_table, 1);
  lv_obj_set_style_bg_color(_pick_table, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_pick_table, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_pick_table, 0, 0);
  lv_obj_set_style_radius(_pick_table, 0, 0);
  lv_obj_set_style_bg_color(_pick_table, lv_color_hex(BG_HEX), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(_pick_table, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(_pick_table, lv_color_hex(FG_HEX), LV_PART_ITEMS);
  lv_obj_set_style_text_font(_pick_table, withEmoji(&lv_font_montserrat_14), LV_PART_ITEMS);
  lv_obj_set_style_pad_top(_pick_table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_bottom(_pick_table, 10, LV_PART_ITEMS);
  lv_obj_set_style_pad_left(_pick_table, 8, LV_PART_ITEMS);
  lv_obj_set_style_border_color(_pick_table, lv_color_hex(0x374151), LV_PART_ITEMS);
  lv_obj_set_style_border_width(_pick_table, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_side(_pick_table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
  lv_table_set_col_cnt(_pick_table, 1);
  lv_table_set_col_width(_pick_table, 0, _screen_w - 8);
  lv_obj_add_event_cb(_pick_table, pick_table_cb, LV_EVENT_VALUE_CHANGED, NULL);
  _pick_sb = attachScrollHandle(_pick_table);
  lv_obj_add_event_cb(_pick_table, pick_table_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

  _pick_kb = lv_keyboard_create(_pick_popup);
  lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_FLOATING);   // overlay, don't take a flex slot
  lv_keyboard_set_textarea(_pick_kb, _pick_search_ta);
  lv_obj_add_event_cb(_pick_kb, pick_kb_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::rebuildPicker() {
  if (!_pick_table) return;
  _prow_count = 0;
  int total = mproxy::getNumContacts();
  for (int i = 0; i < total && _prow_count < CONTACTS_MAX_ROWS; i++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (_pick_filter[0] && !containsCI(c.name, _pick_filter)) continue;
    _prows[_prow_count].idx = (uint16_t)i;
    _prows[_prow_count].heard = c.lastmod;   // our clock (see rebuildContactsList)
    _prows[_prow_count].fav = (c.flags & CONTACT_FLAG_FAVOURITE) ? 1 : 0;
    _prow_count++;
  }
  qsort(_prows, _prow_count, sizeof(ContactDispRow), prow_cmp);
  if (_prow_count == 0) {
    lv_table_set_row_cnt(_pick_table, 1);
    lv_table_set_cell_value(_pick_table, 0, 0, "No contacts.");
    return;
  }
  lv_table_set_row_cnt(_pick_table, _prow_count);
  for (int r = 0; r < _prow_count; r++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(_prows[r].idx, c)) continue;
    char dn[CHAT_PEER_NAME_MAX];
    displayName(c.id.pub_key, c.name, dn, sizeof(dn));
    char clean[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(dn[0] ? dn : "(unnamed)", clean, sizeof(clean));
    char cell[CHAT_PEER_NAME_MAX + 12];
    snprintf(cell, sizeof(cell), "%s %s", contactSymbol(c.type), clean);
    lv_table_set_cell_value(_pick_table, r, 0, cell);
  }
  lv_obj_scroll_to_y(_pick_table, 0, LV_ANIM_OFF);
  lv_obj_update_layout(_pick_table);
  sb_update(_pick_table, _pick_sb);
}

void UITask::openContactPicker(int action) {
  buildContactPickerScreen();
  _pick_action = action;
  _pick_filter[0] = 0;
  lv_textarea_set_text(_pick_search_ta, "");
  lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(_pick_title, action == 1 ? "Send to..." : "Share contact");
  rebuildPicker();
  lv_obj_clear_flag(_pick_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_pick_popup);
}

void UITask::closeContactPicker() {
  if (_pick_kb) lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_HIDDEN);
  if (_pick_popup) lv_obj_add_flag(_pick_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::pick_close_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->closeContactPicker();
}

void UITask::pick_search_ta_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_pick_kb, ta);
    lv_keyboard_set_mode(_instance->_pick_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_pick_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_pick_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_pick_kb);
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    const char* s = lv_textarea_get_text(ta);
    strncpy(_instance->_pick_filter, s ? s : "", sizeof(_instance->_pick_filter) - 1);
    _instance->_pick_filter[sizeof(_instance->_pick_filter) - 1] = 0;
    _instance->rebuildPicker();
  }
}

void UITask::pick_kb_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    lv_obj_add_flag(_instance->_pick_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::pick_table_draw_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_draw_part_dsc_t* dsc = lv_event_get_draw_part_dsc(e);
  if (dsc->part != LV_PART_ITEMS || dsc->label_dsc == NULL) return;
  uint32_t row = dsc->id;  // single column: cell id == row
  dsc->label_dsc->color = ((int)row < _instance->_prow_count && _instance->_prows[row].fav)
      ? lv_color_hex(FAV_HEX) : lv_color_hex(FG_HEX);
}

void UITask::pick_table_cb(lv_event_t* e) {
  if (!_instance) return;
  uint16_t row = 0, col = 0;
  lv_table_get_selected_cell(lv_event_get_target(e), &row, &col);
  if (row == LV_TABLE_CELL_NONE || (int)row >= _instance->_prow_count) return;
  ContactInfo pick;
  if (!mproxy::getContactByIdx(_instance->_prows[row].idx, pick)) return;
  int action = _instance->_pick_action;
  _instance->closeContactPicker();
  if (action == 1) {  // share the viewed contact (cinfo) to the picked recipient
    ContactInfo* viewed = _instance->cinfoContact();
    if (viewed) {
      char hex[2 * PUB_KEY_SIZE + 1];
      mesh::Utils::toHex(hex, viewed->id.pub_key, PUB_KEY_SIZE);
      char ref[2 * PUB_KEY_SIZE + 48];
      snprintf(ref, sizeof(ref), "<%s:%u:%s>", hex, (unsigned)viewed->type, viewed->name);
      const char* me = (_instance->_node_prefs && _instance->_node_prefs->node_name[0])
                           ? _instance->_node_prefs->node_name : "Me";
      char key[CHAT_PEER_NAME_MAX];
      convKey(pick.id.pub_key, false, key, sizeof(key));
      _instance->postSend(false, pick.id.pub_key, -1, key, me, ref);
      _instance->showToast("Contact shared");
    }
  } else if (action == 2) {  // insert the picked contact's ref into compose
    _instance->insertContactRef(pick.id.pub_key, pick.type, pick.name);
  }
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
  _instance->insertContactRef(mproxy::selfPubKey(), ADV_TYPE_CHAT, nm);
  _instance->closeInsertPopup();
}

void UITask::insert_share_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeInsertPopup();
  _instance->openContactPicker(2);  // pick a contact -> insert its <ref> into compose
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
      int total = mproxy::getNumContacts();
      ContactInfo c;
      for (int i = 0; i < total; i++) {
        if (!mproxy::getContactByIdx(i, c)) continue;
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

void UITask::raiseFieldForKb(lv_obj_t* scroll, lv_obj_t* kb, lv_obj_t* ta) {
  if (!scroll || !kb || !ta) return;
  resetKbScroll();   // undo any previous field's padding before re-applying
  lv_obj_update_layout(kb);
  lv_area_t ka, fa;
  lv_obj_get_coords(kb, &ka);
  lv_obj_get_coords(ta, &fa);
  lv_coord_t over = fa.y2 + 3 - ka.y1;    // leave only ~3px between the field and the keyboard
  if (over <= 0) return;                   // already clear of the keyboard -- leave it
  _kb_scroll = scroll;
  _kb_scroll_pad = lv_obj_get_style_pad_bottom(scroll, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scroll, _kb_scroll_pad + over, LV_PART_MAIN);  // room to scroll up
  lv_obj_update_layout(scroll);
  lv_obj_scroll_by(scroll, 0, -over, LV_ANIM_OFF);
}

void UITask::resetKbScroll() {
  if (!_kb_scroll) return;
  lv_obj_set_style_pad_bottom(_kb_scroll, _kb_scroll_pad, LV_PART_MAIN);
  lv_obj_update_layout(_kb_scroll);
  // Removing the padding shrinks the scrollable area; if we were scrolled into
  // it, pull the content back down so there's no empty gap at the bottom.
  lv_coord_t sb = lv_obj_get_scroll_bottom(_kb_scroll);
  if (sb < 0) lv_obj_scroll_by(_kb_scroll, 0, -sb, LV_ANIM_OFF);
  _kb_scroll = NULL;
}

void UITask::storeAppend(bool outgoing, const char* key, const char* sender,
                         const char* text, uint32_t ts, uint8_t status, uint32_t ack, uint32_t expiry_ms,
                         uint32_t cli) {
  _rammsgs.append(outgoing, key, sender, text, ts, status, ack, expiry_ms, cli);  // always: recent ring
#ifdef HAS_SD_CARD
  if (_msgs == &_sdmsgs) _sdmsgs.append(outgoing, key, sender, text, ts, status, ack, expiry_ms, cli);
#endif
}

void UITask::convKey(const uint8_t* key6, bool is_channel, char* out, size_t cap) {
  if (is_channel && cap > 16) {
    out[0] = 'c'; out[1] = 'h'; out[2] = '_';
    mesh::Utils::toHex(out + 3, key6, 6);   // "ch_" + 12 hex
  } else {
    mesh::Utils::toHex(out, key6, 6);        // 12 hex
  }
}

// ---- command-posting helpers (UI core -> backend, via MeshProxy) ------------
void UITask::pushPrefs() {
  if (!_instance || !_instance->_node_prefs) return;
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::UpdatePrefs;
  c.prefs = *_instance->_node_prefs;
  mproxy::postCommand(c);
}
void UITask::pushRadio() {
  if (!_instance || !_instance->_node_prefs) return;
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::ApplyRadio;
  c.prefs = *_instance->_node_prefs;
  mproxy::postCommand(c);
}
void UITask::pushAdvert() {
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::Advert;
  mproxy::postCommand(c);
}
void UITask::postPubkeyCmd(mproxy::CmdKind kind, const uint8_t* pk) {
  mproxy::MeshCmd c{};
  c.kind = kind;
  memcpy(c.pubkey, pk, PUB_KEY_SIZE);
  mproxy::postCommand(c);
}

// Optimistic async send. Appends a SENDING bubble with a fresh client token and
// posts CMD_Send; the backend replies EV_SendResult with the real ack/timeout
// (resolved in drainEvents via setByClientToken). Returns the token.
uint32_t UITask::postSend(bool is_channel, const uint8_t* pubkey6, int channel_idx,
                          const char* conv_key, const char* sender, const char* text) {
  uint32_t token = ++_send_seq;
  if (token == 0) token = ++_send_seq;   // 0 means "no token"
  storeAppend(true, conv_key, sender, text, mproxy::rtcSeconds(),
              MSG_STATUS_SENDING, 0, millis() + 8000, token);  // provisional expiry until EV_SendResult
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::Send;
  c.token = token;
  c.is_channel = is_channel;
  c.channel_idx = channel_idx;
  if (pubkey6) memcpy(c.pubkey, pubkey6, 6);
  strncpy(c.text, text, sizeof(c.text) - 1);
  c.text[sizeof(c.text) - 1] = 0;
  mproxy::postCommand(c);
  return token;
}

void UITask::sendCurrentMessage() {
  if (!_chat_input) return;
  const char* raw = lv_textarea_get_text(_chat_input);
  if (!raw || !raw[0]) return;

  char encoded[CHAT_MSG_TEXT_MAX + 32];
  encodeMentions(raw, encoded, sizeof(encoded));

  const char* me = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "Me";
  // Async send: the backend does sendMessage/sendGroupMessage + addExpectedAck and
  // replies EV_SendResult with the real ack. The bubble shows immediately.
  postSend(_chat_is_channel, _chat_is_channel ? nullptr : _chat_pubkey,
           _chat_channel_idx, _chat_key, me, encoded);
  lv_textarea_set_text(_chat_input, "");
  rebuildChatHistory();
}

// Runs on the backend thread (called from the_mesh.loop()): just enqueue.
void UITask::msgDelivered(uint32_t ack) {
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::Delivered;
  ev.ack  = ack;
  mproxy::pushEvent(ev);
}

void UITask::resendMessage(const ChatMessage* m) {
  if (!m || _chat_is_channel) return;
  char text[CHAT_MSG_TEXT_MAX];
  strncpy(text, m->text, sizeof(text) - 1);
  text[sizeof(text) - 1] = 0;
  // Re-arm the existing bubble in place with a fresh token (no duplicate); the
  // backend resends and replies EV_SendResult. If it scrolled out, append fresh.
  uint32_t token = ++_send_seq;
  if (token == 0) token = ++_send_seq;
  if (!_msgs->requeue(m, 0, millis() + 8000, token)) {
    const char* me = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "Me";
    storeAppend(true, _chat_key, me, text, mproxy::rtcSeconds(),
                MSG_STATUS_SENDING, 0, millis() + 8000, token);
  }
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::Send;
  c.token = token;
  c.is_channel = false;
  c.channel_idx = -1;
  memcpy(c.pubkey, _chat_pubkey, 6);
  strncpy(c.text, text, sizeof(c.text) - 1);
  c.text[sizeof(c.text) - 1] = 0;
  mproxy::postCommand(c);
  rebuildChatHistory();
}

void UITask::chat_resend_cb(lv_event_t* e) {
  if (!_instance) return;
  const ChatMessage* m = (const ChatMessage*)lv_obj_get_user_data(lv_event_get_target(e));
  if (m) _instance->resendMessage(m);
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
      if (p[2] == 0x8D) { p += 3; continue; }        // ZWJ (emoji sequences) -> drop
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
    } else if (p[0] == 0xEF && p[1] == 0xB8 && (p[2] == 0x8E || p[2] == 0x8F)) {
      p += 3; continue;                              // VS15/VS16 variation selectors -> drop
    } else if (p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x8F && p[3] >= 0xBB && p[3] <= 0xBF) {
      p += 4; continue;                              // emoji skin-tone modifiers -> drop
    }
    out[o++] = *p++;  // copy byte (multi-byte UTF-8 passes through intact)
  }
  out[o] = 0;
}

// Render message text into a bubble, turning "@[username]" mentions into
// "@username" with the name highlighted. Brackets hidden, @ kept. The stored
// text keeps the raw @[username] form so compose can round-trip it later.
// Curated emoji + extended-Latin/punctuation fallback font (Montserrat-matched,
// monochrome), generated by lv_font_conv. Chained onto montserrat_14 so chat text
// renders emoji and accented/"smart" Unicode instead of dropping/boxing them.
extern const lv_font_t meshcore_emoji_14;

// imgfont path callback: map an emoji codepoint to its color image on the SD card
// (S: drive). Returns false for non-emoji so text/accents fall through to the
// baked fonts. A missing image makes lv_imgfont's get_info fail -> chain falls
// through to the monochrome baked emoji. VS16/ZWJ/skin-tones are already stripped
// at the display layer, so we see base codepoints here.
// Is this codepoint one we treat as an emoji (route to a color image)? Text and
// accented Latin return false so they stay on the base / monochrome fonts.
static bool isEmojiCp(uint32_t cp) {
  return (cp >= 0x1F000) ||
         (cp >= 0x2600 && cp <= 0x27BF) ||   // misc symbols + dingbats
         (cp >= 0x2B00 && cp <= 0x2BFF) ||   // stars/arrows
         (cp >= 0x2300 && cp <= 0x23FF) ||   // misc technical (⌚⏰ etc.)
         cp == 0x2122 || cp == 0x203C || cp == 0x2049;
}

// imgfont path callback. The image size is the imgfont's own height (we make one
// imgfont per UI font size), so color emoji are pre-rendered per size on the card
// as /emoji/<size>/<codepoint>.bin -- loaded on demand. A missing file makes
// get_info fail and the chain falls through to the 14px monochrome baked set.
static bool emojiImgPathCb(const lv_font_t* font, void* img_src, uint16_t len,
                           uint32_t unicode, uint32_t unicode_next) {
  (void)unicode_next;
  if (!isEmojiCp(unicode)) return false;
  lv_snprintf((char*)img_src, len, "S:/emoji/%d/%lx.bin",
              (int)font->line_height, (unsigned long)unicode);
  return true;
}

// One color imgfont per pixel size (cached). path_cb reads font->line_height to
// pick the matching /emoji/<size>/ folder.
static lv_font_t* emojiImgFont(int size) {
  static struct { int size; lv_font_t* font; } cache[6];
  static int n = 0;
  for (int i = 0; i < n; i++) if (cache[i].size == size) return cache[i].font;
  if (n >= (int)(sizeof(cache) / sizeof(cache[0]))) return nullptr;
  cache[n].size = size;
  cache[n].font = lv_imgfont_create((uint16_t)size, emojiImgPathCb);
  return cache[n++].font;
}

// Nominal pixel size of a built-in Montserrat font (for matching the emoji size).
static int nominalSize(const lv_font_t* f) {
  if (f == &lv_font_montserrat_12) return 12;
  if (f == &lv_font_montserrat_16) return 16;
  if (f == &lv_font_montserrat_20) return 20;
  if (f == &lv_font_montserrat_28) return 28;
  return 14;
}

// Return a copy of `base` whose glyph fallback is: color imgfont at the base's
// size (SD) -> 14px monochrome baked set -> placeholder. So emoji / extended
// Unicode render anywhere this font is used, color-matched to the text size when
// the SD pack has that size. Cached per base font.
static const lv_font_t* withEmoji(const lv_font_t* base) {
  static struct { const lv_font_t* base; lv_font_t font; } cache[6];
  static int n = 0;
  for (int i = 0; i < n; i++) if (cache[i].base == base) return &cache[i].font;
  if (n >= (int)(sizeof(cache) / sizeof(cache[0]))) return base;
  lv_font_t* img = emojiImgFont(nominalSize(base));
  if (img) img->fallback = (lv_font_t*)&meshcore_emoji_14;  // color -> monochrome
  cache[n].base = base;
  cache[n].font = *base;
  cache[n].font.fallback = img ? img : (lv_font_t*)&meshcore_emoji_14;
  return &cache[n++].font;
}

static const lv_font_t* msgFont() { return withEmoji(&lv_font_montserrat_14); }

static void addMessageText(lv_obj_t* bubble, const char* text) {
  static const uint32_t FG_TEXT = 0xF3F4F6;
  static const uint32_t MENTION = 0x34D399;  // emerald-400; reads on gray + blue bubbles

  lv_obj_t* sg = lv_spangroup_create(bubble);
  lv_obj_set_width(sg, LV_PCT(100));
  lv_obj_set_style_text_font(sg, msgFont(), 0);   // emoji/unicode-capable
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
    lv_style_set_text_font(&span->style, msgFont());  // emoji/unicode in every span (incl. mentions)
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
      int total = mproxy::getNumContacts();
      ContactInfo c;
      for (int ci = 0; ci < total; ci++) {
        if (!mproxy::getContactByIdx(ci, c)) continue;
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
  const ContactInfo* c = mproxy::lookupContactByPubKey(pfx, 4);
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
  lv_obj_set_style_text_font(nm, withEmoji(&lv_font_montserrat_14), 0);

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  char keytrunc[24];
  snprintf(keytrunc, sizeof(keytrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_obj_t* kl = lv_label_create(bubble);
  lv_label_set_text(kl, keytrunc);
  lv_obj_set_style_text_color(kl, lv_color_hex(0x9CA3AF), 0);
  lv_obj_set_style_text_font(kl, &lv_font_montserrat_12, 0);

  // Tap a card for a known contact (either direction) -> open Contact Info.
  if (mproxy::lookupContactByPubKey(pubkey, PUB_KEY_SIZE)) {
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
    if (mproxy::lookupContactByPubKey(pubkey, PUB_KEY_SIZE)) {
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

  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::AddContact;
  ContactInfo& c = cmd.contact;
  memcpy(c.id.pub_key, pk, PUB_KEY_SIZE);
  c.type = type;
  strncpy(c.name, name, sizeof(c.name) - 1);
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = mproxy::rtcSeconds();
  mproxy::postCommand(cmd);   // backend adds it; the snapshot + list refresh on next publish
}

// Format helpers for chat timestamps. Inputs are already UTC + the local offset.
static void fmtClockTime(uint32_t local_secs, char* out, size_t cap, bool h12) {
  time_t t = (time_t)local_secs;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  if (h12) {
    strftime(out, cap, "%I:%M %p", &tmv);
    if (out[0] == '0') memmove(out, out + 1, strlen(out) + 1);  // "02:30 PM" -> "2:30 PM"
  } else {
    strftime(out, cap, "%H:%M", &tmv);
  }
}
static void fmtDateLabel(uint32_t local_secs, long now_day, char* out, size_t cap) {
  long day = (long)local_secs / 86400;
  if (day == now_day)     { strncpy(out, "Today", cap - 1);     out[cap - 1] = 0; return; }
  if (day == now_day - 1) { strncpy(out, "Yesterday", cap - 1); out[cap - 1] = 0; return; }
  time_t t = (time_t)local_secs;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, cap, "%a, %b %d", &tmv);
}

void UITask::rebuildChatHistory() {
  if (!_chat_history) return;
  lv_obj_clean(_chat_history);
  _sending_lbl = NULL;  // old label just got deleted; re-set if a sending msg renders

  const ChatMessage* all[CHAT_HISTORY_CAP];
  int n = _msgs->messagesFor(_chat_key, all, CHAT_HISTORY_CAP);

  bool filtering = _search_active && _search_filter[0];
  const ChatMessage* shown[CHAT_HISTORY_CAP];
  int sn = 0;
  for (int i = 0; i < n; i++) {
    if (filtering && !containsCI(all[i]->text, _search_filter)) continue;
    shown[sn++] = all[i];
  }

  if (sn == 0) {
    lv_obj_t* empty = lv_label_create(_chat_history);
    lv_label_set_text(empty, filtering ? "No matches." : "No messages yet.");
    lv_obj_set_style_text_color(empty, lv_color_hex(DIM_HEX), 0);
    lv_obj_center(empty);
    return;
  }

  int  tzoff = _node_prefs ? _node_prefs->tz_offset_minutes * 60 : 0;
  long now_day = ((long)mproxy::rtcSeconds() + tzoff) / 86400;

  char last_sender[CHAT_PEER_NAME_MAX] = "";
  bool last_outgoing = false;
  long last_day = -999999;
  bool first = true;

  for (int i = 0; i < sn; i++) {
    const ChatMessage* m = shown[i];
    long mday = ((long)m->timestamp + tzoff) / 86400;

    // Date separator (centered) whenever the calendar day changes.
    if (m->timestamp != 0 && mday != last_day) {
      char dl[32];
      fmtDateLabel((uint32_t)((long)m->timestamp + tzoff), now_day, dl, sizeof(dl));
      lv_obj_t* date = lv_label_create(_chat_history);
      lv_label_set_text(date, dl);
      lv_obj_set_width(date, LV_PCT(100));
      lv_obj_set_style_text_align(date, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(date, lv_color_hex(DIM_HEX), 0);
      lv_obj_set_style_text_font(date, &lv_font_montserrat_12, 0);
      lv_obj_set_style_pad_ver(date, 4, 0);
      last_day = mday;
      first = true;  // start a fresh sender run after a date break
    }

    // Incoming bubbles get a sender-name header at the start of a same-sender run.
    bool show_name = !m->outgoing &&
                     (first || last_outgoing ||
                      strncmp(last_sender, m->sender, CHAT_PEER_NAME_MAX) != 0);
    if (show_name) {
      lv_obj_t* name = lv_label_create(_chat_history);
      char sname[CHAT_PEER_NAME_MAX + 4];
      sanitizeForFont(m->sender[0] ? m->sender : "?", sname, sizeof(sname));
      lv_label_set_text(name, sname);
      lv_obj_set_style_text_color(name, lv_color_hex(0x9CA3AF), 0);  // gray-400
      lv_obj_set_style_text_font(name, withEmoji(&lv_font_montserrat_12), 0);
    }

    // Full-width row wrapper so we can left/right-align the bubble inside it.
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
    lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? 0x2563EB : 0x374151), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_align(bubble, m->outgoing ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);

    uint8_t cpk[PUB_KEY_SIZE], ctype;
    char cname[CHAT_PEER_NAME_MAX];
    if (parseContactRef(m->text, cpk, ctype, cname, sizeof(cname))) {
      buildContactCard(bubble, m, cpk, ctype, cname);
    } else {
      addMessageText(bubble, m->text);
      if (m->outgoing && m->status == MSG_STATUS_FAILED) {
        lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);  // tap to resend
        lv_obj_set_user_data(bubble, (void*)m);
        lv_obj_add_event_cb(bubble, chat_resend_cb, LV_EVENT_CLICKED, NULL);
      } else {
        attachMentions(bubble, m->text);
      }
    }

    // Timestamp below the bubble, but collapse it within a burst: hide it when
    // the next message is the same sender, same day, and within 5 minutes.
    bool show_time = (m->timestamp != 0);
    if (show_time && i + 1 < sn) {
      const ChatMessage* nx = shown[i + 1];
      long nday = ((long)nx->timestamp + tzoff) / 86400;
      bool same_sender = (nx->outgoing == m->outgoing) &&
                         strncmp(nx->sender, m->sender, CHAT_PEER_NAME_MAX) == 0;
      if (same_sender && nday == mday && nx->timestamp >= m->timestamp &&
          (nx->timestamp - m->timestamp) <= 300)
        show_time = false;
    }
    // Footer line: outgoing delivery status (sending/failed always; delivered ->
    // a check + time) or just the time, with the burst-collapse applied to time.
    bool sending   = m->outgoing && m->status == MSG_STATUS_SENDING;
    bool failed    = m->outgoing && m->status == MSG_STATUS_FAILED;
    bool delivered = m->outgoing && m->status == MSG_STATUS_DELIVERED;
    if (sending || failed || show_time) {
      char ftxt[40];
      if (sending) {
        strcpy(ftxt, "sending");
      } else if (failed) {
        strcpy(ftxt, LV_SYMBOL_WARNING " failed - tap to resend");
      } else {
        char tbuf[12];
        fmtClockTime((uint32_t)((long)m->timestamp + tzoff), tbuf, sizeof(tbuf),
                     _node_prefs && _node_prefs->clock_12h);
        snprintf(ftxt, sizeof(ftxt), "%s%s", delivered ? LV_SYMBOL_OK " " : "", tbuf);
      }
      lv_obj_t* tl = lv_label_create(_chat_history);
      lv_label_set_text(tl, ftxt);
      lv_obj_set_width(tl, LV_PCT(100));
      lv_obj_set_style_text_align(tl, m->outgoing ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_set_style_text_color(tl, lv_color_hex(failed ? 0xF87171 : DIM_HEX), 0);
      lv_obj_set_style_text_font(tl, &lv_font_montserrat_12, 0);
      if (sending) _sending_lbl = tl;  // animate this one's dots in loop()
    }

    strncpy(last_sender, m->sender, CHAT_PEER_NAME_MAX - 1);
    last_sender[CHAT_PEER_NAME_MAX - 1] = 0;
    last_outgoing = m->outgoing;
    first = false;
  }

  lv_obj_scroll_to_y(_chat_history, LV_COORD_MAX, LV_ANIM_OFF);
  lv_obj_update_layout(_chat_history);
  sb_update(_chat_history, _chat_sb);
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
    lv_obj_set_style_text_font(_chat_title, withEmoji(&lv_font_montserrat_20), 0);
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
    _chat_sb = attachScrollHandle(_chat_history);

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

  char dn[CHAT_PEER_NAME_MAX];
  const char* shown = _chat_is_channel ? _chat_peer
                                       : displayName(_chat_pubkey, _chat_peer, dn, sizeof(dn));
  char tname[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(shown, tname, sizeof(tname));
  lv_label_set_text(_chat_title, tname);

  // Stable per-conversation storage key (pubkey for contacts, channel secret for channels).
  if (_chat_is_channel) {
    ChannelDetails ch;
    if (mproxy::getChannel(_chat_channel_idx, ch)) convKey(ch.channel.secret, true, _chat_key, sizeof(_chat_key));
    else _chat_key[0] = 0;
  } else {
    convKey(_chat_pubkey, false, _chat_key, sizeof(_chat_key));
  }

  rebuildChatHistory();
  lv_scr_load(_chat_screen);
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _sensors = sensors;
  // UI-owned working copy of prefs; edits push CMD_UpdatePrefs and the snapshot
  // refreshes it on change (e.g. BLE companion edits). _node_prefs always points
  // here so existing read/write sites are unchanged.
  if (node_prefs) _node_prefs_store = *node_prefs;
  _node_prefs = &_node_prefs_store;
  _last_snap_version = mproxy::snapshotVersion();
  _instance = this;

#ifdef HAS_SD_CARD
  // Pick the initial store from the setting (default on). The Settings toggle
  // can also switch it live at runtime (see set_history_cb).
  if (!_node_prefs || _node_prefs->persist_history) _msgs = &_sdmsgs;
#endif
  _msgs->begin();  // mounts the SD card (SD store) or no-ops (RAM store)
#ifdef HAS_SD_CARD
  // Seed the RAM ring with recent history so disabling/ejecting the card later
  // keeps recent messages visible without a gap.
  if (_msgs == &_sdmsgs) _sdmsgs.preloadRecent(&_rammsgs, CHAT_HISTORY_CAP);
#endif

  // Restore the saved contacts sort/filter (0xFF = unset -> keep code defaults).
  if (_node_prefs) {
    if (_node_prefs->contacts_order <= 2)  _contacts_order = _node_prefs->contacts_order;
    if (_node_prefs->contacts_filter <= 5) _contacts_filt  = _node_prefs->contacts_filter;
  }

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
#ifdef HAS_SD_CARD
  SdSvc::registerFs();   // 'S:' lv_fs driver for SD-backed images (emoji); needs lv_init first
#endif

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

// Runs on the backend thread: enqueue the new offline-queue count for the UI.
void UITask::msgRead(int msgcount) {
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::MsgCount;
  ev.msgcount = msgcount;
  mproxy::pushEvent(ev);
}

// True if `name` matches a configured channel (vs a contact).
static bool nameIsChannel(const char* name) {
  if (!name) return false;
  for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
    ChannelDetails ch;
    if (mproxy::getChannel(idx, ch) && ch.name[0] &&
        strncmp(ch.name, name, sizeof(ch.name)) == 0) return true;
  }
  return false;
}

// Runs on the backend thread. Cook the message (conv-key, channel split) using
// the backend's hookKey + snapshot, then enqueue; the UI core stores + renders
// it in drainEvents(). No store/LVGL work here.
void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  (void)path_len;

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

  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::Msg;
  ev.outgoing = false;
  convKey(mproxy::hookKey(), mproxy::hookIsChannel(), ev.conv_key, sizeof(ev.conv_key));
  strncpy(ev.sender, sender ? sender : "", sizeof(ev.sender) - 1);
  strncpy(ev.text,   body   ? body   : "", sizeof(ev.text) - 1);
  ev.ts = mproxy::rtcSeconds();
  ev.msgcount = msgcount;
  mproxy::pushEvent(ev);
}

// Backend thread: a message we sent via a *connected companion app* (not the
// on-device compose, which uses CMD_Send). Mirror it into on-device history.
void UITask::sentMsg(const char* peer, const char* text) {
  (void)peer;  // keyed by identity (stashed by MyMesh), not the display name
  const char* me = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "Me";
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::Msg;
  ev.outgoing = true;
  convKey(mproxy::hookKey(), mproxy::hookIsChannel(), ev.conv_key, sizeof(ev.conv_key));
  strncpy(ev.sender, me, sizeof(ev.sender) - 1);
  strncpy(ev.text, text ? text : "", sizeof(ev.text) - 1);
  ev.ts = mproxy::rtcSeconds();
  ev.msgcount = -1;   // unchanged
  mproxy::pushEvent(ev);
}

void UITask::notify(UIEventType t) {
  (void)t;
}

// ===== Contact Info page =================================================

// The Contact-Info page works on a UI-owned copy loaded at openContactInfo
// (edits mutate it optimistically + post a command). NULL if the contact is gone.
ContactInfo* UITask::cinfoContact() {
  return _cinfo_valid ? &_cinfo_c : nullptr;
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
  lv_obj_set_style_text_font(_cinfo_title, withEmoji(&lv_font_montserrat_28), 0);

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
  // Name field: caption row shows "Name" + the real advert name (right-justified,
  // only when a local nickname override is set). The textarea edits the override.
  lv_obj_t* fn = lv_obj_create(_cinfo_body);
  lv_obj_set_width(fn, LV_PCT(100));
  lv_obj_set_height(fn, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(fn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(fn, 0, 0);
  lv_obj_set_style_pad_all(fn, 0, 0);
  lv_obj_set_style_pad_row(fn, 2, 0);
  lv_obj_clear_flag(fn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(fn, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* ncap = lv_obj_create(fn);
  lv_obj_set_width(ncap, LV_PCT(100));
  lv_obj_set_height(ncap, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(ncap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ncap, 0, 0);
  lv_obj_set_style_pad_all(ncap, 0, 0);
  lv_obj_clear_flag(ncap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(ncap, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ncap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* ncaplbl = lv_label_create(ncap);
  lv_label_set_text(ncaplbl, "Name");
  lv_obj_set_style_text_color(ncaplbl, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(ncaplbl, &lv_font_montserrat_12, 0);
  _cinfo_realname = lv_label_create(ncap);
  lv_label_set_text(_cinfo_realname, "");
  lv_obj_set_style_text_color(_cinfo_realname, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_realname, withEmoji(&lv_font_montserrat_12), 0);
  _cinfo_name_ta = lv_textarea_create(fn);
  lv_textarea_set_one_line(_cinfo_name_ta, true);
  lv_obj_set_width(_cinfo_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_cinfo_name_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  // Public key: read-only one-line field (scrolls horizontally) + copy button,
  // matching the Settings page. No keyboard is bound, so it's effectively read-only.
  lv_obj_t* fk = makeField(_cinfo_body, "Public Key");
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
  _cinfo_keyfull = lv_textarea_create(krow);
  lv_textarea_set_one_line(_cinfo_keyfull, true);
  lv_obj_set_flex_grow(_cinfo_keyfull, 1);
  lv_obj_set_style_text_font(_cinfo_keyfull, &lv_font_montserrat_12, 0);
  lv_obj_t* copyk = lv_btn_create(krow);
  lv_obj_add_event_cb(copyk, cinfo_copykey_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* ckl = lv_label_create(copyk);
  lv_label_set_text(ckl, LV_SYMBOL_COPY);
  lv_obj_center(ckl);

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
  _cinfo_type_lbl = lv_label_create(ft);  // read-only: type comes from their advert
  lv_obj_set_style_text_color(_cinfo_type_lbl, lv_color_hex(FG_HEX), 0);

  lv_obj_t* flh = makeField(_cinfo_body, "Last Heard");
  _cinfo_lastheard = lv_label_create(flh);
  lv_obj_set_style_text_color(_cinfo_lastheard, lv_color_hex(FG_HEX), 0);

  lv_obj_t* ftel = makeField(_cinfo_body, "Telemetry");
  _cinfo_telem = lv_label_create(ftel);
  lv_label_set_long_mode(_cinfo_telem, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_cinfo_telem, LV_PCT(100));
  lv_obj_set_style_text_color(_cinfo_telem, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_telem, &lv_font_montserrat_14, 0);

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

  bool overridden = _cinfo_override[0];
  const char* shown = overridden ? _cinfo_override : c->name;
  char nm[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(shown[0] ? shown : "(unnamed)", nm, sizeof(nm));
  lv_label_set_text(_cinfo_title, nm);
  lv_textarea_set_text(_cinfo_name_ta, shown);
  if (_cinfo_realname) {
    if (overridden) {
      char rc[CHAT_PEER_NAME_MAX + 4], rn[CHAT_PEER_NAME_MAX + 8];
      sanitizeForFont(c->name, rc, sizeof(rc));
      snprintf(rn, sizeof(rn), "(%s)", rc);
      lv_label_set_text(_cinfo_realname, rn);
    } else {
      lv_label_set_text(_cinfo_realname, "");
    }
  }

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
  char ktrunc[24];
  snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_label_set_text(_cinfo_key, ktrunc);
  lv_textarea_set_text(_cinfo_keyfull, hex);
  lv_textarea_set_cursor_pos(_cinfo_keyfull, 0);  // show the start, not the tail

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

  static const char* TYPE_NAMES[] = {"Unknown", "Chat", "Repeater", "Room Server", "Sensor"};
  lv_label_set_text(_cinfo_type_lbl, c->type <= 4 ? TYPE_NAMES[c->type] : "Unknown");

  char lh[16];
  formatLastSeen(lh, sizeof(lh), c->lastmod, mproxy::rtcSeconds());  // our clock, not theirs
  lv_label_set_text(_cinfo_lastheard, lh);

  if (_cinfo_telem) {
    bool have = _telem_text[0] && memcmp(_telem_pubkey, c->id.pub_key, 6) == 0;
    lv_label_set_text(_cinfo_telem, have ? _telem_text : "(tap Telem to request)");
  }

  int hashSize = (_node_prefs ? _node_prefs->path_hash_mode : 0) + 1;
  if (hashSize < 1) hashSize = 1;
  // A zero/empty out_path is a direct (0-hop) route -- the phone app stores a
  // full zero-filled path for "direct", which we'd otherwise show as N zero hops.
  bool allzero = (c->out_path_len != OUT_PATH_UNKNOWN);
  for (int i = 0; i < c->out_path_len && c->out_path_len != OUT_PATH_UNKNOWN; i++)
    if (c->out_path[i]) { allzero = false; break; }
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    lv_label_set_text(_cinfo_hops, "Hops Away: flood (unknown)");
    lv_obj_add_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_cinfo_outpath, "Out Path: (flood)");
  } else if (c->out_path_len == 0 || allzero) {
    lv_label_set_text(_cinfo_hops, "Hops Away: direct");
    lv_obj_clear_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);  // allow reset to flood
    lv_label_set_text(_cinfo_outpath, "Out Path: (direct)");
  } else {
    char hb[32];
    snprintf(hb, sizeof(hb), "Hops Away: %d", c->out_path_len / hashSize);
    lv_label_set_text(_cinfo_hops, hb);
    lv_obj_clear_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    char op[3 * MAX_PATH_SIZE + 16];
    int n = snprintf(op, sizeof(op), "Out Path: ");
    for (int i = 0; i < c->out_path_len && n < (int)sizeof(op) - 4; i++) {
      n += snprintf(op + n, sizeof(op) - n, "%02x", c->out_path[i]);
      // comma only on hop boundaries (hashSize bytes per hop): e.g. "0000,0000"
      if ((i % hashSize) == (hashSize - 1) && i + 1 < c->out_path_len)
        n += snprintf(op + n, sizeof(op) - n, ",");
    }
    lv_label_set_text(_cinfo_outpath, op);
  }
}

void UITask::openContactInfo(const uint8_t* pubkey, lv_obj_t* return_screen) {
  memcpy(_cinfo_pubkey, pubkey, PUB_KEY_SIZE);
  // Load a UI-owned working copy from the snapshot (edits are optimistic + posted).
  const ContactInfo* src = mproxy::lookupContactByPubKey(pubkey, PUB_KEY_SIZE);
  _cinfo_valid = (src != nullptr);
  if (src) _cinfo_c = *src;
  _cinfo_override[0] = 0;
  mproxy::getNameOverride(pubkey, _cinfo_override, sizeof(_cinfo_override));
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
    // Edit a local nickname override (not the contact's advert name). Blank or
    // a value equal to the advert name clears the override.
    const char* entered = lv_textarea_get_text(ta);
    bool clear = (!entered[0] || strcmp(entered, c->name) == 0);
    strncpy(_cinfo_override, clear ? "" : entered, sizeof(_cinfo_override) - 1);  // optimistic
    _cinfo_override[sizeof(_cinfo_override) - 1] = 0;
    mproxy::MeshCmd cmd{};
    cmd.kind = mproxy::CmdKind::SetNameOvr;
    memcpy(cmd.pubkey, _cinfo_pubkey, PUB_KEY_SIZE);
    strncpy(cmd.name, clear ? "" : entered, sizeof(cmd.name) - 1);
    cmd.name[sizeof(cmd.name) - 1] = 0;
    mproxy::postCommand(cmd);
    populateContactInfo();   // refresh title + real-name hint (list refreshes on next publish)
  } else if (ta == _cinfo_lat_ta || ta == _cinfo_lon_ta) {
    const char* lats = lv_textarea_get_text(_cinfo_lat_ta);
    const char* lons = lv_textarea_get_text(_cinfo_lon_ta);
    if (lats[0] == 0 && lons[0] == 0) { c->gps_lat = 0; c->gps_lon = 0; }
    else { c->gps_lat = (int32_t)lround(atof(lats) * 1e6); c->gps_lon = (int32_t)lround(atof(lons) * 1e6); }
    mproxy::MeshCmd cmd{};
    cmd.kind = mproxy::CmdKind::SaveGps;
    memcpy(cmd.pubkey, _cinfo_pubkey, PUB_KEY_SIZE);
    cmd.gps_lat = c->gps_lat; cmd.gps_lon = c->gps_lon;
    mproxy::postCommand(cmd);
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
  c->flags ^= CONTACT_FLAG_FAVOURITE;   // optimistic; backend applies + persists
  postPubkeyCmd(mproxy::CmdKind::ToggleFav, _instance->_cinfo_pubkey);
  _instance->populateContactInfo();
}

void UITask::cinfo_telemetry_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  // Backend's requestTelemetry records the pending tag so the reply is matched
  // and routed back to telemetryResponse() (a bare request wouldn't be matched).
  postPubkeyCmd(mproxy::CmdKind::ReqTelem, _instance->_cinfo_pubkey);
  _instance->showToast("Telemetry requested...");
}

void UITask::cinfo_clearpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  c->out_path_len = OUT_PATH_UNKNOWN;   // optimistic
  postPubkeyCmd(mproxy::CmdKind::ResetPath, _instance->_cinfo_pubkey);
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
    _instance->raiseFieldForKb(_instance->_cinfo_body, _instance->_cinfo_kb, ta);
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
    _instance->resetKbScroll();
  }
}

void UITask::cinfo_copykey_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
  _instance->copyToClipboard(hex);
  _instance->showToast("Public key copied");
}

void UITask::cinfo_name_clicked_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || _instance->_chat_is_channel) return;
  const ContactInfo* c = mproxy::lookupContactByPubKey(_instance->_chat_pubkey, 6);
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
  if (!_instance) return;
  _instance->closeMenuPopup();
  _instance->openContactPicker(1);  // pick recipient -> send the viewed contact's card
}

void UITask::share_zerohop_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  ContactInfo* c = _instance->cinfoContact();
  if (c) postPubkeyCmd(mproxy::CmdKind::ShareZhop, _instance->_cinfo_pubkey);
  _instance->closeMenuPopup();
  _instance->showToast("Shared (zero-hop)");
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
  lv_obj_set_style_text_font(_qr_name_lbl, withEmoji(&lv_font_montserrat_28), 0);

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
  const ContactInfo* c = mproxy::lookupContactByPubKey(_instance->_chat_pubkey, 6);
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
  const ContactInfo* c = mproxy::lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (!c) { _instance->closeMenuPopup(); return; }
  memcpy(_instance->_cinfo_pubkey, c->id.pub_key, PUB_KEY_SIZE);
  _instance->closeMenuPopup();
  // showShareMenu rebuilds _menu_list (deleting this kebab button); defer it.
  lv_async_call([](void*) { if (_instance) _instance->showShareMenu(); }, NULL);
}

void UITask::kebab_setpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  const ContactInfo* c = mproxy::lookupContactByPubKey(_instance->_chat_pubkey, 6);
  if (!c) { _instance->closeMenuPopup(); return; }
  memcpy(_instance->_cinfo_pubkey, c->id.pub_key, PUB_KEY_SIZE);
  _instance->closeMenuPopup();
  _instance->openPathEditor(_instance->_chat_screen);
}

void UITask::kebab_resetpath_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  postPubkeyCmd(mproxy::CmdKind::ResetPath, _instance->_chat_pubkey);
  _instance->showToast("Path reset to flood");
}

void UITask::kebab_clearhistory_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  _instance->_msgs->clearPeer(_instance->_chat_key);
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
  lv_dropdown_set_selected(_path_size_dd, _node_prefs ? _node_prefs->path_hash_mode : 0);
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
  if (empty) {
    c->out_path_len = OUT_PATH_UNKNOWN;   // optimistic
    postPubkeyCmd(mproxy::CmdKind::ResetPath, _cinfo_pubkey);
    return true;
  }
  uint8_t buf[MAX_PATH_SIZE];
  int n = parseHexCsv(txt, buf, MAX_PATH_SIZE);
  if (n < 0) { lv_label_set_text(_path_err, "Bad path (use aa,bb,cc)"); lv_obj_clear_flag(_path_err, LV_OBJ_FLAG_HIDDEN); return false; }
  memcpy(c->out_path, buf, n);
  c->out_path_len = (uint8_t)n;          // optimistic
  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::SetPath;
  memcpy(cmd.pubkey, _cinfo_pubkey, PUB_KEY_SIZE);
  memcpy(cmd.path, buf, n);
  cmd.path_len = (uint8_t)n;
  mproxy::postCommand(cmd);
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
    lv_obj_t* ta = lv_event_get_target(e);
    lv_keyboard_set_textarea(_instance->_path_kb, ta);
    lv_obj_clear_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_path_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_path_kb);
    // ta -> field column -> scrollable body
    _instance->raiseFieldForKb(lv_obj_get_parent(lv_obj_get_parent(ta)), _instance->_path_kb, ta);
  }
}

void UITask::path_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(_instance->_path_kb, LV_OBJ_FLAG_HIDDEN);
    _instance->resetKbScroll();
  }
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

  // Local-time offset for the header clock. Entered in hours (decimals OK for
  // half/quarter-hour zones, e.g. 5.5, 5.75, -3.5); stored as minutes.
  _set_tz_ta = makeNumberField(body, "UTC offset (hours)", set_tz_ta_event_cb);

  _set_clock_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_clock_chk, "12-hour clock");
  lv_obj_set_style_text_color(_set_clock_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_clock_chk, set_clock_cb, LV_EVENT_VALUE_CHANGED, NULL);

#ifdef HAS_SD_CARD
  _set_history_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_history_chk, "Save chat history");
  lv_obj_set_style_text_color(_set_history_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_history_chk, set_history_cb, LV_EVENT_VALUE_CHANGED, NULL);
#endif

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
    mesh::Utils::toHex(hex, mproxy::selfPubKey(), PUB_KEY_SIZE);
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

  if (_set_tz_ta) {
    char tb[16];
    snprintf(tb, sizeof(tb), "%g", _node_prefs->tz_offset_minutes / 60.0);
    lv_textarea_set_text(_set_tz_ta, tb);
  }
  if (_set_clock_chk) {
    if (_node_prefs->clock_12h) lv_obj_add_state(_set_clock_chk, LV_STATE_CHECKED);
    else                        lv_obj_clear_state(_set_clock_chk, LV_STATE_CHECKED);
  }
  if (_set_history_chk) {
    if (_node_prefs->persist_history != 0) lv_obj_add_state(_set_history_chk, LV_STATE_CHECKED);  // 0xFF/1 = on
    else                                   lv_obj_clear_state(_set_history_chk, LV_STATE_CHECKED);
  }
}

void UITask::commitNodeName() {
  if (!_set_name_ta || !_node_prefs) return;
  const char* nm = lv_textarea_get_text(_set_name_ta);
  if (!nm || !nm[0]) return;
  if (strncmp(nm, _node_prefs->node_name, sizeof(_node_prefs->node_name)) == 0) return;  // unchanged
  strncpy(_node_prefs->node_name, nm, sizeof(_node_prefs->node_name) - 1);
  _node_prefs->node_name[sizeof(_node_prefs->node_name) - 1] = 0;
  pushPrefs();
  pushAdvert();  // re-broadcast identity with the new name
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
  pushRadio();   // backend applies radio params to the SX1262 + persists (CMD_ApplyRadio)
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
    _instance->raiseFieldForKb(_instance->_tab_settings, _instance->_set_kb, ta);
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
    _instance->raiseFieldForKb(_instance->_tab_settings, _instance->_set_kb, ta);
  }
}

void UITask::set_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (_instance->_set_active_ta == _instance->_set_name_ta) _instance->commitNodeName();
    else if (_instance->_set_active_ta == _instance->_set_lat_ta ||
             _instance->_set_active_ta == _instance->_set_lon_ta) _instance->commitPosition();
    else if (_instance->_set_active_ta == _instance->_set_tz_ta) _instance->commitTz();
    _instance->_set_active_ta = NULL;
    lv_obj_add_flag(_instance->_set_kb, LV_OBJ_FLAG_HIDDEN);
    _instance->resetKbScroll();
  }
}

void UITask::set_radio_apply_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->applyRadioSettings();
}

void UITask::set_pathmode_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->path_hash_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
  pushPrefs();
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
      pushPrefs();
    }
  }
}

void UITask::set_rot_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));  // 0..3
  _instance->_node_prefs->display_rotation = (uint8_t)(sel + 1);    // 1-based; 0 = unset
  pushPrefs();
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
  mesh::Utils::toHex(hex, mproxy::selfPubKey(), PUB_KEY_SIZE);
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
  pushPrefs();
}

void UITask::commitTz() {
  if (!_set_tz_ta || !_node_prefs) return;
  long m = lround(atof(lv_textarea_get_text(_set_tz_ta)) * 60.0);  // hours -> minutes
  if (m < -720) m = -720;   // UTC-12:00
  if (m > 840) m = 840;     // UTC+14:00
  _node_prefs->tz_offset_minutes = (int16_t)m;
  pushPrefs();
}

void UITask::set_clock_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->clock_12h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
  pushPrefs();
}

void UITask::set_history_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  _instance->_node_prefs->persist_history = on ? 1 : 0;
  pushPrefs();
#ifdef HAS_SD_CARD
  // Apply live: swap the active store, mounting/unmounting the card. The RAM ring
  // keeps being fed either way (storeAppend), so the recent view never blanks; on
  // re-enable we backfill the card with anything that arrived while saving was off.
  if (on) {
    _instance->_sdmsgs.begin();             // (re)mount + reload from card
    if (_instance->_sd_off_ts)
      _instance->_rammsgs.replayInto(&_instance->_sdmsgs, _instance->_sd_off_ts);
    _instance->_sd_off_ts = 0;
    _instance->_msgs = &_instance->_sdmsgs;
  } else {
    _instance->_sd_off_ts = mproxy::rtcSeconds();  // mark for backfill
    _instance->_msgs = &_instance->_rammsgs;
    _instance->_sdmsgs.end();               // unmount so the card can be pulled
  }
  // Refresh whatever's on screen so it reflects the now-active store.
  if (_instance->_chat_screen && lv_scr_act() == _instance->_chat_screen)
    _instance->rebuildChatHistory();
  _instance->_contacts_dirty = true;        // "latest" sort reads the store
#endif
}

void UITask::set_tz_ta_event_cb(lv_event_t* e) {
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
    _instance->raiseFieldForKb(_instance->_tab_settings, _instance->_set_kb, ta);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitTz();
  }
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
    _instance->raiseFieldForKb(_instance->_tab_settings, _instance->_set_kb, ta);
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
    pushPrefs();
    pushAdvert();
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
  pushPrefs();
  pushAdvert();
  if (_instance->_confirm_popup) lv_obj_add_flag(_instance->_confirm_popup, LV_OBJ_FLAG_HIDDEN);
  _instance->showToast("Position sharing on");
}

// ----- Reusable info modal (telemetry now; repeater status/CLI later) --------
void UITask::info_close_cb(lv_event_t* e) {
  (void)e;
  if (_instance && _instance->_info_popup) lv_obj_add_flag(_instance->_info_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::showInfoPopup(const char* title, const char* body) {
  if (!_info_popup) {
    _info_popup = lv_obj_create(lv_layer_top());
    lv_obj_set_size(_info_popup, _screen_w, _screen_h);
    lv_obj_set_pos(_info_popup, 0, 0);
    lv_obj_set_style_bg_color(_info_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_info_popup, LV_OPA_50, 0);
    lv_obj_set_style_border_width(_info_popup, 0, 0);
    lv_obj_set_style_pad_all(_info_popup, 0, 0);
    lv_obj_add_flag(_info_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_info_popup, [](lv_event_t* ev) {  // tap backdrop closes
      if (_instance && lv_event_get_target(ev) == _instance->_info_popup)
        lv_obj_add_flag(_instance->_info_popup, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* card = lv_obj_create(_info_popup);
    lv_obj_set_width(card, LV_PCT(86));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, LV_PCT(80), 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    _info_title_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(_info_title_lbl, lv_color_hex(0x60A5FA), 0);
    lv_obj_set_style_text_font(_info_title_lbl, &lv_font_montserrat_16, 0);

    _info_body_lbl = lv_label_create(card);
    lv_label_set_long_mode(_info_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_info_body_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_info_body_lbl, lv_color_hex(0xF3F4F6), 0);

    lv_obj_t* ok = lv_btn_create(card);
    lv_obj_add_event_cb(ok, info_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* okl = lv_label_create(ok);
    lv_label_set_text(okl, "Close");
  }
  lv_label_set_text(_info_title_lbl, title);
  lv_label_set_text(_info_body_lbl, body);
  lv_obj_clear_flag(_info_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_info_popup);
}

// CayenneLPP type codes we decode for telemetry display (subset of the standard).
enum {
  LPP_T_ANALOG_INPUT = 2, LPP_T_LUMINOSITY = 101, LPP_T_TEMPERATURE = 103,
  LPP_T_HUMIDITY = 104, LPP_T_BAROMETER = 115, LPP_T_VOLTAGE = 116,
  LPP_T_CURRENT = 117, LPP_T_PERCENTAGE = 120, LPP_T_GPS = 136
};

void UITask::telemetryResponse(const uint8_t* pubkey, const char* from_name,
                               const uint8_t* lpp, uint8_t lpp_len) {
  (void)from_name;
  char text[160];
  size_t o = 0;
  bool have_gps = false;
  int32_t glat6 = 0, glon6 = 0;
  int i = 0;
  while (i + 2 <= lpp_len && o + 1 < sizeof(text)) {
    uint8_t ch = lpp[i++];
    uint8_t type = lpp[i++];
    char line[48];
    line[0] = 0;
    switch (type) {
      case LPP_T_VOLTAGE:
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Battery: %.2f V", ((lpp[i] << 8) | lpp[i + 1]) / 100.0);
        i += 2; break;
      case LPP_T_TEMPERATURE: {
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        int16_t v = (int16_t)((lpp[i] << 8) | lpp[i + 1]);
        snprintf(line, sizeof(line), "Temp: %.1f C", v / 10.0);
        i += 2; break; }
      case LPP_T_HUMIDITY:
        if (i + 1 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Humidity: %.1f %%", lpp[i] / 2.0);
        i += 1; break;
      case LPP_T_BAROMETER:
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Pressure: %.1f hPa", ((lpp[i] << 8) | lpp[i + 1]) / 10.0);
        i += 2; break;
      case LPP_T_ANALOG_INPUT: {
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        int16_t v = (int16_t)((lpp[i] << 8) | lpp[i + 1]);
        snprintf(line, sizeof(line), "Analog %u: %.2f", ch, v / 100.0);
        i += 2; break; }
      case LPP_T_LUMINOSITY:
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Light: %u lux", (unsigned)((lpp[i] << 8) | lpp[i + 1]));
        i += 2; break;
      case LPP_T_CURRENT:
        if (i + 2 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Current: %.3f A", ((lpp[i] << 8) | lpp[i + 1]) / 1000.0);
        i += 2; break;
      case LPP_T_PERCENTAGE:
        if (i + 1 > lpp_len) { i = lpp_len; break; }
        snprintf(line, sizeof(line), "Level: %u%%", lpp[i]);
        i += 1; break;
      case LPP_T_GPS: {  // applied to the contact's position, not the readings line
        if (i + 9 > lpp_len) { i = lpp_len; break; }
        int32_t lat = (lpp[i] << 16) | (lpp[i + 1] << 8) | lpp[i + 2];
        int32_t lon = (lpp[i + 3] << 16) | (lpp[i + 4] << 8) | lpp[i + 5];
        if (lat & 0x800000) lat |= 0xFF000000;
        if (lon & 0x800000) lon |= 0xFF000000;
        glat6 = lat * 100;  // LPP 1e-4 deg -> contact stores 1e-6 deg
        glon6 = lon * 100;
        have_gps = true;
        i += 9; break; }
      default:
        i = lpp_len;  // unknown type: size unknown, stop decoding
        break;
    }
    if (line[0]) o += snprintf(text + o, sizeof(text) - o, "%s%s", o ? ", " : "", line);
  }

  // This runs on the backend thread. The GPS write goes out as a command; the
  // readings go to the UI core as an event (no LVGL here). See drainEvents().
  if (have_gps && (glat6 || glon6)) {
    mproxy::MeshCmd cmd{};
    cmd.kind = mproxy::CmdKind::SaveGps;
    memcpy(cmd.pubkey, pubkey, 6);
    cmd.gps_lat = glat6; cmd.gps_lon = glon6;
    mproxy::postCommand(cmd);
  }
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::Telem;
  memcpy(ev.pubkey, pubkey, 6);
  strncpy(ev.telem_text, o ? text : "(no readings)", sizeof(ev.telem_text) - 1);
  ev.telem_text[sizeof(ev.telem_text) - 1] = 0;
  mproxy::pushEvent(ev);
}

void UITask::loop() {
  if (!_started) return;
  uint32_t now = millis();
  uint32_t delta = now - _last_tick_ms;
  if (delta > 0) {
    lv_tick_inc(delta);
    _last_tick_ms = now;
  }

  // Pin the published snapshot for this UI pass, then apply backend events
  // (new/sent msgs, delivery, telemetry) to the store/LVGL on this (UI) core.
  mproxy::beginUiRead();
  drainEvents();

  // Live header clock (1 Hz). rtcSeconds() is the internal RTC -- no bus traffic.
  if (_clock_label) {
    uint32_t secs = mproxy::rtcSeconds();
    if (secs != _clock_last) {
      _clock_last = secs;
      time_t t = (time_t)secs + (_node_prefs ? _node_prefs->tz_offset_minutes * 60 : 0);
      struct tm tmv;
      gmtime_r(&t, &tmv);  // gmtime of (UTC + offset) = local wall time
      char buf[28];
      strftime(buf, sizeof(buf),
               (_node_prefs && _node_prefs->clock_12h) ? "%m-%d %I:%M:%S %p" : "%m-%d %H:%M:%S",
               &tmv);
      lv_label_set_text(_clock_label, buf);
    }
  }

#ifdef HAS_SD_CARD
  // Surface SD persistence problems instead of silently dropping history.
  static bool sd_warned = false;
  static uint32_t sd_err_toast_ms = 0;
  if (!sd_warned && now > 4000) { sd_warned = true; if (!_msgs->ready()) showToast("No SD card - history won't be saved"); }
  if (_msgs->takeWriteError() && now - sd_err_toast_ms > 15000) {  // throttle: don't spam per message
    sd_err_toast_ms = now;
    showToast("SD write failed - message not saved");
  }
#endif

  // Outgoing send-status: fail any in-flight message past its deadline, then
  // animate the "sending" footer of the most recent in-flight message.
  if (_msgs->expireSending(now, MSG_STATUS_FAILED) && _chat_screen) rebuildChatHistory();
  if (_sending_lbl && now - _anim_ms >= 400) {
    _anim_ms = now;
    _dot_frame = (_dot_frame + 1) & 3;
    static const char* dots[] = {"sending", "sending.", "sending..", "sending..."};
    lv_label_set_text(_sending_lbl, dots[_dot_frame]);
  }

  // Rebuild the contacts/channels lists when the backend publishes a new snapshot
  // (contact add/remove/rename/favourite, or prefs changed over BLE/serial).
  // A snapshot change (contact add/remove/rename/favourite, prefs, channels) just
  // MARKS the lists pending -- it does not rebuild here. rebuildContactsList is
  // heavy (filter + sort + table cells, copying contacts out of PSRAM); rebuilding
  // an off-screen list is exactly what hitches input while you're elsewhere.
  uint32_t sv = mproxy::snapshotVersion();
  if (sv != _last_snap_version) {
    _last_snap_version = sv;
    if (const NodePrefs* p = mproxy::prefsSnap()) *_node_prefs = *p;  // pick up backend-side pref edits
    _contacts_pending = true;
    _channels_pending = true;
  }

  // Service a pending rebuild only when that list's tab is actually on screen
  // (home screen + that tab active), lightly throttled so churn-while-viewing
  // doesn't jank the scroll. Switching to a tab services it within one frame.
  // Tabs: 0 = Contacts, 1 = Channels, 2 = Settings. _contacts_dirty is the
  // message-store "latest" re-sort (independent of the snapshot).
  if (lv_scr_act() == _home_screen && _tabview) {
    int tab = (int)lv_tabview_get_tab_act(_tabview);
    if (tab == 0 && (_contacts_pending || _contacts_dirty) && now - _contacts_rebuilt_ms >= 800) {
      _contacts_pending = false;
      rebuildContactsList();   // also clears _contacts_dirty + sets _contacts_rebuilt_ms
    } else if (tab == 1 && _channels_pending) {
      _channels_pending = false;
      rebuildChannelsList();
    }
  }

  lv_timer_handler();
  mproxy::endUiRead();
}

// Drain the backend->UI event queue and apply each event on the UI core.
void UITask::drainEvents() {
  mproxy::UiEvent ev;
  while (mproxy::pollEvent(ev)) {
    switch (ev.kind) {
      case mproxy::EvKind::Msg: {
        if (ev.msgcount >= 0) _msgcount = ev.msgcount;
        storeAppend(ev.outgoing, ev.conv_key, ev.sender, ev.text, ev.ts);
        if (_chat_screen && strncmp(ev.conv_key, _chat_key, CHAT_PEER_NAME_MAX) == 0)
          rebuildChatHistory();
        _contacts_dirty = true;   // "latest message" sort may have changed
        break;
      }
      case mproxy::EvKind::SendResult: {
        // Fill the optimistic bubble's real ack/expiry, or fail it.
        uint8_t status = ev.ok ? (ev.ack ? MSG_STATUS_SENDING : MSG_STATUS_NONE) : MSG_STATUS_FAILED;
        uint32_t expiry = (ev.ok && ev.ack) ? millis() + ev.timeout + 2000 : 0;
        _msgs->setByClientToken(ev.token, status, ev.ack, expiry);
        if (!ev.ok) showToast("Send failed");
        if (_chat_screen) rebuildChatHistory();
        break;
      }
      case mproxy::EvKind::Delivered: {
        if (_msgs->setStatusByAck(ev.ack, MSG_STATUS_DELIVERED) && _chat_screen)
          rebuildChatHistory();
        break;
      }
      case mproxy::EvKind::Telem: {
        memcpy(_telem_pubkey, ev.pubkey, 6);
        strncpy(_telem_text, ev.telem_text, sizeof(_telem_text) - 1);
        _telem_text[sizeof(_telem_text) - 1] = 0;
        if (_cinfo_screen && lv_scr_act() == _cinfo_screen && memcmp(_cinfo_pubkey, ev.pubkey, 6) == 0)
          populateContactInfo();
        showToast("Telemetry received");
        break;
      }
      case mproxy::EvKind::MsgCount:
        _msgcount = ev.msgcount;
        break;
    }
  }
}
