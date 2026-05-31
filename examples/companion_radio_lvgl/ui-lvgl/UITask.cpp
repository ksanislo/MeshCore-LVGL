#include "UITask.h"
#include "meshcore_assets.h"
#include "MeshProxy.h"                  // UI talks to the backend only through this
#include "ui_theme.h"                   // UI kit: color/layout tokens, avatar palette
#include <helpers/AdvertDataHelpers.h>  // ADV_TYPE_*
#include <helpers/ContactInfo.h>        // ContactInfo (read-model copies)
#include <Utils.h>                      // mesh::Utils::toHex
#include <esp_heap_caps.h>
#include <esp_system.h>                 // esp_restart() for the Reboot button
#include <esp_core_dump.h>              // boot-time crash report (coredump-to-flash is on)
#include "SdCard.h"                     // SdSvc + the shared `sd` handle, to save crash reports
#include <SPIFFS.h>                     // internal fallback for the crash report
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
static void firstGrapheme(const char* in, char* out, size_t cap);  // first UTF-8 codepoint (avatar glyph)
static const lv_font_t* withEmoji(const lv_font_t* base);  // base font + emoji/unicode fallback

// Type ramp: one place that maps a text role to a size (all emoji/unicode-capable).
// hero=page name, title=screen header, heading=section/dialog title, body=default,
// caption=metadata/secondary. Prefer these over naming a montserrat size directly.
static const lv_font_t* fontHero();
static const lv_font_t* fontTitle();
static const lv_font_t* fontHeading();
static const lv_font_t* fontBody();
static const lv_font_t* fontCaption();

UITask* UITask::_instance = NULL;

// Active avatar-color scheme (0 = curated, 1 = iOS parity); declared in ui_theme.h
// so nameColor() can dispatch. Seeded from NodePrefs in begin(), updated live by
// the Settings "Avatar colors" dropdown.
uint8_t g_avatar_palette_mode = 0;

// Active chrome palette (declared in ui_theme.h; UI_* tokens read from it). Starts
// as the built-in dark theme; the Settings "Theme" picker swaps it at runtime.
UiPalette g_ui_palette = UI_THEME_DARK;

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
    _instance->_last_input_ms = millis();   // reset the idle-off timer
    if (_instance->_display_off) {
      // Wake the backlight and swallow the ENTIRE wake gesture (until the finger
      // lifts) so it doesn't also fire a button -- the tap that wakes the screen
      // shouldn't act on whatever's under it.
      board_set_backlight(_instance->_backlight_duty);
      _instance->_display_off = false;
      _instance->_swallow_touch = true;
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    if (_instance->_swallow_touch) {        // still the same wake press -> keep swallowing
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    data->state = LV_INDEV_STATE_PR;
    data->point.x = tx;
    data->point.y = ty;
  } else {
    _instance->_swallow_touch = false;      // finger lifted -> end the wake-swallow latch
    data->state = LV_INDEV_STATE_REL;
  }
}

// Tap on a screen background (outside any text field / keyboard) -> hide the open
// keyboard. Attached to each screen's scrollable body; a tap on a field/button is
// handled by that widget and doesn't reach here (events don't bubble by default).
void UITask::dismiss_kb_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  UITask* s = _instance;
  if (s->_chat_keyboard && lv_scr_act() == s->_chat_screen) s->layoutChatBody(false);  // also restores chat layout
  lv_obj_t* kbs[] = { s->_set_kb, s->_cinfo_kb, s->_path_kb, s->_newchan_kb, s->_login_kb,
                      s->_contacts_kb, s->_pick_kb, s->_pinset_kb, s->_lock_kb, s->_profile_kb };
  for (lv_obj_t* kb : kbs) if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

static void styleAsDarkScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  // text_font is inherited, so this gives every default-font field on the screen
  // (compose box, name/nickname inputs, channel list, search, etc.) emoji/unicode
  // support. Widgets with an explicit larger font wrap it via withEmoji() too.
  lv_obj_set_style_text_font(scr, fontBody(), 0);
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
// Every editable field is created through this so it uniformly supports text
// selection (native drag-select) + a long-press Cut/Copy/Paste toolbar. Replaces
// the bare lv_textarea_create() at all field sites. (The fn-pointer indirection
// keeps this body out of that global rename.)
lv_obj_t* UITask::makeSelTextarea(lv_obj_t* parent) {
  lv_obj_t* (*mk)(lv_obj_t*) = lv_textarea_create;
  lv_obj_t* ta = mk(parent);
  lv_textarea_set_text_selection(ta, true);
  lv_obj_add_event_cb(ta, UITask::ta_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
  return ta;
}

static lv_obj_t* attachScrollHandle(lv_obj_t* content) {
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);   // hide the draw-only native bar
  lv_obj_t* thumb = lv_obj_create(lv_obj_get_parent(content));
  lv_obj_add_flag(thumb, LV_OBJ_FLAG_FLOATING);               // ignored by parent layout
  lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(thumb, 10, 24);
  lv_obj_set_style_radius(thumb, 5, 0);
  lv_obj_set_style_bg_color(thumb, lv_color_hex(DIM_HEX), 0);
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

  // Resolution-independent wordmark: one high-res alpha master (600x67) scaled to
  // ~62% of the screen width via zoom, tinted with the recolorable UI_LOGO token.
  // Pivot = source center so the zoom pins the same point LV_ALIGN_CENTER anchors.
  lv_obj_t* wordmark = lv_img_create(scr);
  lv_img_set_src(wordmark, &meshcore_logo_alpha);
  int target_w = (lv_disp_get_hor_res(NULL) * 80) / 100;
  uint16_t zoom = (uint16_t)((256 * target_w) / meshcore_logo_alpha.header.w);
  lv_img_set_zoom(wordmark, zoom);
  lv_img_set_pivot(wordmark, meshcore_logo_alpha.header.w / 2,
                   meshcore_logo_alpha.header.h / 2);
  lv_obj_set_style_img_recolor(wordmark, lv_color_hex(UI_LOGO), 0);
  lv_obj_set_style_img_recolor_opa(wordmark, LV_OPA_COVER, 0);
  lv_obj_align(wordmark, LV_ALIGN_CENTER, 0, 0);

  return scr;
}

static constexpr int HEADER_H     = 48;
static constexpr int TABBAR_H     = 56;
static constexpr int COMPOSE_H    = 50;
static constexpr int SEARCH_BAR_H = 44;

// Standard 36px square back-target (whole corner is tappable), arrow centered.
lv_obj_t* UITask::makeBackButton(lv_obj_t* bar, lv_event_cb_t cb) {
  lv_obj_t* back = lv_btn_create(bar);
  lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_set_style_pad_all(back, 0, 0);
  lv_obj_set_size(back, 24, 36);   // ~1/3 narrower than the avatar; full-height tap target
  lv_obj_add_event_cb(back, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(FG_HEX), 0);
  lv_obj_center(bl);
  return back;
}

// Standard screen header: fixed UI_SURFACE bar, flex row of [back][title(grows)].
// Callers append right-side actions as further flex children (they sit to the right
// of the grown title).
lv_obj_t* UITask::makeHeaderBar(lv_obj_t* parent, const char* title, lv_event_cb_t back_cb) {
  lv_obj_t* bar = lv_obj_create(parent);
  lv_obj_set_size(bar, _screen_w, HEADER_H);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 6, 0);
  lv_obj_set_style_pad_ver(bar, 4, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  makeBackButton(bar, back_cb);
  if (title) {
    lv_obj_t* bt = lv_label_create(bar);
    lv_obj_set_flex_grow(bt, 1);
    lv_label_set_long_mode(bt, LV_LABEL_LONG_DOT);
    lv_label_set_text(bt, title);
    lv_obj_set_style_text_color(bt, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_text_font(bt, fontTitle(), 0);
  }
  return bar;
}

// Dim full-screen overlay on the top layer. Shared by every popup so the scrim and
// tap-to-dismiss behave identically. tap_cb (optional) fires on a backdrop tap.
lv_obj_t* UITask::makeBackdrop(lv_event_cb_t tap_cb) {
  lv_obj_t* bd = lv_obj_create(lv_layer_top());
  lv_obj_set_size(bd, _screen_w, _screen_h);
  lv_obj_set_pos(bd, 0, 0);
  lv_obj_set_style_bg_color(bd, lv_color_hex(UI_SCRIM), 0);
  lv_obj_set_style_bg_opa(bd, LV_OPA_60, 0);
  lv_obj_set_style_border_width(bd, 0, 0);
  lv_obj_set_style_pad_all(bd, 0, 0);
  lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);   // swallow taps behind the popup
  if (tap_cb) lv_obj_add_event_cb(bd, tap_cb, LV_EVENT_CLICKED, NULL);
  return bd;
}

lv_obj_t* UITask::makeModalCard(lv_obj_t** backdrop_out, lv_event_cb_t backdrop_tap_cb) {
  lv_obj_t* bd = makeBackdrop(backdrop_tap_cb);
  lv_obj_t* card = lv_obj_create(bd);
  lv_obj_set_width(card, LV_PCT(88));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_max_height(card, LV_PCT(85), 0);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 14, 0);
  lv_obj_set_style_pad_row(card, 10, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // consume taps so backdrop dismiss doesn't fire

  if (backdrop_out) *backdrop_out = bd;
  return card;
}

lv_obj_t* UITask::buildHomeScreen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  styleAsDarkScreen(scr);
  lv_obj_set_style_pad_all(scr, 0, 0);

  // ----- header strip (logo + clock; hidden while a settings pane is open) -----
  lv_obj_t* header = lv_obj_create(scr);
  _header = header;
  lv_obj_set_size(header, _screen_w, HEADER_H);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(UI_SURFACE), 0);  // gray-800
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_set_style_radius(header, 0, 0);
  lv_obj_set_style_pad_all(header, 8, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  // MeshCore wordmark, scaled down from the same alpha master as the splash and
  // tinted with UI_LOGO. Pivot = left-mid so the zoom pins the LV_ALIGN_LEFT_MID
  // anchor. ~16px tall: a compact wordmark, deliberately smaller than the splash.
  _header_logo = lv_img_create(header);
  lv_img_set_src(_header_logo, &meshcore_logo_alpha);
  uint16_t hzoom = (uint16_t)((256 * 16) / meshcore_logo_alpha.header.h);
  lv_img_set_zoom(_header_logo, hzoom);
  lv_img_set_pivot(_header_logo, 0, meshcore_logo_alpha.header.h / 2);
  lv_obj_set_style_img_recolor(_header_logo, lv_color_hex(UI_LOGO), 0);
  lv_obj_set_style_img_recolor_opa(_header_logo, LV_OPA_COVER, 0);
  lv_obj_align(_header_logo, LV_ALIGN_LEFT_MID, 4, 0);

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
  // Entering the Settings tab always returns to its category launcher.
  lv_obj_add_event_cb(_tabview, settings_tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // tab bar styling
  lv_obj_t* tab_btns = lv_tabview_get_tab_btns(_tabview);
  lv_obj_set_style_bg_color(tab_btns, lv_color_hex(UI_SURFACE), 0);
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

  // ----- Contacts tab: fixed search/filter band + the virtualized ContactListView -----
  lv_obj_set_style_pad_all(_tab_contacts, 0, 0);
  lv_obj_clear_flag(_tab_contacts, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(_tab_contacts, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(_tab_contacts, 0, 0);   // no gap between the search band and the list
  lv_obj_add_event_cb(_tab_contacts, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb

  // Search row: text field (grows) + a filter/menu button on the right that
  // opens the order/filter pop-out.
  lv_obj_t* cctl = lv_obj_create(_tab_contacts);
  lv_obj_set_width(cctl, LV_PCT(100));
  lv_obj_set_height(cctl, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(cctl, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(cctl, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cctl, 0, 0);
  lv_obj_set_style_radius(cctl, 0, 0);
  lv_obj_set_style_pad_hor(cctl, 6, 0);
  lv_obj_set_style_pad_ver(cctl, 4, 0);   // slim band
  lv_obj_set_style_pad_column(cctl, 6, 0);
  lv_obj_clear_flag(cctl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(cctl, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  _contacts_search_ta = makeSelTextarea(cctl);
  lv_textarea_set_one_line(_contacts_search_ta, true); lv_obj_add_event_cb(_contacts_search_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_placeholder_text(_contacts_search_ta, LV_SYMBOL_EYE_OPEN " Search");
  lv_obj_set_flex_grow(_contacts_search_ta, 1);
  lv_obj_set_style_pad_ver(_contacts_search_ta, 5, 0);   // shorter input (default theme pad is tall)
  lv_obj_set_style_text_font(_contacts_search_ta, &lv_font_montserrat_14, 0);
  lv_obj_add_event_cb(_contacts_search_ta, contacts_search_ta_cb, LV_EVENT_ALL, NULL);

  _contacts_filter_btn = lv_btn_create(cctl);
  lv_obj_add_event_cb(_contacts_filter_btn, contacts_filter_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* fbl = lv_label_create(_contacts_filter_btn);
  lv_label_set_text(fbl, LV_SYMBOL_LIST);
  lv_obj_center(fbl);

  buildContactRows(_tab_contacts);

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

// Short route descriptor for a contact's outbound path: "Direct", "Flood", or
// "N hops". NOTE: out_path_len is an ENCODED value (see Packet::isValidPathLen) --
// low 6 bits = hop count, high 2 bits = hash_size-1 (bytes per hop). So 0x80 means
// 0 hops / hash_size 3 = direct, NOT 128 bytes. Decode it; never treat it as a
// raw length (that both miscounts and reads out of bounds on out_path[]).
static void routeStatus(const ContactInfo& c, char* out, size_t cap) {
  if (c.out_path_len == OUT_PATH_UNKNOWN) { snprintf(out, cap, "Flood"); return; }
  int hops = c.out_path_len & 63;
  if (hops == 0) { snprintf(out, cap, "Direct"); return; }
  snprintf(out, cap, "%d hop%s", hops, hops == 1 ? "" : "s");
}

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
// Category filter (_contacts_filt, shared across the Contacts tab + picker) plus a
// per-list name search (the caller passes its own search string).
bool UITask::contactPasses(const ContactInfo& c, const char* search) {
  switch (_contacts_filt) {
    case 1: if (!(c.flags & CONTACT_FLAG_FAVOURITE)) return false; break;
    case 2: if (c.type != ADV_TYPE_CHAT)     return false; break;
    case 3: if (c.type != ADV_TYPE_REPEATER) return false; break;
    case 4: if (c.type != ADV_TYPE_ROOM)     return false; break;
    case 5: if (c.type != ADV_TYPE_SENSOR)   return false; break;
    default: break;  // All
  }
  if (search && search[0]) {
    char dn[CHAT_PEER_NAME_MAX];
    displayName(c.id.pub_key, c.name, dn, sizeof(dn));
    if (!containsCI(dn, search) && !containsCI(c.name, search)) return false;
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

// Populate a list's display set: shared category filter (_contacts_filt) + a
// per-list name search, sorted by the shared order (_contacts_order). Used by both
// the Contacts tab and the picker so filter/sort settings stay in sync.
void UITask::fillContactDisplaySet(ContactListView& lv, const char* search) {
  lv.count = 0;
  int total = mproxy::getNumContacts();
  for (int i = 0; i < total && lv.count < CONTACTS_MAX_ROWS; i++) {
    ContactInfo c;
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (!contactPasses(c, search)) continue;
    lv.rows[lv.count].idx = (uint16_t)i;
    // Sort key: latest message time for "Latest Messages", else last-heard by OUR
    // clock (lastmod) -- not last_advert_timestamp, which is their (untrusted) clock.
    lv.rows[lv.count].heard = (_contacts_order == 2)
        ? _msgs->latestTimestampFor(c.name) : c.lastmod;
    lv.rows[lv.count].fav = (c.flags & CONTACT_FLAG_FAVOURITE) ? 1 : 0;
    lv.count++;
  }
  s_contacts_order = _contacts_order;
  qsort(lv.rows, lv.count, sizeof(ContactDispRow), crow_cmp);
}

void UITask::rebuildContactsList() {
  if (!_clist.scroll) return;
  _contacts_dirty = false;
  _contacts_rebuilt_ms = millis();
  fillContactDisplaySet(_clist, _contacts_filter);
  clistRefresh(_clist, mproxy::getNumContacts() > 0
      ? "No contacts match." : "No contacts yet.\nWaiting for adverts...");
}

// Unread marker: a bold red right-pointing chevron ">" (a concave dart) drawn once
// into a shared image and layered between the avatar circle and its letter. Spans the
// avatar's left half: rear arms at the box's left edge 2px from top/bottom, tip in at
// the center (behind the letter), back notch pushed in ~1/3 so it stays thick. A thin
// background-colored outline separates the red from the avatar color underneath.
#define UNREAD_MARK_W 22
#define UNREAD_MARK_H 40
static uint8_t s_mark_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(UNREAD_MARK_W, UNREAD_MARK_H)];
static lv_img_dsc_t s_mark_img;
static void buildUnreadMark() {
  static bool built = false;
  if (built) return;
  built = true;
  lv_obj_t* cv = lv_canvas_create(lv_layer_top());   // temp; draw is immediate into the buffer
  lv_canvas_set_buffer(cv, s_mark_buf, UNREAD_MARK_W, UNREAD_MARK_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
  // top rear arm -> front tip (box center) -> bottom rear arm -> back notch (1/3 in).
  lv_point_t pts[4] = {
    { 0, 2 }, { 13, UNREAD_MARK_H / 2 }, { 0, UNREAD_MARK_H - 2 }, { 4, UNREAD_MARK_H / 2 },
  };
  // Outline FIRST: a thick BG-colored loop straddling the silhouette edges. The red
  // fill (drawn next) then covers the inner half, leaving the outline OUTSIDE the red
  // -- so the points keep full oomph instead of being masked. Where the marker
  // overhangs onto the row background the BG outline blends away, so it only reads
  // where the red sits over the avatar circle.
  lv_draw_line_dsc_t ld; lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(BG_HEX); ld.width = 2; ld.round_start = 1; ld.round_end = 1;
  lv_point_t loop[5] = { pts[0], pts[1], pts[2], pts[3], pts[0] };
  lv_canvas_draw_line(cv, loop, 5, &ld);
  lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(UI_UNREAD); d.bg_opa = LV_OPA_COVER;   // alert red -- the outline gives it pop
  // LVGL's canvas polygon fill is convex-only; this chevron is concave (the notch),
  // so fill it as two triangles meeting at the front spine (notch <-> tip).
  lv_point_t tri_top[3] = { pts[0], pts[1], pts[3] };
  lv_point_t tri_bot[3] = { pts[2], pts[1], pts[3] };
  lv_canvas_draw_polygon(cv, tri_top, 3, &d);
  lv_canvas_draw_polygon(cv, tri_bot, 3, &d);
  lv_obj_del(cv);
  s_mark_img.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
  s_mark_img.header.w  = UNREAD_MARK_W;
  s_mark_img.header.h  = UNREAD_MARK_H;
  s_mark_img.data_size = sizeof(s_mark_buf);
  s_mark_img.data      = s_mark_buf;
}

// One contact-list row's widget tree (avatar + unread marker + first-grapheme/glyph
// + name + last-seen). Shared by every contact list so rows are identical; tap_cb
// (with tap_ctx as its event user_data) lets each list pick its own tap behavior.
void UITask::makeContactRowSlot(lv_obj_t* parent, ContactRow& w, lv_event_cb_t tap_cb, void* tap_ctx) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), UI_CONTACT_ROW_H);
  lv_obj_set_style_bg_color(row, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(row, lv_color_hex(UI_BORDER), 0);
  lv_obj_set_style_border_width(row, 1, 0);
  lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_pad_left(row, 8, 0);
  lv_obj_set_style_pad_right(row, 8, 0);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_event_cb(row, tap_cb, LV_EVENT_CLICKED, tap_ctx);

  lv_obj_t* av = lv_obj_create(row);
  lv_obj_remove_style_all(av);
  lv_obj_set_size(av, UI_AVATAR_D, UI_AVATAR_D);
  lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
  lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // Unread marker: shared red chevron, child of the avatar created BEFORE the letter
  // so the draw order is circle -> dart -> letter (letter stays on top + crisp).
  lv_obj_t* dot = lv_img_create(av);
  lv_img_set_src(dot, &s_mark_img);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
  lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* avl = lv_label_create(av);
  lv_obj_center(avl);
  lv_obj_set_style_text_color(avl, lv_color_hex(UI_ON_COLOR), 0);
  lv_obj_set_style_text_font(avl, fontHeading(), 0);

  lv_obj_t* nm = lv_label_create(row);
  lv_obj_set_flex_grow(nm, 1);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(nm, fontBody(), 0);
  lv_obj_set_style_text_color(nm, lv_color_hex(FG_HEX), 0);

  lv_obj_t* sn = lv_label_create(row);
  lv_obj_set_style_text_color(sn, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(sn, fontBody(), 0);

  w.root = row; w.avatar = av; w.avatar_lbl = avl; w.name = nm; w.seen = sn; w.dot = dot;
}

// Fill a row's widgets from a contact: branding (name-seeded color + grapheme, or
// type glyph), last-seen, unread chevron, favourite color. Shared content-fill.
void UITask::fillContactRow(ContactRow& w, const ContactInfo& c) {
  char dn[CHAT_PEER_NAME_MAX];
  displayName(c.id.pub_key, c.name, dn, sizeof(dn));
  char clean[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(dn[0] ? dn : "(unnamed)", clean, sizeof(clean));
  lv_label_set_text(w.name, clean);

  lv_obj_set_style_border_width(w.avatar, 0, 0);   // clear any New-Contact ring (slot reuse)
  lv_obj_set_style_text_color(w.avatar_lbl, lv_color_hex(UI_ON_COLOR), 0);   // letter back to white
  bool is_chat = (c.type == ADV_TYPE_CHAT || c.type == 0);
  if (is_chat) {
    char g[8]; firstGrapheme(clean, g, sizeof(g));
    lv_label_set_text(w.avatar_lbl, g[0] ? g : "?");
    lv_obj_set_style_bg_color(w.avatar, lv_color_hex(nameColor(dn)), 0);
  } else {
    lv_label_set_text(w.avatar_lbl, contactSymbol(c.type));
    lv_obj_set_style_bg_color(w.avatar, lv_color_hex(UI_AVATAR_NEUT), 0);
  }

  char ago[16];   // lastmod = OUR clock (last_advert_timestamp is the remote's, untrusted)
  formatLastSeen(ago, sizeof(ago), c.lastmod, mproxy::rtcSeconds());
  lv_label_set_text(w.seen, ago);

  char ckey[CHAT_PEER_NAME_MAX];
  convKey(c.id.pub_key, false, ckey, sizeof(ckey));
  bool unread = isUnread(ckey);
  bool fav = (c.flags & CONTACT_FLAG_FAVOURITE);
  if (unread) lv_obj_clear_flag(w.dot, LV_OBJ_FLAG_HIDDEN);
  else        lv_obj_add_flag(w.dot, LV_OBJ_FLAG_HIDDEN);
  // The red chevron carries the unread signal, so the name keeps its normal color
  // (amber for favourites, default otherwise).
  lv_obj_set_style_text_color(w.name, lv_color_hex(fav ? FAV_HEX : FG_HEX), 0);
}

// Create the scroll container, content spacer, empty-placeholder, and the recycled
// pool of row widgets (avatar + name + last-seen + unread marker).
void UITask::clistBuild(ContactListView& lv, lv_obj_t* parent) {
  buildUnreadMark();
  lv.scroll = lv_obj_create(parent);
  lv_obj_set_width(lv.scroll, LV_PCT(100));
  lv_obj_set_flex_grow(lv.scroll, 1);            // fill remaining height
  lv_obj_set_style_bg_color(lv.scroll, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(lv.scroll, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(lv.scroll, 0, 0);
  lv_obj_set_style_radius(lv.scroll, 0, 0);
  lv_obj_set_style_pad_all(lv.scroll, 0, 0);
  lv_obj_set_scroll_dir(lv.scroll, LV_DIR_VER);
  // Deliberately NO layout: rows are positioned manually in content space so they
  // can be recycled (a layout would override our lv_obj_set_y).

  lv.spacer = lv_obj_create(lv.scroll);
  lv_obj_remove_style_all(lv.spacer);
  lv_obj_set_pos(lv.spacer, 0, 0);
  lv_obj_set_size(lv.spacer, 1, 0);
  lv_obj_clear_flag(lv.spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  lv.empty = lv_label_create(lv.scroll);
  lv_obj_set_style_text_color(lv.empty, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_align(lv.empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_add_flag(lv.empty, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(lv.empty, LV_ALIGN_TOP_MID, 0, 16);

  int pool = _screen_h / UI_CONTACT_ROW_H + 3;   // cover the viewport + overscan
  if (pool > CONTACT_POOL_MAX) pool = CONTACT_POOL_MAX;
  if (pool < 1) pool = 1;
  lv.pool_n = pool;
  lv.first_visible = -1;
  lv.count = 0;

  for (int s = 0; s < lv.pool_n; s++) {
    lv.bound_idx[s] = -1;
    makeContactRowSlot(lv.scroll, lv.pool[s], clist_row_cb, &lv);  // &lv -> static cbs
  }

  lv.sb = attachScrollHandle(lv.scroll);
  lv_obj_add_event_cb(lv.scroll, clist_scroll_cb, LV_EVENT_SCROLL, &lv);
}

// Render the special lead row pinned at display index 0 (+New Contact, or self).
void UITask::fillLeadRow(ContactListView& lv, ContactRow& w) {
  lv_obj_add_flag(w.dot, LV_OBJ_FLAG_HIDDEN);   // no unread marker on the lead row
  if (lv.lead == ContactListView::LEAD_NEW) {
    lv_label_set_text(w.avatar_lbl, LV_SYMBOL_PLUS);
    // Hollow off-white ring (bg-colored fill) -> an empty slot waiting to be filled.
    lv_obj_set_style_bg_color(w.avatar, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_border_color(w.avatar, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_border_width(w.avatar, 2, 0);
    lv_obj_set_style_text_color(w.avatar_lbl, lv_color_hex(FG_HEX), 0);
    lv_label_set_text(w.name, "New Contact");
    lv_obj_set_style_text_color(w.name, lv_color_hex(UI_ACCENT), 0);
    lv_label_set_text(w.seen, "");
  } else {  // LEAD_SELF: our own identity, like a contact card
    lv_obj_set_style_border_width(w.avatar, 0, 0);   // clear the New-Contact ring (slot reuse)
    lv_obj_set_style_text_color(w.avatar_lbl, lv_color_hex(UI_ON_COLOR), 0);   // letter back to white
    const char* who = (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : "(unnamed)";
    char clean[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(who, clean, sizeof(clean));
    lv_label_set_text(w.name, clean);
    lv_obj_set_style_text_color(w.name, lv_color_hex(FG_HEX), 0);
    char g[8]; firstGrapheme(clean, g, sizeof(g));
    lv_label_set_text(w.avatar_lbl, g[0] ? g : "?");
    lv_obj_set_style_bg_color(w.avatar, lv_color_hex(nameColor(who)), 0);
    lv_label_set_text(w.seen, "You");
  }
}

// Bind pool slot `slot` to display index `disp`. With a lead row, disp 0 is the
// lead and contact rows start at disp 1 (offset by leadN).
void UITask::clistBind(ContactListView& lv, int slot, int disp) {
  ContactRow& w = lv.pool[slot];
  int leadN = (lv.lead != ContactListView::LEAD_NONE) ? 1 : 0;
  if (leadN && disp == 0) {
    fillLeadRow(lv, w);
  } else {
    ContactInfo c;
    if (!mproxy::getContactByIdx(lv.rows[disp - leadN].idx, c)) return;
    fillContactRow(w, c);
  }
  lv_obj_set_user_data(w.root, (void*)(intptr_t)disp);
}

// Re-window the pool to the current scroll position. A display index maps to slot
// (disp % pool_n), so scrolling by one row rebinds exactly one slot.
void UITask::clistRelayout(ContactListView& lv) {
  int total = lv.count + ((lv.lead != ContactListView::LEAD_NONE) ? 1 : 0);
  if (!lv.scroll || lv.pool_n == 0 || total == 0) return;
  int sy = lv_obj_get_scroll_y(lv.scroll);
  if (sy < 0) sy = 0;
  int top = sy / UI_CONTACT_ROW_H;
  if (top == lv.first_visible) return;   // window unchanged -> no churn
  lv.first_visible = top;
  for (int i = 0; i < lv.pool_n; i++) {
    int disp = top + i;
    int slot = disp % lv.pool_n;
    ContactRow& w = lv.pool[slot];
    if (disp >= total) {
      if (lv.bound_idx[slot] != -1) {
        lv_obj_add_flag(w.root, LV_OBJ_FLAG_HIDDEN);
        lv.bound_idx[slot] = -1;
      }
      continue;
    }
    if (lv.bound_idx[slot] != disp) {
      clistBind(lv, slot, disp);
      lv_obj_set_y(w.root, disp * UI_CONTACT_ROW_H);
      lv_obj_clear_flag(w.root, LV_OBJ_FLAG_HIDDEN);
      lv.bound_idx[slot] = disp;
    }
  }
}

// Apply a freshly-filled lv.rows/lv.count: size the virtual content, reset to the
// top, force a full re-window, refresh the scrollbar. Shows empty_text when empty.
void UITask::clistRefresh(ContactListView& lv, const char* empty_text) {
  if (!lv.scroll) return;
  int total = lv.count + ((lv.lead != ContactListView::LEAD_NONE) ? 1 : 0);
  if (total == 0) {
    lv_obj_set_height(lv.spacer, 0);
    for (int s = 0; s < lv.pool_n; s++) {
      lv_obj_add_flag(lv.pool[s].root, LV_OBJ_FLAG_HIDDEN);
      lv.bound_idx[s] = -1;
    }
    if (lv.empty) { lv_label_set_text(lv.empty, empty_text); lv_obj_clear_flag(lv.empty, LV_OBJ_FLAG_HIDDEN); }
    sb_update(lv.scroll, lv.sb);   // nothing to scroll -> hide thumb
    return;
  }
  if (lv.empty) lv_obj_add_flag(lv.empty, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_height(lv.spacer, total * UI_CONTACT_ROW_H);
  lv_obj_scroll_to_y(lv.scroll, 0, LV_ANIM_OFF);
  for (int s = 0; s < lv.pool_n; s++) lv.bound_idx[s] = -1;
  lv.first_visible = -1;
  clistRelayout(lv);
  lv_obj_update_layout(lv.scroll);   // refresh scroll range -> drag thumb
  sb_update(lv.scroll, lv.sb);
}

void UITask::clist_scroll_cb(lv_event_t* e) {
  ContactListView* lv = (ContactListView*)lv_event_get_user_data(e);
  if (_instance && lv) _instance->clistRelayout(*lv);
}

void UITask::clist_row_cb(lv_event_t* e) {
  if (!_instance) return;
  ContactListView* lv = (ContactListView*)lv_event_get_user_data(e);
  if (!lv) return;
  int disp = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  int leadN = (lv->lead != ContactListView::LEAD_NONE) ? 1 : 0;
  if (leadN && disp == 0) { if (lv->on_lead) lv->on_lead(); return; }   // lead row
  int ci = disp - leadN;
  if (ci < 0 || ci >= lv->count) return;
  ContactInfo c;
  if (!mproxy::getContactByIdx(lv->rows[ci].idx, c)) return;
  if (lv->on_tap) lv->on_tap(c);   // per-instance behavior (open chat / pick-for-send)
}

// Contacts tab tap behavior: open the tapped contact's chat.
void UITask::clistOpenContact(const ContactInfo& c) {
  if (!_instance) return;
  _instance->_chat_is_channel = false;
  memcpy(_instance->_chat_pubkey, c.id.pub_key, 6);
  _instance->openChat(c.name);
}

// Picker tap behavior: run the pending action against the chosen contact -- share
// the viewed contact's card to this recipient (action 1) or insert this contact's
// ref into the compose field (action 2).
void UITask::clistPickSelect(const ContactInfo& pick) {
  if (!_instance) return;
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

// Picker lead row: select our own contact (synthesize a self ContactInfo and run
// the same pick action -- e.g. insert our own contact ref).
void UITask::clistPickSelf() {
  if (!_instance || !mproxy::selfPubKey()) return;
  ContactInfo self{};
  memcpy(self.id.pub_key, mproxy::selfPubKey(), PUB_KEY_SIZE);
  self.type = ADV_TYPE_CHAT;
  const char* nm = (_instance->_node_prefs && _instance->_node_prefs->node_name[0])
                       ? _instance->_node_prefs->node_name : "Me";
  strncpy(self.name, nm, sizeof(self.name) - 1);
  clistPickSelect(self);
}

// The Contacts tab is one ContactListView instance whose tap opens the chat.
void UITask::buildContactRows(lv_obj_t* parent) {
  clistBuild(_clist, parent);
  _clist.on_tap = clistOpenContact;
  _clist.lead = ContactListView::LEAD_NEW;   // "+ New Contact" pinned at the top
  _clist.on_lead = clistNewContact;
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
  lv_obj_set_style_text_color(ind, lv_color_hex(UI_ACCENT), 0);
  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_FG_BRIGHT), 0);
  return row;
}

static lv_obj_t* addFilterHeader(lv_obj_t* card, const char* text) {
  lv_obj_t* h = lv_label_create(card);
  lv_label_set_text(h, text);
  lv_obj_set_style_text_color(h, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_text_font(h, fontHeading(), 0);
  lv_obj_set_style_pad_top(h, 4, 0);
  return h;
}

void UITask::showContactsFilter() {
  if (!_cfilt_popup) {
    lv_obj_t* card = makeModalCard(&_cfilt_popup, [](lv_event_t* ev) {  // tap backdrop to close
      (void)ev; if (_instance) lv_obj_add_flag(_instance->_cfilt_popup, LV_OBJ_FLAG_HIDDEN);
    });
    lv_obj_set_width(card, LV_PCT(80));
    lv_obj_align(card, LV_ALIGN_TOP_RIGHT, -6, HEADER_H + 6);  // drop down from the filter button
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
  if (_instance->_pick_popup && !lv_obj_has_flag(_instance->_pick_popup, LV_OBJ_FLAG_HIDDEN))
    _instance->rebuildPicker();   // shared order setting -> keep an open picker in sync
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
  if (_instance->_pick_popup && !lv_obj_has_flag(_instance->_pick_popup, LV_OBJ_FLAG_HIDDEN))
    _instance->rebuildPicker();   // shared category filter -> keep an open picker in sync
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

static void hexAvatarDrawCb(lv_event_t* e);                                 // (defined below)
static void brandChannelAvatar(lv_obj_t* hex, lv_obj_t* lbl, const char* cname);

void UITask::rebuildChannelsList() {
  if (!_channels_list) return;
  lv_obj_clean(_channels_list);

  // "+ New channel" entry: a hollow white circle ring (an empty channel waiting to be
  // created), matching the New Contact placeholder. Same row layout as the channel
  // rows below, so the two pages read consistently. (Channels are circles overall;
  // the per-channel hexagon brand would be too busy at this size for a placeholder.)
  {
    lv_obj_t* row = lv_obj_create(_channels_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_CONTACT_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, newchan_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* av = lv_obj_create(row);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, UI_AVATAR_D, UI_AVATAR_D);
    lv_obj_set_style_bg_color(av, lv_color_hex(BG_HEX), 0);          // hollow (matches the row)
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(av, lv_color_hex(FG_HEX), 0);      // off-white circle ring (theme tint)
    lv_obj_set_style_border_width(av, 2, 0);
    lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* avl = lv_label_create(av);
    lv_obj_center(avl);
    lv_obj_set_style_text_color(avl, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_text_font(avl, fontHeading(), 0);
    lv_label_set_text(avl, LV_SYMBOL_PLUS);

    lv_obj_t* nm = lv_label_create(row);
    lv_obj_set_flex_grow(nm, 1);
    lv_obj_set_style_text_font(nm, fontBody(), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(UI_ACCENT), 0);
    lv_label_set_text(nm, "New channel");
  }

  // getChannel() returns true for any in-range slot incl. empty ones, so skip
  // slots with no name. The Public channel is added on every boot (MyMesh).
  // Rows match the contacts list: a name-colored hexagon avatar + the shared red
  // unread chevron + the name. (Few channels, so plain rows -- no recycler.)
  for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
    ChannelDetails ch;
    if (!mproxy::getChannel(idx, ch) || ch.name[0] == 0) continue;

    char ckey[CHAT_PEER_NAME_MAX];
    convKey(ch.channel.secret, true, ckey, sizeof(ckey));
    bool unread = isUnread(ckey);
    char cname[CHAT_PEER_NAME_MAX + 4];
    sanitizeForFont(ch.name, cname, sizeof(cname));

    lv_obj_t* row = lv_obj_create(_channels_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), UI_CONTACT_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(BG_HEX), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(row, (void*)(intptr_t)idx);
    lv_obj_add_event_cb(row, channel_clicked_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* av = lv_obj_create(row);
    lv_obj_remove_style_all(av);
    lv_obj_set_size(av, UI_AVATAR_D, UI_AVATAR_D);
    lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
    lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(av, hexAvatarDrawCb, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_t* dot = lv_img_create(av);   // shared unread chevron (created before the letter)
    lv_img_set_src(dot, &s_mark_img);
    lv_obj_add_flag(dot, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
    if (!unread) lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t* avl = lv_label_create(av);
    lv_obj_center(avl);
    lv_obj_set_style_text_color(avl, lv_color_hex(UI_ON_COLOR), 0);
    lv_obj_set_style_text_font(avl, fontHeading(), 0);
    brandChannelAvatar(av, avl, ch.name);   // name-colored hexagon

    lv_obj_t* nm = lv_label_create(row);
    lv_obj_set_flex_grow(nm, 1);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(nm, fontBody(), 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(unread ? UI_UNREAD : FG_HEX), 0);
    lv_label_set_text(nm, cname);
    // (long-press intentionally left unbound -- reserved for a future context menu)
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
  lv_obj_set_style_bg_color(_insert_popup, lv_color_hex(UI_SCRIM), 0);
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
  lv_obj_set_style_bg_color(_insert_list, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_border_width(_insert_list, 0, 0);

  lv_obj_add_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

static void styleMenuBtn(lv_obj_t* b) {
  lv_obj_set_style_bg_color(b, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(UI_FG_BRIGHT), 0);
  lv_obj_set_style_border_color(b, lv_color_hex(UI_BORDER), 0);
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
  if (_clip_kind != CLIP_EMPTY) {  // only when something has been copied
    lv_obj_t* b3 = lv_list_add_btn(_insert_list, LV_SYMBOL_PASTE, "Paste");
    styleMenuBtn(b3);
    lv_obj_add_event_cb(b3, insert_paste_cb, LV_EVENT_CLICKED, NULL);
  }
  lv_obj_clear_flag(_insert_popup, LV_OBJ_FLAG_HIDDEN);
}

// ===== Virtualized contact picker (share recipient / insert reference) =======
// A full-screen top-layer panel: header (title tells the mode) + search field +
// the same ContactListView component as the Contacts tab, so it shows the whole
// address book without a cap and looks/scrolls identically -- only the tap action
// differs (select-for-send instead of opening the contact).

void UITask::buildContactPickerScreen() {
  if (_pick_popup) return;
  _pick_popup = lv_obj_create(lv_layer_top());
  lv_obj_set_size(_pick_popup, _screen_w, _screen_h);
  lv_obj_set_pos(_pick_popup, 0, 0);
  styleAsDarkScreen(_pick_popup);
  lv_obj_set_style_pad_all(_pick_popup, 0, 0);
  lv_obj_set_flex_flow(_pick_popup, LV_FLEX_FLOW_COLUMN);

  // Standard header: back button (bail without picking, like every page) + title.
  lv_obj_t* bar = lv_obj_create(_pick_popup);
  lv_obj_set_width(bar, LV_PCT(100));
  lv_obj_set_height(bar, HEADER_H);
  lv_obj_set_style_bg_color(bar, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 6, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  makeBackButton(bar, pick_close_cb);
  _pick_title = lv_label_create(bar);
  lv_obj_set_flex_grow(_pick_title, 1);
  lv_label_set_long_mode(_pick_title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(_pick_title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_pick_title, fontTitle(), 0);

  // Search + filter button, same row layout (and slim styling) as the Contacts tab.
  lv_obj_t* srow = lv_obj_create(_pick_popup);
  lv_obj_set_width(srow, LV_PCT(100));
  lv_obj_set_height(srow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(srow, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(srow, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(srow, 0, 0);
  lv_obj_set_style_radius(srow, 0, 0);
  lv_obj_set_style_pad_hor(srow, 6, 0);
  lv_obj_set_style_pad_ver(srow, 4, 0);
  lv_obj_set_style_pad_column(srow, 6, 0);
  lv_obj_clear_flag(srow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(srow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(srow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  _pick_search_ta = makeSelTextarea(srow);
  lv_textarea_set_one_line(_pick_search_ta, true); lv_obj_add_event_cb(_pick_search_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_placeholder_text(_pick_search_ta, LV_SYMBOL_EYE_OPEN " Search");
  lv_obj_set_flex_grow(_pick_search_ta, 1);
  lv_obj_set_style_pad_ver(_pick_search_ta, 5, 0);
  lv_obj_set_style_text_font(_pick_search_ta, &lv_font_montserrat_14, 0);
  lv_obj_add_event_cb(_pick_search_ta, pick_search_ta_cb, LV_EVENT_ALL, NULL);
  lv_obj_t* fbtn = lv_btn_create(srow);   // same order/filter pop-out as the Contacts tab
  lv_obj_add_event_cb(fbtn, contacts_filter_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* fbl = lv_label_create(fbtn);
  lv_label_set_text(fbl, LV_SYMBOL_LIST);
  lv_obj_center(fbl);

  // Same virtualized contact-list component as the Contacts tab; tap selects the
  // contact for the pending send/insert action instead of opening it.
  clistBuild(_pick_list, _pick_popup);
  _pick_list.on_tap = clistPickSelect;
  _pick_list.on_lead = clistPickSelf;   // lead row (set per-open) selects our own contact

  _pick_kb = lv_keyboard_create(_pick_popup);
  lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_FLOATING);   // overlay, don't take a flex slot
  lv_keyboard_set_textarea(_pick_kb, _pick_search_ta);
  lv_obj_add_event_cb(_pick_kb, pick_kb_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_pick_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::rebuildPicker() {
  if (!_pick_list.scroll) return;
  // Same shared category filter + order as the Contacts tab, with the picker's own
  // search box.
  fillContactDisplaySet(_pick_list, _pick_filter);
  clistRefresh(_pick_list, "No contacts.");
}

void UITask::openContactPicker(int action) {
  buildContactPickerScreen();
  _pick_action = action;
  _pick_filter[0] = 0;
  // "Share contact" (insert, action 2) offers our own contact as the lead row; the
  // recipient picker (action 1) does not.
  _pick_list.lead = (action == 2) ? ContactListView::LEAD_SELF : ContactListView::LEAD_NONE;
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
  if (!_chat_is_channel && _chat_contact_type == ADV_TYPE_REPEATER) {
    // A repeater chat is a CLI console: send the line as a command (no ACK), reply
    // arrives as a CLI_DATA message in this thread.
    postCliCommand(_chat_pubkey, _chat_key, encoded);
  } else {
    // Async send: the backend does sendMessage/sendGroupMessage + addExpectedAck and
    // replies EV_SendResult with the real ack. The bubble shows immediately.
    postSend(_chat_is_channel, _chat_is_channel ? nullptr : _chat_pubkey,
             _chat_channel_idx, _chat_key, me, encoded);
  }
  lv_textarea_set_text(_chat_input, "");
  rebuildChatHistory();
}

// CLI command to a repeater: no ACK, so show it as a plain sent bubble and post the
// command. The reply comes back as an incoming CLI_DATA message in the same thread.
void UITask::postCliCommand(const uint8_t* pubkey6, const char* conv_key, const char* text) {
  storeAppend(true, conv_key, "Me", text, mproxy::rtcSeconds(), MSG_STATUS_NONE, 0, 0, 0);
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::SendCommand;
  if (pubkey6) memcpy(c.pubkey, pubkey6, 6);
  strncpy(c.text, text, sizeof(c.text) - 1);
  c.text[sizeof(c.text) - 1] = 0;
  mproxy::postCommand(c);
}

// Runs on the backend thread (called from the_mesh.loop()): just enqueue.
void UITask::msgDelivered(uint32_t ack) {
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::Delivered;
  ev.ack  = ack;
  mproxy::pushEvent(ev);
}

// Backend thread: a repeater/room login response arrived -> enqueue for the UI.
void UITask::loginResult(const uint8_t* pubkey, bool ok, uint8_t is_admin, uint16_t keep_alive_secs) {
  mproxy::UiEvent ev{};
  ev.kind = mproxy::EvKind::LoginResult;
  ev.ok = ok;
  ev.is_admin = is_admin;
  ev.keep_alive = keep_alive_secs;
  if (pubkey) memcpy(ev.pubkey, pubkey, 6);
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
  if (_instance->_sel.state != SS_IDLE) return;   // a long-press started a text selection
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

static const lv_font_t* msgFont() { return fontBody(); }

// Type ramp definitions (see forward decls up top).
static const lv_font_t* fontHero()    { return withEmoji(&lv_font_montserrat_28); }
static const lv_font_t* fontTitle()   { return withEmoji(&lv_font_montserrat_20); }
static const lv_font_t* fontHeading() { return withEmoji(&lv_font_montserrat_16); }
static const lv_font_t* fontBody()    { return withEmoji(&lv_font_montserrat_14); }
static const lv_font_t* fontCaption() { return withEmoji(&lv_font_montserrat_12); }

// Message text is a recolor lv_label (NOT a spangroup) so it supports native text
// selection (LV_LABEL_TEXT_SELECTION) + per-letter hit-testing for the universal
// copy/paste feature. Recolor markup: plain text uses the label's default color
// (FG_TEXT); a @[name] mention is wrapped in "#34D399 @name#". Because '#' is the
// recolor command char, every LITERAL '#' in the body is escaped to "##" (LVGL
// renders that as one '#'). The visible/"logical" text therefore differs from the
// rendered markup string -- selection slices the markup back to logical text in
// labelSelToLogical() (defined with the selection controller).
static const uint32_t MSG_FG_TEXT = 0xF3F4F6;
static const uint32_t MSG_MENTION  = 0x34D399;  // emerald-400; reads on gray + blue bubbles
static const uint32_t MSG_HASHTAG  = 0x60A5FA;  // blue-400 (UI_ACCENT); #channel tags

// Public #hashtag channel key derivation -- matches the Flutter client's
// Channel.derivePskFromHashtag(): PSK = SHA256("#" + name)[0:16] (AES-128), and
// the on-air channel id = SHA256(psk)[0]. `name` is the bare tag (no '#').
static void deriveHashtagPsk(const char* name, uint8_t psk[16]) {
  char buf[80];
  snprintf(buf, sizeof(buf), "#%s", name ? name : "");
  mesh::Utils::sha256(psk, 16, (const uint8_t*)buf, (int)strlen(buf));
}

// Key for a "Public channel" by name: the well-known legacy PSK for "Public"
// (case-insensitive, so re-adding it rejoins the real Public channel), else the
// #hashtag derivation so anyone can join by name.
static void derivePublicChannelKey(const char* name, uint8_t psk[16]) {
  if (strcasecmp(name ? name : "", "public") == 0) {
    static const uint8_t PUBLIC_PSK[16] = {   // base64 izOH6cXN6mrJ5e26oRXNcg==
      0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
      0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72 };
    memcpy(psk, PUBLIC_PSK, 16);
  } else {
    deriveHashtagPsk(name, psk);
  }
}

// Is `name` (case-insensitive) the well-known Public channel?
static bool isPublicName(const char* name) { return strcasecmp(name ? name : "", "public") == 0; }

// A '#hashtag' is a '#' at a word boundary (start, or preceded by a non-alnum)
// followed by >=1 tag char [A-Za-z0-9_-]. Returns the '#' and sets *name_end to
// the char past the tag; nullptr if none from `p` onward. `base` = string start.
static const char* findHashtag(const char* p, const char* base, const char** name_end) {
  for (const char* q = p; *q; q++) {
    if (*q != '#') continue;
    if (q != base && isalnum((unsigned char)q[-1])) continue;   // not a word boundary
    const char* e = q + 1;
    while (*e && (isalnum((unsigned char)*e) || *e == '_' || *e == '-')) e++;
    if (e == q + 1) continue;   // '#' with no tag chars -> literal '#'
    *name_end = e;
    return q;
  }
  return nullptr;
}

// Frees the logical-source copy stashed on a message label (for chip tap resolution).
static void msglabel_free_cb(lv_event_t* e) {
  void* p = lv_obj_get_user_data(lv_event_get_target(e));
  if (p) lv_mem_free(p);
}

// Append [s,len) to the markup writer, escaping '#' -> '##'. `w` advances; never
// overruns `end` (leaves room for a trailing NUL).
static void appendEscaped(char*& w, char* end, const char* s, size_t len) {
  for (size_t i = 0; i < len && w < end - 2; i++) {
    if (s[i] == '#') { *w++ = '#'; *w++ = '#'; }
    else             { *w++ = s[i]; }
  }
}

static void addMessageText(lv_obj_t* bubble, const char* text) {
  char clean[CHAT_MSG_TEXT_MAX + 8];
  sanitizeForFont(text, clean, sizeof(clean));

  // Build recolor markup. Worst case mixes '#' doubling (2x) with per-mention color
  // commands (~9 bytes) -- size for ~3x the sanitized body so a max-length,
  // mention-heavy message never truncates.
  char markup[3 * (CHAT_MSG_TEXT_MAX + 8) + 32];
  char* w = markup;
  char* end = markup + sizeof(markup);
  const char* p = clean;
  while (*p && w < end - 16) {
    // Find the earliest interactive token from here: an @[name] mention or a
    // #hashtag channel tag. Emit the plain run before it, then the colored chip.
    const char* at = strstr(p, "@[");
    const char* atclose = at ? strchr(at + 2, ']') : nullptr;
    if (at && !atclose) at = nullptr;                  // malformed mention -> treat as plain
    const char* htend = nullptr;
    const char* ht = findHashtag(p, clean, &htend);
    const char* tok = at;
    bool is_mention = (at != nullptr);
    if (ht && (!tok || ht < tok)) { tok = ht; is_mention = false; }
    if (!tok) { appendEscaped(w, end, p, strlen(p)); break; }   // no more tokens

    appendEscaped(w, end, p, tok - p);                 // plain run before the token
    if (is_mention) {
      // "#34D399 @name#" -- closing '#' is always followed by plain text/end.
      int wrote = snprintf(w, end - w, "#%06X @", (unsigned)MSG_MENTION);
      if (wrote < 0 || wrote >= end - w) break;
      w += wrote;
      appendEscaped(w, end, at + 2, atclose - (at + 2));   // username
      if (w < end - 1) *w++ = '#';
      p = atclose + 1;
    } else {
      // A literal '#' can't live inside a recolor span (it closes the color), so
      // render the '#' in the default color, then the name in the tag color:
      // "##" (literal '#') + "#RRGGBB name#".
      appendEscaped(w, end, "#", 1);                       // -> "##" = one literal '#'
      int wrote = snprintf(w, end - w, "#%06X ", (unsigned)MSG_HASHTAG);
      if (wrote < 0 || wrote >= end - w) break;
      w += wrote;
      appendEscaped(w, end, tok + 1, htend - (tok + 1));   // the tag name (without '#')
      if (w < end - 1) *w++ = '#';
      p = htend;
    }
  }
  *w = 0;

  lv_obj_t* lbl = lv_label_create(bubble);
  lv_obj_set_width(lbl, LV_PCT(100));
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_label_set_recolor(lbl, true);
  lv_obj_set_style_text_font(lbl, msgFont(), 0);            // emoji/unicode-capable
  lv_obj_set_style_text_color(lbl, lv_color_hex(MSG_FG_TEXT), 0);  // default (non-recolored) color
  lv_label_set_text(lbl, markup);

  // If the run has tappable tokens (@mention / #hashtag), stash the logical source
  // on the label so a tap can resolve the exact chip under the finger (per-chip,
  // no picker). Plain runs store nothing. Freed on the label's LV_EVENT_DELETE.
  const char* htmp = nullptr;
  if (strstr(clean, "@[") || findHashtag(clean, clean, &htmp)) {
    size_t n = strlen(clean);
    char* src = (char*)lv_mem_alloc(n + 1);
    if (src) {
      memcpy(src, clean, n + 1);
      lv_obj_set_user_data(lbl, src);
      lv_obj_add_event_cb(lbl, msglabel_free_cb, LV_EVENT_DELETE, NULL);
    }
  }
}

// Chips (@mentions / #hashtags) are resolved per-tap by hit-testing the touched
// character against the message's source text -- see resolveChip() + sel_event_cb.
// No bubble-level collection or picker: you tap the exact chip you want.

// Tap a #hashtag: open the public channel if we already have it (matched by the
// derived secret), otherwise offer to join it.
void UITask::openOrJoinHashtag(const char* name) {
  uint8_t psk[16]; deriveHashtagPsk(name, psk);
  char convkey[20]; convkey[0] = 'c'; convkey[1] = 'h'; convkey[2] = '_';
  mesh::Utils::toHex(convkey + 3, psk, 6);          // ch_ + first 6 bytes of the secret
  if (openConversationByKey(convkey)) return;       // already joined -> open it
  strncpy(_joinch_name, name, sizeof(_joinch_name) - 1);
  _joinch_name[sizeof(_joinch_name) - 1] = 0;
  memcpy(_joinch_psk, psk, 16);
  showJoinChannel(name);
}

void UITask::showJoinChannel(const char* name) {
  if (!_joinch_popup) {
    lv_obj_t* card = makeModalCard(&_joinch_popup, NULL);
    _joinch_lbl = lv_label_create(card);
    lv_label_set_long_mode(_joinch_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_joinch_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_joinch_lbl, lv_color_hex(UI_FG_BRIGHT), 0);

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
    lv_obj_add_event_cb(cancel, joinch_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");

    lv_obj_t* ok = lv_btn_create(btns);
    lv_obj_set_style_bg_color(ok, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_add_event_cb(ok, joinch_join_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* okl = lv_label_create(ok);
    lv_label_set_text(okl, LV_SYMBOL_OK " Join");
  }
  char q[CHAT_PEER_NAME_MAX + 48];
  snprintf(q, sizeof(q), "Join the public channel #%s?", name);
  lv_label_set_text(_joinch_lbl, q);
  lv_obj_clear_flag(_joinch_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_joinch_popup);
}

void UITask::joinch_cancel_cb(lv_event_t* e) {
  (void)e;
  if (_instance && _instance->_joinch_popup) lv_obj_add_flag(_instance->_joinch_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::joinch_join_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  char chname[CHAT_PEER_NAME_MAX];
  snprintf(chname, sizeof(chname), "#%s", _instance->_joinch_name);   // stored/displayed with the '#'
  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::AddChannel;
  strncpy(cmd.name, chname, sizeof(cmd.name) - 1);
  memcpy(cmd.path, _instance->_joinch_psk, 16);
  cmd.path_len = 16;
  mproxy::postCommand(cmd);
  if (_instance->_joinch_popup) lv_obj_add_flag(_instance->_joinch_popup, LV_OBJ_FLAG_HIDDEN);
  char toast[CHAT_PEER_NAME_MAX + 16];
  snprintf(toast, sizeof(toast), "Joined #%s", _instance->_joinch_name);
  _instance->showToast(toast);   // appears in the channels list on the next snapshot
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

// Brand an avatar circle from a contact's (sanitized) name + type: name-seeded
// color + first grapheme for chat contacts, or a neutral circle + type glyph for
// repeaters/rooms/sensors. Same scheme as the contacts list and hero cards.
// parseContactRef over a [b,e) span (not necessarily null-terminated): copy + parse.
static bool parseContactRefSpan(const char* b, const char* e, uint8_t* pk, uint8_t& ty,
                                char* nm, size_t cap) {
  size_t n = (size_t)(e - b);
  if (n == 0 || n >= CHAT_MSG_TEXT_MAX + 8) return false;
  char buf[CHAT_MSG_TEXT_MAX + 8];
  memcpy(buf, b, n); buf[n] = 0;
  return parseContactRef(buf, pk, ty, nm, cap);
}

// Render a [s,len) text run into the bubble via the shared message pipeline; skips
// whitespace-only runs (so the card doesn't get blank lines around it).
static void addTextSpan(lv_obj_t* bubble, const char* s, size_t len) {
  bool nonblank = false;
  for (size_t i = 0; i < len; i++) if (!isspace((unsigned char)s[i])) { nonblank = true; break; }
  if (!nonblank) return;
  char buf[CHAT_MSG_TEXT_MAX + 8];
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, s, len); buf[len] = 0;
  addMessageText(bubble, buf);
}

// Does the text contain at least one parseable contact ref anywhere?
static bool textHasContactRef(const char* text) {
  const char* p = text;
  while (p && *p) {
    const char* lt = strchr(p, '<');
    if (!lt) return false;
    const char* gt = strchr(lt, '>');
    if (!gt) return false;
    uint8_t pk[PUB_KEY_SIZE], ty; char nm[CHAT_PEER_NAME_MAX];
    if (parseContactRefSpan(lt, gt + 1, pk, ty, nm, sizeof(nm))) return true;
    p = lt + 1;
  }
  return false;
}

// Heap target stashed on a contact card so a tap knows which contact it is (a
// message may contain several cards). Freed on the card's LV_EVENT_DELETE.
struct CardTarget { uint8_t pubkey[PUB_KEY_SIZE]; uint8_t type; char name[CHAT_PEER_NAME_MAX]; };

// Channel avatar = a name-colored CIRCLE (the object's normal bg) with a hexagon
// "hole" punched in it. The hole shows the background (an empty channel) or, in a
// user-in-channel alert, the user's name-hash color + their letter. The circle is
// drawn by LVGL (bg_color + radius); this hook draws the inner hexagon on top in
// the obj's BORDER color (used purely as the hole-color slot). Gated by USER_1.
//
// Regular flat-top hexagon filling the inclusive box `a` (coords are LVGL-inclusive:
// x1/x2, y1/y2 are the first/last pixels). Sized to the box width; a regular hexagon
// is sqrt(3)/2 (~0.866) as tall as wide, so it's centred vertically. Every vertex is
// placed relative to its OWN edge (left/right points on x1/x2; top/bottom edges inset
// `ex` from each side), so it's symmetric at any size with no rounded centre to drift.
static void drawHexBox(lv_draw_ctx_t* ctx, const lv_area_t* a, lv_color_t color) {
  lv_coord_t lx = a->x1, rx = a->x2;                         // left/right points sit on the box edges
  lv_coord_t span = rx - lx;                                 // vertex-to-vertex width
  lv_coord_t ex = (lv_coord_t)lroundf(span * 0.25f);         // top/bottom edge horizontal inset (R*cos60)
  lv_coord_t hh = (lv_coord_t)lroundf(span * 0.8660254f);    // regular hexagon height (= span*sin60)
  // Place the top/bottom edges by EQUAL margins from each edge (not via a rounded
  // centre) so the shape is symmetric vertically on even-height boxes too.
  lv_coord_t m = ((a->y2 - a->y1) - hh) / 2; if (m < 0) m = 0;
  lv_coord_t ty = a->y1 + m, by = a->y2 - m, cy = (ty + by) / 2;
  lv_point_t pts[6] = {
    { rx,                    cy },                           // right point
    { (lv_coord_t)(rx - ex), ty },                           // top-right
    { (lv_coord_t)(lx + ex), ty },                           // top-left
    { lx,                    cy },                           // left point
    { (lv_coord_t)(lx + ex), by },                           // bottom-left
    { (lv_coord_t)(rx - ex), by },                           // bottom-right
  };
  lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
  d.bg_color = color; d.bg_opa = LV_OPA_COVER;
  lv_draw_polygon(ctx, &d, pts, 6);
}

static void hexAvatarDrawCb(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target(e);
  if (!lv_obj_has_flag(o, LV_OBJ_FLAG_USER_1)) return;   // only on channel avatars
  lv_area_t a; lv_obj_get_coords(o, &a);                 // the (channel-color) circle's box
  lv_coord_t inset = (lv_coord_t)lroundf(lv_area_get_width(&a) * 0.12f);   // channel-color ring
  lv_area_t hole = { (lv_coord_t)(a.x1 + inset), (lv_coord_t)(a.y1 + inset),
                     (lv_coord_t)(a.x2 - inset), (lv_coord_t)(a.y2 - inset) };
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_color_t bg   = lv_obj_get_style_bg_color(lv_obj_get_parent(o), LV_PART_MAIN);  // the background
  lv_color_t fill = lv_obj_get_style_border_color(o, LV_PART_MAIN);                 // empty(bg) or user color
  drawHexBox(ctx, &hole, bg);                            // 1px background outline -> always separates the hole
  lv_area_t in = { (lv_coord_t)(hole.x1 + 1), (lv_coord_t)(hole.y1 + 1),
                   (lv_coord_t)(hole.x2 - 1), (lv_coord_t)(hole.y2 - 1) };
  drawHexBox(ctx, &in, fill);
}

// Brand a CONTACT avatar: a name-colored circle (chat) or neutral type-glyph
// circle (repeater/room/sensor). Also resets any prior hexagon (channel) state.
static void brandAvatar(lv_obj_t* circle, lv_obj_t* lbl, const char* sname, uint8_t type) {
  lv_obj_clear_flag(circle, LV_OBJ_FLAG_USER_1);              // back to circle mode
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
  if (type == ADV_TYPE_CHAT || type == 0) {
    char g[8]; firstGrapheme(sname, g, sizeof(g));
    lv_label_set_text(lbl, g[0] ? g : "?");
    lv_obj_set_style_bg_color(circle, lv_color_hex(nameColor(sname)), 0);
  } else {
    lv_label_set_text(lbl, contactSymbol(type));
    lv_obj_set_style_bg_color(circle, lv_color_hex(UI_AVATAR_NEUT), 0);
  }
}

// The name-colored circle shared by both channel-avatar variants (leading '#' is
// skipped for the color seed). Requires hexAvatarDrawCb attached to the avatar.
static void brandChannelCircle(lv_obj_t* hex, const char* cname) {
  const char* nm = cname ? cname : "";
  while (*nm == '#' || *nm == ' ') nm++;
  lv_obj_set_style_bg_color(hex, lv_color_hex(nameColor(nm[0] ? nm : cname)), 0);  // channel circle
  lv_obj_set_style_bg_opa(hex, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(hex, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(hex, 0, 0);                   // border_color is just the hole-color slot
  lv_obj_add_flag(hex, LV_OBJ_FLAG_USER_1);                   // -> hexAvatarDrawCb punches the hole
}

// Plain CHANNEL avatar (branding itself): the channel circle with a background-
// colored hexagon hole and the CHANNEL's own first letter (leading '#' skipped).
static void brandChannelAvatar(lv_obj_t* hex, lv_obj_t* lbl, const char* cname) {
  brandChannelCircle(hex, cname);
  lv_color_t bg = lv_obj_get_style_bg_color(lv_obj_get_parent(hex), LV_PART_MAIN);  // "show the background"
  lv_obj_set_style_border_color(hex, bg, 0);
  if (lbl) {
    const char* nm = cname ? cname : "";
    while (*nm == '#' || *nm == ' ') nm++;
    char g[8]; firstGrapheme(nm[0] ? nm : cname, g, sizeof(g));
    lv_label_set_text(lbl, g[0] ? g : "#");
  }
}

// USER-in-channel avatar (alerts): the channel circle with the hole filled in the
// USER's name-hash color + the user's letter -- shows that user is in the channel.
static void brandChannelUserAvatar(lv_obj_t* hex, lv_obj_t* lbl, const char* cname, const char* uname) {
  brandChannelCircle(hex, cname);
  lv_obj_set_style_border_color(hex, lv_color_hex(nameColor(uname ? uname : "")), 0);
  if (lbl) { char g[8]; firstGrapheme(uname ? uname : "", g, sizeof(g)); lv_label_set_text(lbl, g[0] ? g : "?"); }
}

// A contact reference (<pubkey:type:name>) rendered as its own bordered card box
// *inside* the chat bubble, alongside any surrounding message text. The whole card
// is the tap target: a known contact opens Contact Info, an unknown one opens the
// New Contact screen (prefilled, key locked).
void UITask::buildContactCard(lv_obj_t* parent, const ChatMessage* m,
                              const uint8_t* pubkey, uint8_t type, const char* name) {
  char sname[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(name, sname, sizeof(sname));

  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_width(card, LV_PCT(100));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_BG), 0);   // inset, darker than the bubble
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(UI_BORDER), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_pad_all(card, 8, 0);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  CardTarget* ct = (CardTarget*)lv_mem_alloc(sizeof(CardTarget));   // freed on card delete
  if (ct) {
    memcpy(ct->pubkey, pubkey, PUB_KEY_SIZE);
    ct->type = type;
    strncpy(ct->name, name, sizeof(ct->name) - 1); ct->name[sizeof(ct->name) - 1] = 0;
    lv_obj_set_user_data(card, ct);
    lv_obj_add_event_cb(card, card_free_cb, LV_EVENT_DELETE, NULL);
  }
  lv_obj_add_event_cb(card, contact_card_cb, LV_EVENT_CLICKED, NULL);
  makeCardSelectable(card);   // long-press selects the whole card (atomic)

  // Branded row: avatar circle on the left, name over "<pub..key>" on the right --
  // same layout/styling as the contacts list rows and hero cards.
  lv_obj_t* row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* av = lv_obj_create(row);
  lv_obj_remove_style_all(av);
  lv_obj_set_size(av, UI_AVATAR_D, UI_AVATAR_D);
  lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
  lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* avl = lv_label_create(av);
  lv_obj_center(avl);
  lv_obj_set_style_text_color(avl, lv_color_hex(UI_ON_COLOR), 0);
  lv_obj_set_style_text_font(avl, fontHeading(), 0);
  brandAvatar(av, avl, sname, type);

  lv_obj_t* col = lv_obj_create(row);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* nm = lv_label_create(col);
  lv_obj_set_width(nm, LV_PCT(100));
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_label_set_text(nm, sname);
  lv_obj_set_style_text_color(nm, lv_color_hex(UI_FG_BRIGHT), 0);
  lv_obj_set_style_text_font(nm, fontBody(), 0);

  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  char keytrunc[24];
  snprintf(keytrunc, sizeof(keytrunc), "<%.6s...%.6s>", hex, hex + 2 * PUB_KEY_SIZE - 6);
  lv_obj_t* kl = lv_label_create(col);
  lv_obj_set_width(kl, LV_PCT(100));
  lv_label_set_long_mode(kl, LV_LABEL_LONG_DOT);
  lv_label_set_text(kl, keytrunc);
  lv_obj_set_style_text_color(kl, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(kl, fontCaption(), 0);

  bool known = mproxy::lookupContactByPubKey(pubkey, PUB_KEY_SIZE) != nullptr;
  lv_obj_t* hint = lv_label_create(card);
  lv_label_set_text(hint, known ? LV_SYMBOL_OK " In contacts \xC2\xB7 tap to open"
                                : LV_SYMBOL_PLUS " Tap to add contact");
  lv_obj_set_style_text_color(hint, lv_color_hex(known ? 0x34D399 : UI_ACCENT), 0);
  lv_obj_set_style_text_font(hint, fontCaption(), 0);
}

// Render a message body that mixes plain text with inline "rich tokens" -- emit
// plain text runs (addMessageText) and rich widgets in order, stacked in the
// bubble. Today the only rich token is a contact ref (<pubkey:type:name>) -> an
// inline contact card; this is the hook for future inline types (channel links,
// http image links, etc.): add a detector + a builder in the scan below.
void UITask::renderRichBody(lv_obj_t* bubble, const ChatMessage* m) {
  lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(bubble, 4, 0);
  const char* p = m->text;
  while (p && *p) {
    const char* lt = strchr(p, '<');
    const char* gt = lt ? strchr(lt, '>') : nullptr;
    uint8_t pk[PUB_KEY_SIZE], ty; char nm[CHAT_PEER_NAME_MAX];
    if (lt && gt && parseContactRefSpan(lt, gt + 1, pk, ty, nm, sizeof(nm))) {
      addTextSpan(bubble, p, (size_t)(lt - p));   // text before the token (skips blanks)
      buildContactCard(bubble, m, pk, ty, nm);    // inline contact card
      p = gt + 1;
    } else if (lt) {
      addTextSpan(bubble, p, (size_t)(lt - p) + 1);  // a stray '<' -> keep as text
      p = lt + 1;
    } else {
      addTextSpan(bubble, p, strlen(p));          // trailing text
      break;
    }
  }
}

// Tap an inline contact card -> open the contact (known) or the New Contact screen
// prefilled with key locked (unknown), per the card's stashed target.
void UITask::contact_card_cb(lv_event_t* e) {
  if (!_instance) return;
  if (_instance->_sel.state != SS_IDLE) return;   // a long-press selected the card instead
  const CardTarget* t = (const CardTarget*)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (!t) return;
  if (mproxy::lookupContactByPubKey(t->pubkey, PUB_KEY_SIZE))
    _instance->openContactInfo(t->pubkey, _instance->_chat_screen);
  else
    _instance->openNewContactPrefilled(t->pubkey, t->type, t->name, _instance->_chat_screen);
}

void UITask::card_free_cb(lv_event_t* e) {
  void* p = lv_obj_get_user_data(lv_event_get_target(e));
  if (p) lv_mem_free(p);
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
      lv_obj_set_style_text_color(name, lv_color_hex(DIM_HEX), 0);  // gray-400
      lv_obj_set_style_text_font(name, fontCaption(), 0);
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
    lv_obj_set_style_bg_color(bubble, lv_color_hex(m->outgoing ? UI_MSG_OUT : UI_MSG_IN), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_align(bubble, m->outgoing ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);

    if (textHasContactRef(m->text)) {
      renderRichBody(bubble, m);   // text runs + inline contact card(s)
    } else {
      addMessageText(bubble, m->text);
      if (m->outgoing && m->status == MSG_STATUS_FAILED) {
        lv_obj_add_flag(bubble, LV_OBJ_FLAG_CLICKABLE);  // tap to resend
        lv_obj_set_user_data(bubble, (void*)m);
        lv_obj_add_event_cb(bubble, chat_resend_cb, LV_EVENT_CLICKED, NULL);
      }
    }
    // @mention / #hashtag chips are resolved per-tap on the text label itself
    // (makeLabelSelectable wires sel_event_cb -> resolveChip).

    // Make every text run in the bubble selectable (cards are wired in
    // buildContactCard). Long-press a label -> drag-select -> Copy.
    {
      uint32_t nch = lv_obj_get_child_cnt(bubble);
      for (uint32_t ci = 0; ci < nch; ci++) {
        lv_obj_t* ch = lv_obj_get_child(bubble, ci);
        if (lv_obj_check_type(ch, &lv_label_class)) makeLabelSelectable(ch);
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
      lv_obj_set_style_text_color(tl, lv_color_hex(failed ? UI_ERROR : DIM_HEX), 0);
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

// Refresh the chat header (name + avatar + route status) from the current snapshot.
// Called on open and again whenever the snapshot changes while the chat is on screen,
// so a learned/changed route or rename shows up live.
void UITask::updateChatHeader() {
  if (!_chat_title) return;
  char dn[CHAT_PEER_NAME_MAX];
  const char* shown = _chat_is_channel ? _chat_peer
                                       : displayName(_chat_pubkey, _chat_peer, dn, sizeof(dn));
  char tname[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(shown, tname, sizeof(tname));
  lv_label_set_text(_chat_title, tname);

  if (_chat_is_channel) {
    brandChannelAvatar(_chat_avatar, _chat_avatar_lbl, shown);   // name-colored hexagon
    lv_label_set_text(_chat_status, "Channel");
    return;
  }
  const ContactInfo* c = mproxy::lookupContactByPubKey(_chat_pubkey, 6);
  brandAvatar(_chat_avatar, _chat_avatar_lbl, tname,
              (c && !(c->type == ADV_TYPE_CHAT || c->type == 0)) ? c->type : ADV_TYPE_CHAT);
  char rs[24];
  if (c) routeStatus(*c, rs, sizeof(rs));
  else   snprintf(rs, sizeof(rs), "Flood");
  lv_label_set_text(_chat_status, rs);
}

void UITask::openChat(const char* peer_name) {
  strncpy(_chat_peer, peer_name ? peer_name : "", CHAT_PEER_NAME_MAX - 1);
  _chat_peer[CHAT_PEER_NAME_MAX - 1] = 0;

  // Build the screen skeleton once; reuse it across contacts.
  if (!_chat_screen) {
    _chat_screen = lv_obj_create(NULL);
    styleAsDarkScreen(_chat_screen);
    lv_obj_set_style_pad_all(_chat_screen, 0, 0);

    // Fixed top bar: back | avatar | (name / route status) | kebab.
    lv_obj_t* bar = lv_obj_create(_chat_screen);
    lv_obj_set_size(bar, _screen_w, HEADER_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 6, 0);
    lv_obj_set_style_pad_ver(bar, 4, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    makeBackButton(bar, chat_back_cb);

    // Avatar circle -- same branding as the contacts list (set per-open in openChat).
    _chat_avatar = lv_obj_create(bar);
    lv_obj_remove_style_all(_chat_avatar);
    lv_obj_set_size(_chat_avatar, 34, 34);
    lv_obj_set_style_radius(_chat_avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(_chat_avatar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(_chat_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_chat_avatar, hexAvatarDrawCb, LV_EVENT_DRAW_MAIN_END, NULL);  // channel hexagon
    _chat_avatar_lbl = lv_label_create(_chat_avatar);
    lv_obj_center(_chat_avatar_lbl);
    lv_obj_set_style_text_color(_chat_avatar_lbl, lv_color_hex(UI_ON_COLOR), 0);
    lv_obj_set_style_text_font(_chat_avatar_lbl, fontHeading(), 0);

    // Name (top) + route status (bottom). Tap anywhere here -> Contact Info.
    lv_obj_t* col = lv_obj_create(bar);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 1, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(col, cinfo_name_clicked_cb, LV_EVENT_CLICKED, NULL);

    _chat_title = lv_label_create(col);
    lv_obj_set_width(_chat_title, LV_PCT(100));
    lv_label_set_long_mode(_chat_title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(_chat_title, lv_color_hex(FG_HEX), 0);
    lv_obj_set_style_text_font(_chat_title, fontHeading(), 0);

    _chat_status = lv_label_create(col);
    lv_obj_set_width(_chat_status, LV_PCT(100));
    lv_label_set_long_mode(_chat_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(_chat_status, lv_color_hex(DIM_HEX), 0);
    lv_obj_set_style_text_font(_chat_status, fontCaption(), 0);

    // Overflow (kebab) menu.
    lv_obj_t* kebab = lv_btn_create(bar);
    lv_obj_set_style_bg_opa(kebab, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(kebab, 0, 0);
    lv_obj_set_style_pad_all(kebab, 4, 0);
    lv_obj_add_event_cb(kebab, chat_kebab_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* kl = lv_label_create(kebab);
    lv_label_set_text(kl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(kl, lv_color_hex(FG_HEX), 0);

    // In-conversation search bar: hidden until the kebab "Search" reveals it.
    // Geometry (position/height) is applied by layoutChatBody when active.
    _chat_search_bar = lv_obj_create(_chat_screen);
    lv_obj_set_style_bg_color(_chat_search_bar, lv_color_hex(BG_HEX), 0);
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

    _chat_search_ta = makeSelTextarea(_chat_search_bar);
    lv_textarea_set_one_line(_chat_search_ta, true); lv_obj_add_event_cb(_chat_search_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
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
    lv_obj_add_event_cb(_chat_history, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap history -> hide kb
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
    lv_obj_set_style_bg_color(_chat_compose, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(_chat_compose, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_chat_compose, 0, 0);
    lv_obj_set_style_radius(_chat_compose, 0, 0);
    lv_obj_set_style_pad_all(_chat_compose, 5, 0);
    lv_obj_set_style_pad_column(_chat_compose, 5, 0);
    lv_obj_clear_flag(_chat_compose, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_chat_compose, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_chat_compose, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Insert (+) button: a hollow white ring (filled with the chat-window bg, not the
    // lighter compose-bar bg) so it matches the New Contact / New channel placeholders.
    lv_obj_t* plus = lv_btn_create(_chat_compose);
    lv_obj_set_size(plus, COMPOSE_H - 14, COMPOSE_H - 14);
    lv_obj_set_style_radius(plus, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(plus, 0, 0);
    lv_obj_set_style_bg_color(plus, lv_color_hex(BG_HEX), 0);        // chat-window bg fill
    lv_obj_set_style_bg_opa(plus, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(plus, lv_color_hex(FG_HEX), 0);    // greyish off-white ring (selected-tab tint)
    lv_obj_set_style_border_width(plus, 2, 0);
    lv_obj_set_style_shadow_width(plus, 0, 0);                       // drop the default button shadow
    lv_obj_add_event_cb(plus, chat_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* plus_lbl = lv_label_create(plus);
    lv_label_set_text(plus_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(plus_lbl, lv_color_hex(FG_HEX), 0);  // greyish off-white +
    lv_obj_center(plus_lbl);

    _chat_input = makeSelTextarea(_chat_compose);
    lv_textarea_set_one_line(_chat_input, true); lv_obj_add_event_cb(_chat_input, UITask::ta_done_cb, LV_EVENT_READY, NULL);
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

  updateChatHeader();   // name + avatar + route status (also refreshed live, see loop())

  // Stable per-conversation storage key (pubkey for contacts, channel secret for channels).
  if (_chat_is_channel) {
    ChannelDetails ch;
    if (mproxy::getChannel(_chat_channel_idx, ch)) convKey(ch.channel.secret, true, _chat_key, sizeof(_chat_key));
    else _chat_key[0] = 0;
  } else {
    convKey(_chat_pubkey, false, _chat_key, sizeof(_chat_key));
  }

  // Opening a conversation marks it read; drop any unread flag + repaint its list.
  if (_chat_key[0]) {
    clearUnread(_chat_key);
    if (_chat_is_channel) _channels_pending = true;
    else                  _contacts_dirty = true;
  }

  // Remember the contact type so the compose bar / kebab can offer login + CLI for
  // repeaters and room servers.
  _chat_contact_type = ADV_TYPE_CHAT;
  if (!_chat_is_channel) {
    const ContactInfo* c = mproxy::lookupContactByPubKey(_chat_pubkey, 6);
    if (c) _chat_contact_type = c->type;
    // Repeater/room: silently auto-login if a credential is saved (backend no-ops
    // otherwise), so the console is ready without making the user stop to type.
    if (_chat_contact_type == ADV_TYPE_REPEATER || _chat_contact_type == ADV_TYPE_ROOM) {
      mproxy::MeshCmd c2{};
      c2.kind = mproxy::CmdKind::AutoLogin;
      memcpy(c2.pubkey, _chat_pubkey, 6);
      mproxy::postCommand(c2);
    }
  }

  rebuildChatHistory();
  lv_scr_load(_chat_screen);
}

// If the previous boot ended in a panic, the IDF has already written a coredump to
// the flash 'coredump' partition (coredump-to-flash is enabled in the Arduino
// sdkconfig). Read its summary and save a decodable report to the SD card (or
// internal SPIFFS if no card), then erase the image so we report it exactly once.
// The PC + backtrace addresses are offline-decoded with xtensa-...-addr2line
// against firmware.elf (match app_sha256 to be sure it's the right build).
void UITask::reportCrashIfAny() {
  if (esp_core_dump_image_check() != ESP_OK) return;   // no valid coredump stored -> normal boot

  char rpt[900];
  int n = 0;
  n += snprintf(rpt + n, sizeof(rpt) - n, "MeshCore CrowPanel crash report\n");
  n += snprintf(rpt + n, sizeof(rpt) - n,
                "reset_reason=%d  (4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 8=DEEPSLEEP 12=SDIO)\n",
                (int)esp_reset_reason());

  esp_core_dump_summary_t* s = (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
  if (s && esp_core_dump_get_summary(s) == ESP_OK) {
    n += snprintf(rpt + n, sizeof(rpt) - n, "task=%.16s\n", s->exc_task);
    n += snprintf(rpt + n, sizeof(rpt) - n, "PC=0x%08x  cause=0x%08x  vaddr=0x%08x\n",
                  (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause, (unsigned)s->ex_info.exc_vaddr);
    n += snprintf(rpt + n, sizeof(rpt) - n, "backtrace%s:", s->exc_bt_info.corrupted ? " (corrupt)" : "");
    for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16; i++)
      n += snprintf(rpt + n, sizeof(rpt) - n, " 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
    n += snprintf(rpt + n, sizeof(rpt) - n, "\napp_sha256=");
    for (size_t i = 0; i < sizeof(s->app_elf_sha256) && n < (int)sizeof(rpt) - 3; i++)
      n += snprintf(rpt + n, sizeof(rpt) - n, "%02x", s->app_elf_sha256[i]);
    n += snprintf(rpt + n, sizeof(rpt) - n, "\n");
  } else {
    n += snprintf(rpt + n, sizeof(rpt) - n, "(coredump present, summary unavailable)\n");
  }
  if (s) free(s);

  // Prefer the SD card; fall back to internal SPIFFS.
  bool wrote = false;
  char where[40] = "";
  if (SdSvc::ensureMounted()) {
    SdSvc::Lock lk;
    if (!sd.exists("/crash")) sd.mkdir("/crash");
    snprintf(where, sizeof(where), "/crash/crash-%u.txt", (unsigned)mproxy::rtcSeconds());
    FsFile f = sd.open(where, O_WRONLY | O_CREAT | O_TRUNC);
    if (f) { f.write((const uint8_t*)rpt, n); f.close(); wrote = true; }
  }
  if (!wrote) {
    File f = SPIFFS.open("/last_crash.txt", FILE_WRITE);
    if (f) { f.write((const uint8_t*)rpt, n); f.close(); wrote = true; strcpy(where, "spiffs:/last_crash.txt"); }
  }

  esp_core_dump_image_erase();   // consume it so we don't re-report the same crash

  if (wrote) snprintf(_crash_note, sizeof(_crash_note), LV_SYMBOL_WARNING " Crash report: %s", where);
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
  // Duty to restore on wake from idle-off: the configured brightness, or ~60%
  // (matching the board's default) when unset. Seed the idle timer.
  _backlight_duty = (_node_prefs && _node_prefs->display_brightness) ? _node_prefs->display_brightness : 153;
  _last_input_ms = millis();

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

  if (_node_prefs) g_avatar_palette_mode = _node_prefs->avatar_palette ? 1 : 0;  // seed avatar scheme BEFORE any list renders

  _splash_screen = buildSplashScreen();
  _home_screen   = buildHomeScreen();
  rebuildContactsList();
  rebuildChannelsList();
  lv_scr_load(_splash_screen);

  // Auto-dismiss splash after a brief dwell. Single-shot via lv_timer_del.
  lv_timer_t* t = lv_timer_create(splash_dismiss_cb, 2500, this);
  lv_timer_set_repeat_count(t, 1);

  ensureBanner();   // pre-build so the first notification frame stays light
  _muted_count = mproxy::copyMutedKeys(_muted_keys, MUTE_MAX);   // seed mutes from the backend

#ifdef PIN_BUZZER
  // Notification chimes. quiet() honors the persisted buzzer_quiet; the genericBuzzer
  // also early-returns when quiet, so notify() needs no extra sound gate.
  _buzzer.begin();
  _buzzer.quiet(_node_prefs && _node_prefs->buzzer_quiet);
#endif

  reportCrashIfAny();   // if the last boot panicked, save a decodable report (SD or SPIFFS)

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
#ifdef PIN_BUZZER
  if (!notifyEnabled()) return;   // master toggle off -> silent (buzzer_quiet also gates internally)
  switch (t) {
    case UIEventType::contactMessage:
    case UIEventType::newContactMessage:
      _buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");  // 3-note rising chime
      break;
    case UIEventType::roomMessage:
    case UIEventType::channelMessage:
      _buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");        // short blip
      break;
    case UIEventType::ack:
      _buzzer.play("ack:d=32,o=8,b=120:c");                    // single high tick
      break;
    default:
      break;
  }
#else
  (void)t;
#endif
}

// ===== Notifications =====================================================

// Copy the first UTF-8 codepoint of `in` into `out` (1-4 bytes + NUL), uppercasing
// a lone ASCII letter. Used for the avatar grapheme; routes through withEmoji so an
// emoji/CJK leading character renders. Empty if `in` is empty.
static void firstGrapheme(const char* in, char* out, size_t cap) {
  out[0] = 0;
  if (!in || !in[0] || cap < 5) return;
  uint32_t next = 0;
  _lv_txt_encoded_next(in, &next);   // advances `next` past one UTF-8 char
  if (next == 0 || next >= cap) return;
  memcpy(out, in, next);   // first character as-is (preserve case, emoji, CJK)
  out[next] = 0;
}

bool UITask::isUnread(const char* key) const {
  if (!key) return false;
  for (uint8_t i = 0; i < _unread_count; i++)
    if (strncmp(_unread_keys[i], key, CHAT_PEER_NAME_MAX) == 0) return true;
  return false;
}

void UITask::markUnread(const char* key) {
  if (!key || !key[0] || isUnread(key)) return;
  if (_unread_count >= UNREAD_MAX) {   // full: drop the oldest, keep newest convs marked
    memmove(_unread_keys[0], _unread_keys[1], (size_t)(UNREAD_MAX - 1) * CHAT_PEER_NAME_MAX);
    _unread_count = UNREAD_MAX - 1;
  }
  strncpy(_unread_keys[_unread_count], key, CHAT_PEER_NAME_MAX - 1);
  _unread_keys[_unread_count][CHAT_PEER_NAME_MAX - 1] = 0;
  _unread_count++;
}

void UITask::clearUnread(const char* key) {
  if (!key) return;
  for (uint8_t i = 0; i < _unread_count; i++) {
    if (strncmp(_unread_keys[i], key, CHAT_PEER_NAME_MAX) == 0) {
      if (i != _unread_count - 1)   // compact: move the last entry into this slot
        memcpy(_unread_keys[i], _unread_keys[_unread_count - 1], CHAT_PEER_NAME_MAX);
      _unread_count--;
      return;
    }
  }
}

bool UITask::isMuted(const char* key) const {
  if (!key) return false;
  for (int i = 0; i < _muted_count; i++)
    if (strncmp(_muted_keys[i], key, CHAT_PEER_NAME_MAX) == 0) return true;
  return false;
}

void UITask::setMuted(const char* key, bool on) {
  if (!key || !key[0]) return;
  bool have = isMuted(key);
  if (on && !have && _muted_count < MUTE_MAX) {
    strncpy(_muted_keys[_muted_count], key, CHAT_PEER_NAME_MAX - 1);
    _muted_keys[_muted_count][CHAT_PEER_NAME_MAX - 1] = 0;
    _muted_count++;
  } else if (!on && have) {
    for (int i = 0; i < _muted_count; i++)
      if (strncmp(_muted_keys[i], key, CHAT_PEER_NAME_MAX) == 0) {
        if (i != _muted_count - 1) memcpy(_muted_keys[i], _muted_keys[_muted_count - 1], CHAT_PEER_NAME_MAX);
        _muted_count--;
        break;
      }
  } else {
    return;   // no change
  }
  // Persist via the backend (it owns flash).
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::SetMute;
  strncpy(c.name, key, sizeof(c.name) - 1);
  c.muted = on;
  mproxy::postCommand(c);
}

void UITask::ta_done_cb(lv_event_t* e) {
  if (!_instance) return;
  UITask* s = _instance;
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_send(ta, LV_EVENT_DEFOCUSED, NULL);   // run the field's commit-on-defocus
  lv_obj_clear_state(ta, LV_STATE_FOCUSED);
  lv_obj_t* kbs[] = { s->_set_kb, s->_cinfo_kb, s->_path_kb, s->_newchan_kb, s->_login_kb,
                      s->_contacts_kb, s->_pick_kb, s->_pinset_kb, s->_lock_kb, s->_profile_kb };
  for (lv_obj_t* kb : kbs) if (kb) lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::kebab_mute_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_chat_key[0]) return;
  _instance->closeMenuPopup();
  bool m = _instance->isMuted(_instance->_chat_key);
  _instance->setMuted(_instance->_chat_key, !m);
  _instance->showToast(m ? "Notifications on" : "Muted");
}

// Incoming message for a conversation the user isn't looking at: wake the screen,
// pop the banner, chime. (Caller already marked it unread; mute gating is added in
// Phase D.)
// Wake the screen + show the banner for a message in a conversation you're NOT
// looking at. The chime is separate (fires for every non-muted message, viewed or
// not -- phone-style) and is deferred in drainEvents/loop so its first note isn't
// stretched by this draw work.
void UITask::onIncomingNotify(const char* conv_key, const char* sender,
                              const char* text, bool is_channel) {
  _last_input_ms = millis();                         // restart the idle-off timer
  if (_display_off) { board_set_backlight(_backlight_duty); _display_off = false; }
  showBanner(conv_key, sender, text, is_channel);
}

// Build the (reused) banner widgets once. Called at startup so the first real
// notification only repopulates labels -- not ~15 widget creations in the same
// frame the chime starts (which would stretch the first note).
void UITask::ensureBanner() {
  if (_banner) return;
  {
    _banner = lv_obj_create(lv_layer_top());
    lv_obj_set_width(_banner, _screen_w);
    lv_obj_set_height(_banner, LV_SIZE_CONTENT);
    lv_obj_align(_banner, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(_banner, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_banner, 2, 0);
    lv_obj_set_style_border_side(_banner, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(_banner, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_radius(_banner, 0, 0);
    lv_obj_set_style_pad_all(_banner, 8, 0);
    lv_obj_set_style_pad_column(_banner, 8, 0);
    lv_obj_clear_flag(_banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_banner, banner_body_cb, LV_EVENT_CLICKED, NULL);

    _banner_avatar = lv_obj_create(_banner);
    lv_obj_set_size(_banner_avatar, 36, 36);
    lv_obj_set_style_radius(_banner_avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(_banner_avatar, 0, 0);
    lv_obj_set_style_pad_all(_banner_avatar, 0, 0);
    lv_obj_clear_flag(_banner_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_banner_avatar, hexAvatarDrawCb, LV_EVENT_DRAW_MAIN_END, NULL);  // channel hole
    _banner_avatar_lbl = lv_label_create(_banner_avatar);
    lv_obj_center(_banner_avatar_lbl);
    lv_obj_set_style_text_color(_banner_avatar_lbl, lv_color_hex(UI_ON_COLOR), 0);
    lv_obj_set_style_text_font(_banner_avatar_lbl, fontHeading(), 0);

    lv_obj_t* col = lv_obj_create(_banner);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    _banner_title = lv_label_create(col);
    lv_obj_set_style_text_color(_banner_title, lv_color_hex(UI_FG_BRIGHT), 0);
    lv_obj_set_style_text_font(_banner_title, fontBody(), 0);
    // Preview is rendered through the same pipeline as chat bubbles
    // (addMessageText in showBanner): "@[name]" mentions become a highlighted
    // "@name", emoji/Unicode resolve identically. This is just a clipping host
    // capped to ~2 lines of body text so a long message can't grow the banner.
    _banner_body = lv_obj_create(col);
    lv_obj_remove_style_all(_banner_body);
    lv_obj_set_width(_banner_body, LV_PCT(100));
    lv_obj_set_height(_banner_body, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(_banner_body, 40, 0);
    lv_obj_clear_flag(_banner_body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* x = lv_btn_create(_banner);
    lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(x, 0, 0);
    lv_obj_add_event_cb(x, banner_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* xl = lv_label_create(x);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xl, lv_color_hex(UI_DIM), 0);
  }
  lv_obj_add_flag(_banner, LV_OBJ_FLAG_HIDDEN);   // built hidden; showBanner reveals it
}

void UITask::showBanner(const char* conv_key, const char* sender,
                        const char* text, bool is_channel) {
  ensureBanner();

  strncpy(_banner_key, conv_key ? conv_key : "", sizeof(_banner_key) - 1);
  _banner_key[sizeof(_banner_key) - 1] = 0;

  const char* who = (sender && sender[0]) ? sender : "(unknown)";

  if (is_channel) {
    // Channel circle (channel color) with the sender filling the hole (their color
    // + letter), so it's clearly a channel message AND shows who posted. The channel
    // name is looked up by the conv_key's secret prefix.
    char chname[CHAT_PEER_NAME_MAX] = "";
    uint8_t sec6[6];
    if (conv_key && strncmp(conv_key, "ch_", 3) == 0 && mesh::Utils::fromHex(sec6, 6, conv_key + 3)) {
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        ChannelDetails ch;
        if (!mproxy::getChannel(i, ch) || ch.name[0] == 0) continue;
        if (memcmp(ch.channel.secret, sec6, 6) == 0) { strncpy(chname, ch.name, sizeof(chname) - 1); break; }
      }
    }
    brandChannelUserAvatar(_banner_avatar, _banner_avatar_lbl, chname[0] ? chname : "#", who);
  } else {
    char cl[CHAT_PEER_NAME_MAX + 4]; sanitizeForFont(who, cl, sizeof(cl));
    brandAvatar(_banner_avatar, _banner_avatar_lbl, cl, ADV_TYPE_CHAT);   // plain sender circle
  }

  char clean[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(who, clean, sizeof(clean));
  lv_label_set_text(_banner_title, clean);
  // Same branching as a chat bubble (see the message renderer): a shared-contact
  // message is a "<pubkey:type:name>" ref, not prose -- show a concise branded
  // line (type glyph + name) like the contact card, never the raw ref; otherwise
  // run the text through addMessageText so mentions/emoji look identical to chat.
  lv_obj_clean(_banner_body);
  uint8_t cpk[PUB_KEY_SIZE], ctype;
  char cname[CHAT_PEER_NAME_MAX];
  if (text && parseContactRef(text, cpk, ctype, cname, sizeof(cname))) {
    char nm[CHAT_PEER_NAME_MAX];
    sanitizeForFont(cname, nm, sizeof(nm));
    char line[CHAT_PEER_NAME_MAX + 24];
    snprintf(line, sizeof(line), "%s Contact: %s", contactSymbol(ctype), nm);
    lv_obj_t* lbl = lv_label_create(_banner_body);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_FG_BRIGHT), 0);
    lv_obj_set_style_text_font(lbl, fontBody(), 0);
    lv_label_set_text(lbl, line);
  } else {
    addMessageText(_banner_body, text ? text : "");   // same recolor render as a chat bubble
  }

  lv_obj_clear_flag(_banner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_banner);

  if (_banner_timer) { lv_timer_del(_banner_timer); _banner_timer = NULL; }
  _banner_timer = lv_timer_create(banner_timer_cb, 5000, NULL);
  lv_timer_set_repeat_count(_banner_timer, 1);
}

void UITask::hideBanner() {
  if (_banner_timer) { lv_timer_del(_banner_timer); _banner_timer = NULL; }
  if (_banner) lv_obj_add_flag(_banner, LV_OBJ_FLAG_HIDDEN);
}

bool UITask::openConversationByKey(const char* conv_key) {
  if (!conv_key || !conv_key[0]) return false;
  if (strncmp(conv_key, "ch_", 3) == 0) {
    uint8_t secret6[6];
    if (!mesh::Utils::fromHex(secret6, 6, conv_key + 3)) return false;
    for (int idx = 0; idx < MAX_GROUP_CHANNELS; idx++) {
      ChannelDetails ch;
      if (!mproxy::getChannel(idx, ch) || ch.name[0] == 0) continue;
      if (memcmp(ch.channel.secret, secret6, 6) == 0) {
        _chat_is_channel = true;
        _chat_channel_idx = idx;
        openChat(ch.name);
        return true;
      }
    }
    return false;
  }
  uint8_t pk6[6];
  if (!mesh::Utils::fromHex(pk6, 6, conv_key)) return false;
  const ContactInfo* c = mproxy::lookupContactByPubKey(pk6, 6);
  if (!c) return false;
  _chat_is_channel = false;
  memcpy(_chat_pubkey, c->id.pub_key, 6);
  openChat(c->name);
  return true;
}

void UITask::banner_body_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  char key[CHAT_PEER_NAME_MAX];
  strncpy(key, _instance->_banner_key, sizeof(key) - 1);
  key[sizeof(key) - 1] = 0;
  _instance->hideBanner();
  _instance->openConversationByKey(key);
}

void UITask::banner_close_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->hideBanner();
}

void UITask::banner_timer_cb(lv_timer_t* t) {
  (void)t;
  if (!_instance) return;
  _instance->_banner_timer = NULL;   // one-shot: LVGL deletes it after this returns
  _instance->hideBanner();
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
    lv_obj_set_style_bg_color(_toast, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_bg_opa(_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(_toast, lv_color_hex(UI_FG_BRIGHT), 0);
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
// Keyboard-dismiss model, applied uniformly across every screen: each screen with
// a keyboard wires dismiss_kb_cb on its scroll body (tap empty space -> hide kb),
// and every *passive* layout container (field wrappers, hero cards, value rows) is
// run through makePassive() so it doesn't eat the tap before it reaches that body.
// Inputs/buttons stay clickable, so tapping another field still moves focus
// directly. Any new layout-only container placed over a keyboard-bearing body
// should also be made passive.
static void makePassive(lv_obj_t* o) {
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

static lv_obj_t* makeField(lv_obj_t* parent, const char* caption) {
  lv_obj_t* col = lv_obj_create(parent);
  lv_obj_set_width(col, LV_PCT(100));
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  makePassive(col);   // taps on a field's caption/gaps fall through to dismiss the kb
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_t* cap = lv_label_create(col);
  lv_label_set_text(cap, caption);
  lv_obj_set_style_text_color(cap, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
  return col;
}

// Big detail-page hero card: avatar circle + name (28px) stacked over a tappable
// "<pub..key>" line. Shared by the Contact Info page and the Settings > Profile
// page so every detail page looks identical. Out-params expose the pieces for
// populate; keyCb handles a tap on the key line (opens the full-key popup).
static void makeHeroCard(lv_obj_t* parent, lv_obj_t** avatarOut, lv_obj_t** avatarLblOut,
                         lv_obj_t** nameOut, lv_obj_t** keyOut, lv_event_cb_t keyCb) {
  lv_obj_t* hero = lv_obj_create(parent);
  lv_obj_remove_style_all(hero);
  lv_obj_set_width(hero, LV_PCT(100));
  lv_obj_set_height(hero, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(hero, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(hero, 8, 0);
  lv_obj_set_style_pad_all(hero, 12, 0);
  lv_obj_set_style_pad_column(hero, 12, 0);
  lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
  makePassive(hero);   // tap the card (except the key line) falls through to dismiss kb

  lv_obj_t* av = lv_obj_create(hero);
  lv_obj_remove_style_all(av);
  lv_obj_set_size(av, 56, 56);
  lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
  lv_obj_clear_flag(av, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(av, hexAvatarDrawCb, LV_EVENT_DRAW_MAIN_END, NULL);  // channel hexagon support
  lv_obj_t* avl = lv_label_create(av);
  lv_obj_center(avl);
  lv_obj_set_style_text_color(avl, lv_color_hex(UI_ON_COLOR), 0);
  lv_obj_set_style_text_font(avl, fontHero(), 0);

  lv_obj_t* col = lv_obj_create(hero);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* nm = lv_label_create(col);
  lv_obj_set_width(nm, LV_PCT(100));
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(nm, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(nm, fontHero(), 0);
  lv_obj_t* ky = lv_label_create(col);
  lv_obj_set_width(ky, LV_PCT(100));
  lv_label_set_long_mode(ky, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(ky, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(ky, fontCaption(), 0);
  lv_obj_add_flag(ky, LV_OBJ_FLAG_CLICKABLE);   // tap -> full key + copy popup
  lv_obj_add_event_cb(ky, keyCb, LV_EVENT_CLICKED, NULL);

  *avatarOut = av; *avatarLblOut = avl; *nameOut = nm; *keyOut = ky;
}

void UITask::buildContactInfoScreen() {
  if (_cinfo_screen) return;
  _cinfo_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_cinfo_screen);
  lv_obj_set_style_pad_all(_cinfo_screen, 0, 0);

  // fixed top bar (+ an add-mode Save button, hidden when viewing an existing contact)
  lv_obj_t* bar = makeHeaderBar(_cinfo_screen, "Contact", cinfo_back_cb);
  _cinfo_header_title = lv_obj_get_child(bar, 1);   // [0]=back btn, [1]=title label
  _cinfo_save_btn = lv_btn_create(bar);   // flexes to the right of the grown title
  lv_obj_add_event_cb(_cinfo_save_btn, cinfo_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* svl = lv_label_create(_cinfo_save_btn);
  lv_label_set_text(svl, LV_SYMBOL_OK " Save");
  lv_obj_add_flag(_cinfo_save_btn, LV_OBJ_FLAG_HIDDEN);

  // scrollable body
  _cinfo_body = lv_obj_create(_cinfo_screen);
  lv_obj_add_event_cb(_cinfo_body, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb
  lv_obj_set_size(_cinfo_body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(_cinfo_body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(_cinfo_body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_cinfo_body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_cinfo_body, 0, 0);
  lv_obj_set_style_radius(_cinfo_body, 0, 0);
  lv_obj_set_style_pad_all(_cinfo_body, 12, 0);
  lv_obj_set_style_pad_row(_cinfo_body, 8, 0);
  lv_obj_set_flex_flow(_cinfo_body, LV_FLEX_FLOW_COLUMN);

  // Big hero card (avatar + 28px name over the tappable "<pub..key>" line), shared
  // with the Settings > Profile page so all detail pages match. Branding
  // (name-seeded color / type glyph) is applied in populateContactInfo().
  makeHeroCard(_cinfo_body, &_cinfo_avatar, &_cinfo_avatar_lbl, &_cinfo_title,
               &_cinfo_key, cinfo_key_cb);

  // action row (Fav / Telem / Share -- view-mode only)
  lv_obj_t* actions = lv_obj_create(_cinfo_body);
  _cinfo_actions = actions;
  makePassive(actions);
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
  makePassive(ncap);
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
  lv_obj_set_style_text_font(_cinfo_realname, fontCaption(), 0);
  _cinfo_name_ta = makeSelTextarea(fn);
  lv_textarea_set_one_line(_cinfo_name_ta, true); lv_obj_add_event_cb(_cinfo_name_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_cinfo_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_cinfo_name_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  // Public key: when VIEWING an existing contact it lives in the hero (tap for the
  // full key + copy), so this field stays hidden. When ADDING, we show the whole
  // key in its own box -- typed, pasted (long-press menu), or prefilled+locked.
  _cinfo_key_field = makeField(_cinfo_body, "Public Key (hex)");
  _cinfo_key_ta = makeSelTextarea(_cinfo_key_field);
  lv_textarea_set_one_line(_cinfo_key_ta, true);
  lv_obj_add_event_cb(_cinfo_key_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_cinfo_key_ta, LV_PCT(100));
  lv_obj_add_event_cb(_cinfo_key_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);
  _cinfo_err = lv_label_create(_cinfo_key_field);
  lv_label_set_text(_cinfo_err, "");
  lv_obj_set_style_text_color(_cinfo_err, lv_color_hex(UI_ERROR), 0);
  lv_obj_add_flag(_cinfo_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_cinfo_key_field, LV_OBJ_FLAG_HIDDEN);   // view-mode default

  lv_obj_t* fp = makeField(_cinfo_body, "Position (lat, lon)");
  _cinfo_pos_field = fp;
  lv_obj_t* prow = lv_obj_create(fp);
  makePassive(prow);
  lv_obj_set_width(prow, LV_PCT(100));
  lv_obj_set_height(prow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(prow, 0, 0);
  lv_obj_set_style_pad_all(prow, 0, 0);
  lv_obj_set_style_pad_column(prow, 6, 0);
  lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
  _cinfo_lat_ta = makeSelTextarea(prow);
  lv_textarea_set_one_line(_cinfo_lat_ta, true); lv_obj_add_event_cb(_cinfo_lat_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_flex_grow(_cinfo_lat_ta, 1);
  lv_obj_add_event_cb(_cinfo_lat_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);
  _cinfo_lon_ta = makeSelTextarea(prow);
  lv_textarea_set_one_line(_cinfo_lon_ta, true); lv_obj_add_event_cb(_cinfo_lon_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_flex_grow(_cinfo_lon_ta, 1);
  lv_obj_add_event_cb(_cinfo_lon_ta, cinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  lv_obj_t* ft = makeField(_cinfo_body, "Contact Type");
  _cinfo_type_field = ft;
  _cinfo_type_lbl = lv_label_create(ft);  // read-only: type comes from their advert
  lv_obj_set_style_text_color(_cinfo_type_lbl, lv_color_hex(FG_HEX), 0);

  lv_obj_t* flh = makeField(_cinfo_body, "Last Heard");
  _cinfo_lh_field = flh;
  _cinfo_lastheard = lv_label_create(flh);
  lv_obj_set_style_text_color(_cinfo_lastheard, lv_color_hex(FG_HEX), 0);

  lv_obj_t* ftel = makeField(_cinfo_body, "Telemetry");
  _cinfo_tel_field = ftel;
  _cinfo_telem = lv_label_create(ftel);
  lv_label_set_long_mode(_cinfo_telem, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_cinfo_telem, LV_PCT(100));
  lv_obj_set_style_text_color(_cinfo_telem, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_cinfo_telem, &lv_font_montserrat_14, 0);

  // path: hops row
  lv_obj_t* hrow = lv_obj_create(_cinfo_body);
  _cinfo_hops_row = hrow;
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
  _cinfo_outpath_row = orow;
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

// Show/hide widgets for the active mode. View = an existing contact: all the
// read-only/edit sections, no Save (edits commit live). Add = a new contact:
// just the hero preview + Name + the full Key box + Save; the sections a
// not-yet-saved contact can't have (actions, position, type, last-heard,
// telemetry, path) are hidden.
void UITask::applyCinfoMode() {
  bool add = (_cinfo_mode == CINFO_ADD);
  if (_cinfo_header_title) lv_label_set_text(_cinfo_header_title, add ? "New Contact" : "Contact");
  auto show = [](lv_obj_t* o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  };
  show(_cinfo_save_btn,     add);
  show(_cinfo_key_field,    add);
  show(_cinfo_actions,      !add);
  show(_cinfo_pos_field,    !add);
  show(_cinfo_type_field,   !add);
  show(_cinfo_lh_field,     !add);
  show(_cinfo_tel_field,    !add);
  show(_cinfo_hops_row,     !add);
  show(_cinfo_outpath_row,  !add);
  if (add && _cinfo_err) lv_obj_add_flag(_cinfo_err, LV_OBJ_FLAG_HIDDEN);
}

// Refresh the hero (avatar + name + truncated key line) from current state. Used
// on open and, in add-mode, live as the Name / Key fields are edited.
void UITask::refreshCinfoHero() {
  ContactInfo* c = &_cinfo_c;
  // Resolve the name to display in the hero.
  char shownbuf[CHAT_PEER_NAME_MAX + 4];
  const char* shown;
  if (_cinfo_mode == CINFO_ADD) {
    if (!_cinfo_addkey_locked) resolveContactKey();   // refresh _cinfo_c key/haskey from the box
    const char* typed = _cinfo_name_ta ? lv_textarea_get_text(_cinfo_name_ta) : "";
    shown = (typed && typed[0]) ? typed : (c->name[0] ? c->name : "New contact");
  } else {
    shown = _cinfo_override[0] ? _cinfo_override : (c->name[0] ? c->name : "(unnamed)");
  }
  sanitizeForFont(shown, shownbuf, sizeof(shownbuf));
  lv_label_set_text(_cinfo_title, shownbuf);
  if (_cinfo_avatar) brandAvatar(_cinfo_avatar, _cinfo_avatar_lbl, shownbuf, c->type);

  // Key line: truncated <aaaaaa...bbbbbb> + copy when we have a key, else a prompt.
  if (_cinfo_mode == CINFO_ADD && !_cinfo_haskey) {
    lv_label_set_text(_cinfo_key, "(enter a key)");
  } else {
    char hex[2 * PUB_KEY_SIZE + 1];
    mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
    char ktrunc[32];
    snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>  " LV_SYMBOL_COPY, hex, hex + 2 * PUB_KEY_SIZE - 6);
    lv_label_set_text(_cinfo_key, ktrunc);
  }
}

void UITask::populateContactInfo() {
  ContactInfo* c = cinfoContact();
  if (!c) { lv_scr_load(_cinfo_return_screen ? _cinfo_return_screen : _home_screen); return; }

  // Hero (avatar + name + truncated key) is shared with add-mode and live edits.
  refreshCinfoHero();

  bool overridden = _cinfo_override[0];
  const char* shown = overridden ? _cinfo_override : c->name;
  // The Name field + "(advert name)" hint are view-mode edits of the local
  // nickname override. In add-mode the opener seeds the field and we don't
  // clobber what the user is typing.
  if (_cinfo_mode != CINFO_ADD) {
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
  } else if (_cinfo_realname) {
    lv_label_set_text(_cinfo_realname, "");
  }

  // The rest is view-mode detail (fav/position/type/last-heard/telemetry/path);
  // those widgets are hidden in add-mode, so the values below are harmless.
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

  // out_path_len is an ENCODED Packet path_len (low 6 bits = hop count, high 2 bits =
  // hash_size-1 = bytes/hop). Decode it; the real byte length is count*size -- never
  // iterate out_path_len directly (it miscounts and reads past out_path[]).
  int hopCount = c->out_path_len & 63;
  int hashSize = (c->out_path_len >> 6) + 1;
  int byteLen  = hopCount * hashSize;
  if (byteLen > MAX_PATH_SIZE) byteLen = MAX_PATH_SIZE;  // safety clamp
  // A phone-app "direct" route is a zero-filled path, so also treat all-zero as direct.
  bool allzero = true;
  for (int i = 0; i < byteLen; i++) if (c->out_path[i]) { allzero = false; break; }
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    lv_label_set_text(_cinfo_hops, "Hops Away: flood (unknown)");
    lv_obj_add_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_cinfo_outpath, "Out Path: (flood)");
  } else if (hopCount == 0 || allzero) {
    lv_label_set_text(_cinfo_hops, "Hops Away: direct");
    lv_obj_clear_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);  // allow reset to flood
    lv_label_set_text(_cinfo_outpath, "Out Path: (direct)");
  } else {
    char hb[32];
    snprintf(hb, sizeof(hb), "Hops Away: %d", hopCount);
    lv_label_set_text(_cinfo_hops, hb);
    lv_obj_clear_flag(_cinfo_hops_x, LV_OBJ_FLAG_HIDDEN);
    char op[3 * MAX_PATH_SIZE + 16];
    int n = snprintf(op, sizeof(op), "Out Path: ");
    for (int i = 0; i < byteLen && n < (int)sizeof(op) - 4; i++) {
      n += snprintf(op + n, sizeof(op) - n, "%02x", c->out_path[i]);
      // comma only on hop boundaries (hashSize bytes per hop): e.g. "0000,0000"
      if ((i % hashSize) == (hashSize - 1) && i + 1 < byteLen)
        n += snprintf(op + n, sizeof(op) - n, ",");
    }
    lv_label_set_text(_cinfo_outpath, op);
  }
}

void UITask::openContactInfo(const uint8_t* pubkey, lv_obj_t* return_screen) {
  // A link to our own contact opens the owner profile, so "view contact" behaves
  // the same whether it's someone else or us.
  const uint8_t* self = mproxy::selfPubKey();
  if (self && pubkey && memcmp(pubkey, self, PUB_KEY_SIZE) == 0) { openProfile(return_screen); return; }
  memcpy(_cinfo_pubkey, pubkey, PUB_KEY_SIZE);
  // Load a UI-owned working copy from the snapshot (edits are optimistic + posted).
  const ContactInfo* src = mproxy::lookupContactByPubKey(pubkey, PUB_KEY_SIZE);
  _cinfo_valid = (src != nullptr);
  if (src) _cinfo_c = *src;
  _cinfo_override[0] = 0;
  mproxy::getNameOverride(pubkey, _cinfo_override, sizeof(_cinfo_override));
  _cinfo_return_screen = return_screen;
  buildContactInfoScreen();
  _cinfo_mode = CINFO_VIEW;
  _cinfo_addkey_locked = false;
  _cinfo_haskey = true;          // viewing -> key is real
  _cinfo_active_ta = NULL;
  lv_obj_add_flag(_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  applyCinfoMode();
  populateContactInfo();
  lv_obj_scroll_to_y(_cinfo_body, 0, LV_ANIM_OFF);
  lv_scr_load(_cinfo_screen);
}

void UITask::commitCinfoField(lv_obj_t* ta) {
  // Add-mode: nothing is posted live; field edits just refresh the hero preview.
  // The contact is created wholesale (AddContact) only when Save is pressed.
  if (_cinfo_mode == CINFO_ADD) { refreshCinfoHero(); return; }
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
  // A prefilled key is read-only -> don't raise a keyboard for it.
  if (ta == _instance->_cinfo_key_ta && _instance->_cinfo_addkey_locked) return;
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
  } else if (code == LV_EVENT_VALUE_CHANGED) {
    // Add-mode: live hero preview as the Name / Key are typed or pasted.
    if (_instance->_cinfo_mode == CINFO_ADD) _instance->refreshCinfoHero();
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

// Shared "full public key" popup: the truncated key line on the contact hero and
// the owner profile hero both open this (full hex + a copy-to-clipboard button).
void UITask::keypop_close_cb(lv_event_t* e) {
  (void)e;
  if (_instance && _instance->_keypop_popup) lv_obj_add_flag(_instance->_keypop_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::keypop_copy_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->copyToClipboard(_instance->_keypop_hex);
  _instance->showToast("Public key copied");
  if (_instance->_keypop_popup) lv_obj_add_flag(_instance->_keypop_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::showKeyPopup(const char* hex) {
  if (!hex || !hex[0]) return;
  strncpy(_keypop_hex, hex, sizeof(_keypop_hex) - 1);
  _keypop_hex[sizeof(_keypop_hex) - 1] = 0;
  if (!_keypop_popup) {
    lv_obj_t* card = makeModalCard(&_keypop_popup, [](lv_event_t* ev) {  // tap backdrop closes
      (void)ev;
      if (_instance) lv_obj_add_flag(_instance->_keypop_popup, LV_OBJ_FLAG_HIDDEN);
    });
    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "Public Key");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_text_font(title, fontHeading(), 0);

    _keypop_lbl = lv_label_create(card);
    lv_label_set_long_mode(_keypop_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_keypop_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_keypop_lbl, lv_color_hex(UI_FG_BRIGHT), 0);
    lv_obj_set_style_text_font(_keypop_lbl, &lv_font_montserrat_12, 0);  // full hex, compact

    lv_obj_t* btnrow = lv_obj_create(card);
    lv_obj_remove_style_all(btnrow);
    lv_obj_set_width(btnrow, LV_PCT(100));
    lv_obj_set_height(btnrow, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnrow, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btnrow, 8, 0);
    lv_obj_clear_flag(btnrow, LV_OBJ_FLAG_SCROLLABLE);

    // Secondary (plain) + primary (UI_PRIMARY) buttons -- same pattern as the
    // share-position confirm dialog, so all modal actions look alike.
    lv_obj_t* closeb = lv_btn_create(btnrow);
    lv_obj_add_event_cb(closeb, keypop_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* xl = lv_label_create(closeb);
    lv_label_set_text(xl, "Close");

    lv_obj_t* copyb = lv_btn_create(btnrow);
    lv_obj_set_style_bg_color(copyb, lv_color_hex(UI_PRIMARY), 0);
    lv_obj_add_event_cb(copyb, keypop_copy_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cl = lv_label_create(copyb);
    lv_label_set_text(cl, LV_SYMBOL_COPY " Copy");
  }
  lv_label_set_text(_keypop_lbl, _keypop_hex);
  lv_obj_clear_flag(_keypop_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_keypop_popup);
}

// Contact hero key line -> full key popup (key from the contact being viewed, or
// the prospective contact in add-mode once a valid key has been entered).
void UITask::cinfo_key_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_cinfo_mode == CINFO_ADD && !_instance->_cinfo_haskey) return;
  ContactInfo* c = _instance->cinfoContact();
  if (!c) return;
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, c->id.pub_key, PUB_KEY_SIZE);
  _instance->showKeyPopup(hex);
}

// Owner profile hero key line -> full key popup (our own public key).
void UITask::profile_key_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !mproxy::selfPubKey()) return;
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, mproxy::selfPubKey(), PUB_KEY_SIZE);
  _instance->showKeyPopup(hex);
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
    _menu_popup = makeBackdrop([](lv_event_t* ev) {
      (void)ev; if (_instance) _instance->closeMenuPopup();
    });
    _menu_list = lv_list_create(_menu_popup);
    lv_obj_set_width(_menu_list, LV_PCT(82));
    lv_obj_set_height(_menu_list, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(_menu_list, LV_PCT(72), 0);
    lv_obj_center(_menu_list);
    lv_obj_set_style_bg_color(_menu_list, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_border_width(_menu_list, 0, 0);
    lv_obj_set_style_radius(_menu_list, 8, 0);   // match the modal-card radius
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
  makeHeaderBar(_qr_screen, "Share QR", qr_back_cb);

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
  lv_obj_set_style_text_font(_qr_name_lbl, fontHero(), 0);

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
  lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_LIST, "Details"),
                      s->_chat_is_channel ? kebab_chdetails_cb : kebab_details_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_EYE_OPEN, "Search"), kebab_search_cb, LV_EVENT_CLICKED, NULL);
  {
    bool m = s->isMuted(s->_chat_key);
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_BELL, m ? "Unmute notifications" : "Mute notifications"),
                        kebab_mute_cb, LV_EVENT_CLICKED, NULL);
  }
  if (!s->_chat_is_channel &&
      (s->_chat_contact_type == ADV_TYPE_REPEATER || s->_chat_contact_type == ADV_TYPE_ROOM)) {
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_SETTINGS, "Login"), kebab_login_cb, LV_EVENT_CLICKED, NULL);
  }
  if (s->_chat_is_channel) {
    lv_obj_add_event_cb(lv_list_add_btn(list, LV_SYMBOL_UPLOAD, "Share"), kebab_chanshare_cb, LV_EVENT_CLICKED, NULL);
  }
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

  lv_obj_t* bar = makeHeaderBar(_path_screen, "Out Path", path_back_cb);
  lv_obj_t* save = lv_btn_create(bar);   // flexes to the right of the grown title
  lv_obj_add_event_cb(save, path_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* svl = lv_label_create(save);
  lv_label_set_text(svl, LV_SYMBOL_OK);

  lv_obj_t* body = lv_obj_create(_path_screen);
  lv_obj_add_event_cb(body, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb
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
  _path_ta = makeSelTextarea(fp);
  lv_textarea_set_one_line(_path_ta, true); lv_obj_add_event_cb(_path_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_path_ta, LV_PCT(100));
  lv_obj_add_event_cb(_path_ta, path_ta_event_cb, LV_EVENT_ALL, NULL);

  _path_err = lv_label_create(body);
  lv_label_set_text(_path_err, "");
  lv_obj_set_style_text_color(_path_err, lv_color_hex(UI_ERROR), 0);
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

// ===== New-channel screen (create/join by name + base64 key) =============

// Parse a hex string (MeshCore channel keys are hex, e.g. "4dd75e...") into bytes,
// tolerating spaces/colons/dashes. Returns byte count, or -1 on a bad digit / odd
// length / overflow. 16 bytes (32 hex) = 128-bit key, 32 bytes (64 hex) = 256-bit.
static int hexToBytes(const char* s, uint8_t* out, int cap) {
  int n = 0, hi = -1;
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c == ' ' || c == ':' || c == '-' || c == '\n' || c == '\r' || c == '\t') continue;
    int v;
    if      (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else return -1;
    if (hi < 0) hi = v;
    else { if (n >= cap) return -1; out[n++] = (uint8_t)((hi << 4) | v); hi = -1; }
  }
  return hi < 0 ? n : -1;   // dangling nibble -> odd length -> invalid
}

// Minimal URL-decode (for a channel name pulled from a meshcore:// URI): %XX + '+'.
static void urlDecodeInto(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (const char* p = in; *p && o + 1 < cap; p++) {
    if (*p == '+') { out[o++] = ' '; }
    else if (*p == '%' && p[1] && p[2]) {
      auto hx = [](char c)->int{ return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; };
      int h = hx(p[1]), l = hx(p[2]);
      if (h >= 0 && l >= 0) { out[o++] = (char)((h << 4) | l); p += 2; }
      else out[o++] = *p;
    } else out[o++] = *p;
  }
  out[o] = 0;
}

// Pull a "key=value" param's value (up to '&' / end) out of a query string.
static bool uriParam(const char* uri, const char* key, char* out, size_t cap) {
  char pat[24]; snprintf(pat, sizeof(pat), "%s=", key);
  const char* s = strstr(uri, pat);
  if (!s) return false;
  s += strlen(pat);
  size_t o = 0;
  while (*s && *s != '&' && o + 1 < cap) out[o++] = *s++;
  out[o] = 0;
  return true;
}

// ===== Channel Details page (hexagon hero + editable name + key) =========
void UITask::buildChannelInfoScreen() {
  if (_chinfo_screen) return;
  _chinfo_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_chinfo_screen);
  lv_obj_set_style_pad_all(_chinfo_screen, 0, 0);
  makeHeaderBar(_chinfo_screen, "Channel", chinfo_back_cb);

  _chinfo_body = lv_obj_create(_chinfo_screen);
  lv_obj_add_event_cb(_chinfo_body, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_size(_chinfo_body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(_chinfo_body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(_chinfo_body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_chinfo_body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_chinfo_body, 0, 0);
  lv_obj_set_style_pad_all(_chinfo_body, 12, 0);
  lv_obj_set_style_pad_row(_chinfo_body, 8, 0);
  lv_obj_set_flex_flow(_chinfo_body, LV_FLEX_FLOW_COLUMN);

  makeHeroCard(_chinfo_body, &_chinfo_avatar, &_chinfo_avatar_lbl, &_chinfo_title, &_chinfo_key, chinfo_key_cb);

  lv_obj_t* fn = makeField(_chinfo_body, "Name");
  _chinfo_name_ta = makeSelTextarea(fn);
  lv_textarea_set_one_line(_chinfo_name_ta, true);
  lv_obj_add_event_cb(_chinfo_name_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_chinfo_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_chinfo_name_ta, chinfo_ta_event_cb, LV_EVENT_ALL, NULL);

  _chinfo_kb = lv_keyboard_create(_chinfo_screen);
  lv_obj_add_event_cb(_chinfo_kb, chinfo_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_chinfo_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::populateChannelInfo() {
  char nm[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(_chinfo_name[0] ? _chinfo_name : "(unnamed)", nm, sizeof(nm));
  lv_label_set_text(_chinfo_title, nm);
  brandChannelAvatar(_chinfo_avatar, _chinfo_avatar_lbl, _chinfo_name);
  lv_textarea_set_text(_chinfo_name_ta, _chinfo_name);
  if (_chinfo_is_hashtag) lv_obj_add_state(_chinfo_name_ta, LV_STATE_DISABLED);   // #tag: name fixed
  else                    lv_obj_clear_state(_chinfo_name_ta, LV_STATE_DISABLED);
  static const uint8_t zeros[16] = {0};
  int slen = (memcmp(&_chinfo_secret[16], zeros, 16) == 0) ? 16 : 32;   // 128- vs 256-bit
  char hex[2 * 32 + 1]; mesh::Utils::toHex(hex, _chinfo_secret, slen);
  char ktrunc[40];
  snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>  " LV_SYMBOL_COPY, hex, hex + 2 * slen - 6);
  lv_label_set_text(_chinfo_key, ktrunc);
}

void UITask::openChannelInfo(int channel_idx, lv_obj_t* return_screen) {
  ChannelDetails ch;
  if (!mproxy::getChannel(channel_idx, ch) || ch.name[0] == 0) return;
  buildChannelInfoScreen();
  memcpy(_chinfo_secret, ch.channel.secret, sizeof(_chinfo_secret));
  strncpy(_chinfo_name, ch.name, sizeof(_chinfo_name) - 1);
  _chinfo_name[sizeof(_chinfo_name) - 1] = 0;
  // A #hashtag channel's secret == SHA256("#"+name)[0:16]; its name IS the key, so
  // renaming is blocked (matches the phone app). Public etc. (independent PSK) rename freely.
  {
    const char* nm = _chinfo_name; while (*nm == '#' || *nm == ' ') nm++;
    uint8_t psk[16]; deriveHashtagPsk(nm, psk);
    static const uint8_t z[16] = {0};
    _chinfo_is_hashtag = (memcmp(psk, _chinfo_secret, 16) == 0 && memcmp(_chinfo_secret + 16, z, 16) == 0);
  }
  _chinfo_return_screen = return_screen;
  _chinfo_active_ta = NULL;
  lv_obj_add_flag(_chinfo_kb, LV_OBJ_FLAG_HIDDEN);
  populateChannelInfo();
  lv_obj_scroll_to_y(_chinfo_body, 0, LV_ANIM_OFF);
  lv_scr_load(_chinfo_screen);
}

void UITask::commitChannelName() {
  if (_chinfo_is_hashtag) return;   // #tag channel: name is the key, rename blocked
  const char* entered = lv_textarea_get_text(_chinfo_name_ta);
  if (!entered || !entered[0]) { lv_textarea_set_text(_chinfo_name_ta, _chinfo_name); return; }  // no blank names
  if (strcmp(entered, _chinfo_name) == 0) return;                                                // unchanged
  strncpy(_chinfo_name, entered, sizeof(_chinfo_name) - 1);                                      // optimistic
  _chinfo_name[sizeof(_chinfo_name) - 1] = 0;
  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::RenameChannel;
  memcpy(cmd.path, _chinfo_secret, sizeof(_chinfo_secret));   // identity = secret
  cmd.path_len = sizeof(_chinfo_secret);
  strncpy(cmd.name, _chinfo_name, sizeof(cmd.name) - 1);
  mproxy::postCommand(cmd);
  populateChannelInfo();   // refresh title + hexagon (name-seeded color may change)
}

void UITask::chinfo_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_chinfo_active_ta) { _instance->commitChannelName(); _instance->_chinfo_active_ta = NULL; }
  lv_obj_add_flag(_instance->_chinfo_kb, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(_instance->_chinfo_return_screen ? _instance->_chinfo_return_screen : _instance->_home_screen);
}

void UITask::chinfo_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  if (_instance->_chinfo_is_hashtag) return;   // #tag channel name is read-only -> no keyboard
  lv_obj_t* ta = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    _instance->_chinfo_active_ta = ta;
    lv_keyboard_set_textarea(_instance->_chinfo_kb, ta);
    lv_keyboard_set_mode(_instance->_chinfo_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_chinfo_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_chinfo_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_chinfo_kb);
    _instance->raiseFieldForKb(_instance->_chinfo_body, _instance->_chinfo_kb, ta);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitChannelName();
  }
}

void UITask::chinfo_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (_instance->_chinfo_active_ta) { _instance->commitChannelName(); _instance->_chinfo_active_ta = NULL; }
    lv_obj_add_flag(_instance->_chinfo_kb, LV_OBJ_FLAG_HIDDEN);
    _instance->resetKbScroll();
  }
}

void UITask::chinfo_key_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  static const uint8_t zeros[16] = {0};
  int slen = (memcmp(&_instance->_chinfo_secret[16], zeros, 16) == 0) ? 16 : 32;
  char hex[2 * 32 + 1]; mesh::Utils::toHex(hex, _instance->_chinfo_secret, slen);
  _instance->showKeyPopup(hex);
}

void UITask::kebab_chdetails_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->closeMenuPopup();
  _instance->openChannelInfo(_instance->_chat_channel_idx, _instance->_chat_screen);
}

void UITask::buildNewChannelScreen() {
  if (_newchan_screen) return;
  _newchan_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_newchan_screen);
  lv_obj_set_style_pad_all(_newchan_screen, 0, 0);

  lv_obj_t* bar = makeHeaderBar(_newchan_screen, "New Channel", newchan_back_cb);
  lv_obj_t* save = lv_btn_create(bar);   // flexes to the right of the grown title
  lv_obj_add_event_cb(save, newchan_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* svl = lv_label_create(save);
  lv_label_set_text(svl, LV_SYMBOL_OK);

  lv_obj_t* body = lv_obj_create(_newchan_screen);
  lv_obj_add_event_cb(body, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb
  lv_obj_set_size(body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 12, 0);
  lv_obj_set_style_pad_row(body, 8, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

  // Live hexagon hero (branded like a built channel) -- updates as the name is typed.
  makeHeroCard(body, &_newchan_hero_av, &_newchan_hero_avl, &_newchan_hero_nm, &_newchan_hero_key, newchan_key_cb);

  lv_obj_t* fn = makeField(body, "Name");
  _newchan_name_ta = makeSelTextarea(fn);
  lv_textarea_set_one_line(_newchan_name_ta, true); lv_obj_add_event_cb(_newchan_name_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_newchan_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_newchan_name_ta, newchan_ta_event_cb, LV_EVENT_ALL, NULL);

  // "Public channel": derive the key from the name (anyone can join by name) instead
  // of pasting a secret. Auto-checks when the name is "Public".
  _newchan_public_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_newchan_public_chk, "Public channel (key from name)");
  lv_obj_set_style_text_color(_newchan_public_chk, lv_color_hex(UI_FG), 0);
  lv_obj_add_event_cb(_newchan_public_chk, newchan_public_cb, LV_EVENT_VALUE_CHANGED, NULL);

  _newchan_key_field = makeField(body, "Key (hex) or paste meshcore:// link");
  _newchan_key_ta = makeSelTextarea(_newchan_key_field);
  lv_textarea_set_one_line(_newchan_key_ta, true); lv_obj_add_event_cb(_newchan_key_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_newchan_key_ta, LV_PCT(100));
  lv_obj_add_event_cb(_newchan_key_ta, newchan_ta_event_cb, LV_EVENT_ALL, NULL);

  _newchan_err = lv_label_create(body);
  lv_label_set_text(_newchan_err, "");
  lv_obj_set_style_text_color(_newchan_err, lv_color_hex(UI_ERROR), 0);
  lv_obj_add_flag(_newchan_err, LV_OBJ_FLAG_HIDDEN);

  _newchan_kb = lv_keyboard_create(_newchan_screen);
  lv_obj_add_event_cb(_newchan_kb, newchan_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_newchan_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::openNewChannel() {
  buildNewChannelScreen();
  lv_textarea_set_text(_newchan_name_ta, "");
  lv_textarea_set_text(_newchan_key_ta, "");
  if (_newchan_public_chk) lv_obj_clear_state(_newchan_public_chk, LV_STATE_CHECKED);
  lv_obj_clear_state(_newchan_key_ta, LV_STATE_DISABLED);
  lv_obj_add_flag(_newchan_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_newchan_kb, LV_OBJ_FLAG_HIDDEN);
  refreshNewChannelHero();
  lv_scr_load(_newchan_screen);
}

// Live hexagon hero + "Public channel" key handling. Auto-checks Public for the
// name "Public"; when public, derives the key from the name and shows it read-only.
void UITask::refreshNewChannelHero() {
  if (!_newchan_hero_nm) return;
  const char* name = lv_textarea_get_text(_newchan_name_ta);
  bool pub = _newchan_public_chk && lv_obj_has_state(_newchan_public_chk, LV_STATE_CHECKED);

  char keyhex[2 * 32 + 1] = "";
  if (pub) {
    if (name && name[0]) { uint8_t psk[16]; derivePublicChannelKey(name, psk); mesh::Utils::toHex(keyhex, psk, 16); }
    lv_textarea_set_text(_newchan_key_ta, keyhex);            // derived, shown read-only
    lv_obj_add_state(_newchan_key_ta, LV_STATE_DISABLED);
  } else {
    lv_obj_clear_state(_newchan_key_ta, LV_STATE_DISABLED);
    const char* k = lv_textarea_get_text(_newchan_key_ta);
    uint8_t sec[32]; int len = hexToBytes(k ? k : "", sec, sizeof(sec));
    if (len == 16 || len == 32) mesh::Utils::toHex(keyhex, sec, len);
  }

  const char* shown = (name && name[0]) ? name : "New channel";
  char clean[CHAT_PEER_NAME_MAX + 4]; sanitizeForFont(shown, clean, sizeof(clean));
  lv_label_set_text(_newchan_hero_nm, clean);
  brandChannelAvatar(_newchan_hero_av, _newchan_hero_avl, shown);
  if (keyhex[0]) {
    int kl = (int)strlen(keyhex);
    char trunc[24]; snprintf(trunc, sizeof(trunc), "<%.6s...%.6s>", keyhex, keyhex + kl - 6);
    lv_label_set_text(_newchan_hero_key, trunc);
  } else {
    lv_label_set_text(_newchan_hero_key, pub ? "(enter a name)" : "(enter a key)");
  }
}

void UITask::newchan_public_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->refreshNewChannelHero();
}

void UITask::newchan_key_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  const char* name = lv_textarea_get_text(_instance->_newchan_name_ta);
  bool pub = _instance->_newchan_public_chk && lv_obj_has_state(_instance->_newchan_public_chk, LV_STATE_CHECKED);
  char keyhex[2 * 32 + 1] = "";
  if (pub) { if (name && name[0]) { uint8_t psk[16]; derivePublicChannelKey(name, psk); mesh::Utils::toHex(keyhex, psk, 16); } }
  else {
    const char* k = lv_textarea_get_text(_instance->_newchan_key_ta);
    uint8_t sec[32]; int len = hexToBytes(k ? k : "", sec, sizeof(sec));
    if (len == 16 || len == 32) mesh::Utils::toHex(keyhex, sec, len);
  }
  if (keyhex[0]) _instance->showKeyPopup(keyhex);
}

bool UITask::createChannelFromForm() {
  auto fail = [this](const char* m){
    lv_label_set_text(_newchan_err, m);
    lv_obj_clear_flag(_newchan_err, LV_OBJ_FLAG_HIDDEN);
  };

  char name[32];
  snprintf(name, sizeof(name), "%s", lv_textarea_get_text(_newchan_name_ta));
  uint8_t secret[32] = {0};
  int len = 0;
  // "Public" is always the well-known public channel even if Save was hit before the
  // name field defocused (which is what normally auto-checks the box).
  bool pub = (_newchan_public_chk && lv_obj_has_state(_newchan_public_chk, LV_STATE_CHECKED))
             || isPublicName(name);

  if (pub) {
    // Key derived from the name (Public -> legacy PSK, else SHA256("#"+name)).
    if (!name[0]) { fail("Enter a channel name"); return false; }
    uint8_t psk[16]; derivePublicChannelKey(name, psk);
    memcpy(secret, psk, 16); len = 16;
  } else {
    // The key field also accepts a pasted "meshcore://channel/add?name=..&secret=hex"
    // link (the phone app's Share Channel format): pull name+secret out of it.
    const char* keyIn = lv_textarea_get_text(_newchan_key_ta);
    char keyhex[80];
    snprintf(keyhex, sizeof(keyhex), "%s", keyIn ? keyIn : "");
    if (keyIn && strstr(keyIn, "channel/add")) {
      char sec[80], nm[64];
      if (uriParam(keyIn, "secret", sec, sizeof(sec))) snprintf(keyhex, sizeof(keyhex), "%s", sec);
      if ((!name[0]) && uriParam(keyIn, "name", nm, sizeof(nm))) urlDecodeInto(nm, name, sizeof(name));
    }
    if (!name[0])   { fail("Enter a channel name"); return false; }
    if (!keyhex[0]) { fail("Enter a hex key");      return false; }
    len = hexToBytes(keyhex, secret, sizeof(secret));
    if (len != 16 && len != 32) { fail("Key must be hex (32 or 64 chars)"); return false; }
  }

  // Reject a duplicate: a channel with this secret is already added (matches the
  // phone app -- includes re-adding "Public" or a #tag you already have).
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails ch;
    if (!mproxy::getChannel(i, ch) || ch.name[0] == 0) continue;
    if (memcmp(ch.channel.secret, secret, len) == 0) { fail("Channel already added"); return false; }
  }

  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::AddChannel;
  strncpy(cmd.name, name, sizeof(cmd.name) - 1);   // channel name
  memcpy(cmd.path, secret, len);                   // raw secret bytes
  cmd.path_len = (uint8_t)len;
  mproxy::postCommand(cmd);
  showToast("Channel added");
  return true;   // appears in the list once the backend republishes the snapshot
}

void UITask::newchan_open_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->openNewChannel();
}

// Long-press a channel -> show its shareable QR (meshcore://channel/add?...), matching
// the phone app's "Share Channel" format so a phone can scan it to join.
void UITask::openShareChannelQR(int idx) {
  ChannelDetails ch;
  if (!mproxy::getChannel(idx, ch) || ch.name[0] == 0) return;
  buildShareQRScreen();
  _qr_return_screen = _home_screen;

  char sname[36];
  sanitizeForFont(ch.name, sname, sizeof(sname));
  lv_label_set_text(_qr_name_lbl, sname);

  // 128-bit key unless the upper 16 secret bytes are non-zero (mirrors setChannel).
  static const uint8_t zeros[16] = {0};
  int slen = (memcmp(&ch.channel.secret[16], zeros, 16) == 0) ? 16 : 32;
  char hex[2 * 32 + 1];
  mesh::Utils::toHex(hex, ch.channel.secret, slen);
  char ktrunc[24];
  snprintf(ktrunc, sizeof(ktrunc), "<%.6s...%.6s>", hex, hex + 2 * slen - 6);
  lv_label_set_text(_qr_key_lbl, ktrunc);

  char ename[3 * 32 + 1];
  urlEncode(ch.name, ename, sizeof(ename));
  char uri[64 + sizeof(ename) + 2 * 32];
  snprintf(uri, sizeof(uri), "meshcore://channel/add?name=%s&secret=%s", ename, hex);
  lv_qrcode_update(_qr_code, uri, strlen(uri));
  lv_scr_load(_qr_screen);
}

void UITask::kebab_chanshare_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  int idx = _instance->_chat_channel_idx;
  _instance->closeMenuPopup();
  _instance->openShareChannelQR(idx);
}

void UITask::newchan_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  lv_obj_add_flag(_instance->_newchan_kb, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(_instance->_home_screen);
}

void UITask::newchan_save_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->createChannelFromForm()) {
    lv_obj_add_flag(_instance->_newchan_kb, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(_instance->_home_screen);
  }
}

void UITask::newchan_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* ta = lv_event_get_target(e);
  if (code == LV_EVENT_VALUE_CHANGED) { _instance->refreshNewChannelHero(); return; }   // live preview only
  if (code == LV_EVENT_DEFOCUSED) {
    // Only when focus LEAVES the name field: auto-check Public for the name "Public"
    // (so mid-typing "publicity" doesn't transiently trip it).
    if (ta == _instance->_newchan_name_ta && isPublicName(lv_textarea_get_text(ta)) &&
        _instance->_newchan_public_chk && !lv_obj_has_state(_instance->_newchan_public_chk, LV_STATE_CHECKED)) {
      lv_obj_add_state(_instance->_newchan_public_chk, LV_STATE_CHECKED);
      _instance->refreshNewChannelHero();
    }
    return;
  }
  // The key field is read-only while "Public channel" derives it -> no keyboard.
  if (ta == _instance->_newchan_key_ta && lv_obj_has_state(ta, LV_STATE_DISABLED)) return;
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_newchan_kb, ta);
    lv_keyboard_set_mode(_instance->_newchan_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_newchan_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_newchan_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_newchan_kb);
    _instance->raiseFieldForKb(lv_obj_get_parent(lv_obj_get_parent(ta)), _instance->_newchan_kb, ta);
  }
}

void UITask::newchan_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(_instance->_newchan_kb, LV_OBJ_FLAG_HIDDEN);
    _instance->resetKbScroll();
  }
}

// ----- Add a contact: the Contact Info screen in CINFO_ADD mode ----------------
// "Adding a contact" is not its own screen -- it's buildContactInfoScreen() driven
// in CINFO_ADD mode, so the hero, Name field, key popup, keyboard, and styling are
// the exact same widgets a viewed contact uses (UX stays consistent, no fork).
//
// The Key field is bare hex only (typed, pasted via long-press, or prefilled and
// locked). meshcore:// links are decoded by the chat-display layer into a contact
// ref before they ever reach here. resolveContactKey() parses the box into the
// prospective contact (_cinfo_c) and reports whether we now hold a full key.
bool UITask::resolveContactKey() {
  if (_cinfo_addkey_locked) { _cinfo_haskey = true; return true; }   // prefilled key already in _cinfo_c
  const char* keyIn = _cinfo_key_ta ? lv_textarea_get_text(_cinfo_key_ta) : "";
  uint8_t pk[PUB_KEY_SIZE];
  bool ok = (hexToBytes(keyIn ? keyIn : "", pk, PUB_KEY_SIZE) == PUB_KEY_SIZE);
  if (ok) memcpy(_cinfo_c.id.pub_key, pk, PUB_KEY_SIZE);
  _cinfo_haskey = ok;
  return ok;
}

void UITask::openNewContact(lv_obj_t* return_screen) {
  buildContactInfoScreen();
  _cinfo_return_screen = return_screen;
  _cinfo_mode = CINFO_ADD;
  _cinfo_addkey_locked = false;
  _cinfo_haskey = false;
  _cinfo_valid = true;                    // _cinfo_c is the working prospective contact
  _cinfo_c = ContactInfo();
  _cinfo_c.type = ADV_TYPE_CHAT;
  _cinfo_c.out_path_len = OUT_PATH_UNKNOWN;
  _cinfo_override[0] = 0;
  memset(_cinfo_pubkey, 0, PUB_KEY_SIZE);
  _cinfo_active_ta = NULL;
  lv_textarea_set_text(_cinfo_name_ta, "");
  lv_textarea_set_text(_cinfo_key_ta, "");
  lv_obj_add_flag(_cinfo_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  applyCinfoMode();
  populateContactInfo();
  lv_obj_scroll_to_y(_cinfo_body, 0, LV_ANIM_OFF);
  lv_scr_load(_cinfo_screen);
}

void UITask::openNewContactPrefilled(const uint8_t* pubkey, uint8_t type, const char* advname,
                                     lv_obj_t* return_screen) {
  buildContactInfoScreen();
  _cinfo_return_screen = return_screen;
  _cinfo_mode = CINFO_ADD;
  _cinfo_addkey_locked = true;            // key came from a contact ref -> read-only
  _cinfo_haskey = true;
  _cinfo_valid = true;
  _cinfo_c = ContactInfo();
  memcpy(_cinfo_c.id.pub_key, pubkey, PUB_KEY_SIZE);
  memcpy(_cinfo_pubkey, pubkey, PUB_KEY_SIZE);
  _cinfo_c.type = type;
  _cinfo_c.out_path_len = OUT_PATH_UNKNOWN;
  strncpy(_cinfo_c.name, advname ? advname : "", sizeof(_cinfo_c.name) - 1);
  _cinfo_c.name[sizeof(_cinfo_c.name) - 1] = 0;
  _cinfo_override[0] = 0;
  _cinfo_active_ta = NULL;
  lv_textarea_set_text(_cinfo_name_ta, _cinfo_c.name);   // editable display name
  char hex[2 * PUB_KEY_SIZE + 1];
  mesh::Utils::toHex(hex, pubkey, PUB_KEY_SIZE);
  lv_textarea_set_text(_cinfo_key_ta, hex);              // whole key, shown but read-only
  lv_obj_add_flag(_cinfo_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
  applyCinfoMode();
  populateContactInfo();
  lv_obj_scroll_to_y(_cinfo_body, 0, LV_ANIM_OFF);
  lv_scr_load(_cinfo_screen);
}

// Add-mode Save: build the contact from the form and post AddContact (+ a
// SetNameOvr when the typed name differs from the advertised/linked baseline).
bool UITask::saveContactFromForm() {
  auto fail = [this](const char* m){ lv_label_set_text(_cinfo_err, m); lv_obj_clear_flag(_cinfo_err, LV_OBJ_FLAG_HIDDEN); };
  if (!resolveContactKey()) { fail("Key must be 64 hex chars"); return false; }

  const char* advname = _cinfo_c.name;     // advertised/linked baseline (prefill); "" for manual entry
  const char* typed = lv_textarea_get_text(_cinfo_name_ta);
  const char* base = advname[0] ? advname : typed;   // advertised name is the stored contact name
  if (!base || !base[0]) { fail("Enter a name"); return false; }

  mproxy::MeshCmd cmd{};
  cmd.kind = mproxy::CmdKind::AddContact;
  ContactInfo& c = cmd.contact;
  memcpy(c.id.pub_key, _cinfo_c.id.pub_key, PUB_KEY_SIZE);
  c.type = _cinfo_c.type;
  strncpy(c.name, base, sizeof(c.name) - 1);
  c.out_path_len = OUT_PATH_UNKNOWN;
  c.lastmod = mproxy::rtcSeconds();
  mproxy::postCommand(cmd);

  // A name edited beyond the advertised name is a local override; the contact keeps
  // its advert name and displayName() prefers the override.
  if (advname[0] && typed && typed[0] && strcmp(typed, advname) != 0) {
    mproxy::MeshCmd ov{};
    ov.kind = mproxy::CmdKind::SetNameOvr;
    memcpy(ov.pubkey, _cinfo_c.id.pub_key, PUB_KEY_SIZE);
    strncpy(ov.name, typed, sizeof(ov.name) - 1);
    mproxy::postCommand(ov);
  }
  showToast("Contact added");
  return true;   // appears in the list once the backend republishes the snapshot
}

void UITask::cinfo_save_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->saveContactFromForm()) {
    lv_obj_add_flag(_instance->_cinfo_kb, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(_instance->_cinfo_return_screen ? _instance->_cinfo_return_screen : _instance->_home_screen);
  }
}

// Contacts tab lead row: open the New Contact screen for manual entry.
void UITask::clistNewContact() {
  if (_instance) _instance->openNewContact(_instance->_home_screen);
}

// Add an inline show/hide eye to a one-line textarea: hidden (password) by default,
// the small mono eye sits ~2px from the right edge (in reserved padding so text clears).
lv_obj_t* UITask::attachInlineEye(lv_obj_t* ta) {
  lv_textarea_set_password_mode(ta, true);
  lv_obj_set_style_pad_right(ta, 20, 0);
  lv_obj_t* eye = lv_label_create(ta);
  lv_label_set_text(eye, LV_SYMBOL_EYE_CLOSE);          // closed = currently hidden
  lv_obj_set_style_text_font(eye, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(eye, lv_color_hex(DIM_HEX), 0);
  lv_obj_align(eye, LV_ALIGN_RIGHT_MID, 18, 0);         // nudged into the padding, flush to the edge
  lv_obj_add_flag(eye, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(eye, ta);
  lv_obj_add_event_cb(eye, pw_eye_cb, LV_EVENT_CLICKED, NULL);
  return eye;
}

void UITask::pw_eye_cb(lv_event_t* e) {
  lv_obj_t* eye = lv_event_get_target(e);
  lv_obj_t* ta = (lv_obj_t*)lv_obj_get_user_data(eye);
  if (!ta) return;
  bool nowHidden = !lv_textarea_get_password_mode(ta);
  lv_textarea_set_password_mode(ta, nowHidden);
  lv_label_set_text(eye, nowHidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

// ===== Repeater / room-server login (popup over the originating screen) ====

void UITask::buildLoginPopup() {
  if (_login_popup) return;
  _login_card = makeModalCard(&_login_popup, login_dismiss_cb);  // tap outside = cancel
  lv_obj_align(_login_card, LV_ALIGN_TOP_MID, 0, HEADER_H + 8);   // top -> room for the keyboard below
  lv_obj_add_event_cb(_login_card, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap card empty -> hide kb
  lv_obj_add_flag(_login_popup, LV_OBJ_FLAG_HIDDEN);

  _login_title = lv_label_create(_login_card);
  lv_obj_set_style_text_color(_login_title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_login_title, fontHeading(), 0);
  lv_label_set_text(_login_title, "Login");

  // Password row: [field with inline eye] [send].
  lv_obj_t* prow = lv_obj_create(_login_card);
  makePassive(prow);
  lv_obj_set_width(prow, LV_PCT(100));
  lv_obj_set_height(prow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(prow, 0, 0);
  lv_obj_set_style_pad_all(prow, 0, 0);
  lv_obj_set_style_pad_column(prow, 6, 0);
  lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  _login_pw_ta = makeSelTextarea(prow);
  lv_textarea_set_one_line(_login_pw_ta, true); lv_obj_add_event_cb(_login_pw_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_flex_grow(_login_pw_ta, 1);
  lv_obj_add_event_cb(_login_pw_ta, login_ta_event_cb, LV_EVENT_ALL, NULL);
  attachInlineEye(_login_pw_ta);                         // hidden by default + inline eye toggle

  lv_obj_t* go = lv_btn_create(prow);                    // send / done, at the end of the field
  lv_obj_add_event_cb(go, login_go_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* gl = lv_label_create(go);
  lv_label_set_text(gl, LV_SYMBOL_OK);
  lv_obj_center(gl);

  // Checkbox row: Save login | Auto-login.
  lv_obj_t* crow = lv_obj_create(_login_card);
  lv_obj_set_width(crow, LV_PCT(100));
  lv_obj_set_height(crow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(crow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(crow, 0, 0);
  lv_obj_set_style_pad_all(crow, 0, 0);
  lv_obj_set_style_pad_column(crow, 16, 0);
  lv_obj_clear_flag(crow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
  _login_save_chk = lv_checkbox_create(crow);
  lv_checkbox_set_text(_login_save_chk, "Save login");
  lv_obj_set_style_text_color(_login_save_chk, lv_color_hex(FG_HEX), 0);
  _login_auto_chk = lv_checkbox_create(crow);
  lv_checkbox_set_text(_login_auto_chk, "Auto-login");
  lv_obj_set_style_text_color(_login_auto_chk, lv_color_hex(FG_HEX), 0);

  _login_kb = lv_keyboard_create(_login_popup);
  lv_obj_add_event_cb(_login_kb, login_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_login_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::openLogin(const uint8_t* pubkey6, const char* name) {
  buildLoginPopup();
  if (pubkey6) memcpy(_login_pubkey, pubkey6, 6);
  char sn[CHAT_PEER_NAME_MAX], t[CHAT_PEER_NAME_MAX + 12];
  sanitizeForFont(name && name[0] ? name : "server", sn, sizeof(sn));
  snprintf(t, sizeof(t), "Login: %s", sn);
  lv_label_set_text(_login_title, t);
  lv_textarea_set_text(_login_pw_ta, "");
  lv_obj_clear_state(_login_save_chk, LV_STATE_CHECKED);
  lv_obj_clear_state(_login_auto_chk, LV_STATE_CHECKED);
  lv_obj_add_flag(_login_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_login_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_login_popup);
}

void UITask::kebab_login_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  uint8_t pk[6]; memcpy(pk, _instance->_chat_pubkey, 6);
  char nm[CHAT_PEER_NAME_MAX]; strncpy(nm, _instance->_chat_peer, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
  _instance->closeMenuPopup();
  _instance->openLogin(pk, nm);
}

void UITask::login_dismiss_cb(lv_event_t* e) {
  if (!_instance) return;
  if (lv_event_get_target(e) != _instance->_login_popup) return;  // ignore clicks on the card
  lv_obj_add_flag(_instance->_login_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_instance->_login_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::login_go_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  const char* pw = lv_textarea_get_text(_instance->_login_pw_ta);
  bool save  = lv_obj_has_state(_instance->_login_save_chk, LV_STATE_CHECKED);
  bool autol = lv_obj_has_state(_instance->_login_auto_chk, LV_STATE_CHECKED);
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::ServerLogin;
  memcpy(c.pubkey, _instance->_login_pubkey, 6);
  strncpy(c.password, pw ? pw : "", sizeof(c.password) - 1);
  c.password[sizeof(c.password) - 1] = 0;
  c.save_login = save;
  c.auto_login = autol;                       // backend saves if either is set
  mproxy::postCommand(c);
  lv_obj_add_flag(_instance->_login_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_instance->_login_popup, LV_OBJ_FLAG_HIDDEN);
  _instance->showToast("Logging in...");      // result arrives via EvKind::LoginResult
}

void UITask::login_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_login_kb, _instance->_login_pw_ta);
    lv_keyboard_set_mode(_instance->_login_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_login_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_login_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_login_kb);
  }
}

void UITask::login_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    lv_obj_add_flag(_instance->_login_kb, LV_OBJ_FLAG_HIDDEN);
}

// ===== Node-info / status screen =========================================

void UITask::buildNodeInfoScreen() {
  if (_nodeinfo_screen) return;
  _nodeinfo_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_nodeinfo_screen);
  lv_obj_set_style_pad_all(_nodeinfo_screen, 0, 0);

  makeHeaderBar(_nodeinfo_screen, "Node Info", nodeinfo_back_cb);

  lv_obj_t* body = lv_obj_create(_nodeinfo_screen);
  lv_obj_set_size(body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(body, 0, 0);
  lv_obj_set_style_pad_all(body, 12, 0);

  _nodeinfo_lbl = lv_label_create(body);
  lv_obj_set_width(_nodeinfo_lbl, LV_PCT(100));
  lv_label_set_long_mode(_nodeinfo_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(_nodeinfo_lbl, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_nodeinfo_lbl, &lv_font_montserrat_14, 0);
  lv_label_set_text(_nodeinfo_lbl, "");
}

void UITask::refreshNodeInfo() {
  if (!_nodeinfo_lbl) return;
  NodeStats st;
  mproxy::getStats(st);

  uint32_t up = st.uptime_secs;
  uint32_t d = up / 86400, h = (up % 86400) / 3600, m = (up % 3600) / 60, sec = up % 60;
  uint32_t freeRam = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
  uint32_t minRam  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
  uint32_t freePs  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;

  char keyhex[2 * PUB_KEY_SIZE + 1] = "";
  if (mproxy::selfPubKey()) mesh::Utils::toHex(keyhex, mproxy::selfPubKey(), PUB_KEY_SIZE);

  const NodePrefs* p = _node_prefs;
  char buf[700];
  int n = 0;
#ifndef LVGL_GUI_VERSION
  #define LVGL_GUI_VERSION "v?"
#endif
#ifndef FW_GIT_REV
  #define FW_GIT_REV "nogit"
#endif
  n += snprintf(buf + n, sizeof(buf) - n, "MeshCore: %s\n", mproxy::firmwareVersion());
  n += snprintf(buf + n, sizeof(buf) - n, "LVGL-GUI: %s (%s, %s)\n", LVGL_GUI_VERSION, FW_GIT_REV, __DATE__);
  n += snprintf(buf + n, sizeof(buf) - n, "Name: %s\n", p ? p->node_name : "?");
  n += snprintf(buf + n, sizeof(buf) - n, "Key: %.16s...\n", keyhex);
  if (p) n += snprintf(buf + n, sizeof(buf) - n, "Radio: %g MHz  SF%u  BW%g  CR%u  %ddBm\n",
                       p->freq, p->sf, p->bw, p->cr, (int)p->tx_power_dbm);
  n += snprintf(buf + n, sizeof(buf) - n, "Uptime: %ud %uh %um %us\n", d, h, m, sec);
  n += snprintf(buf + n, sizeof(buf) - n, "Free RAM: %u KB (min %u)\n", freeRam, minRam);
  n += snprintf(buf + n, sizeof(buf) - n, "Free PSRAM: %u KB\n", freePs);
#ifdef HAS_SD_CARD
  n += snprintf(buf + n, sizeof(buf) - n, "SD: %s\n", SdSvc::ready() ? "ok" : "none");
#endif
  n += snprintf(buf + n, sizeof(buf) - n, "TX flood/direct: %u / %u\n", st.sent_flood, st.sent_direct);
  n += snprintf(buf + n, sizeof(buf) - n, "RX flood/direct: %u / %u\n", st.recv_flood, st.recv_direct);
  n += snprintf(buf + n, sizeof(buf) - n, "PHY sent/recv/err: %u / %u / %u\n", st.pkts_sent, st.pkts_recv, st.recv_errors);
  n += snprintf(buf + n, sizeof(buf) - n, "Last RSSI: %d dBm  SNR: %.2f dB\n", (int)st.last_rssi, st.last_snr_q4 / 4.0);
  n += snprintf(buf + n, sizeof(buf) - n, "Noise floor: %d\n", (int)st.noise_floor);
  n += snprintf(buf + n, sizeof(buf) - n, "Airtime TX/RX: %us / %us\n", st.tx_air_secs, st.rx_air_secs);
  n += snprintf(buf + n, sizeof(buf) - n, "Queue: %u   ErrFlags: 0x%02X", st.queue_len, st.err_flags);
  lv_label_set_text(_nodeinfo_lbl, buf);
}

void UITask::openNodeInfo() {
  buildNodeInfoScreen();
  refreshNodeInfo();
  if (!_nodeinfo_timer) _nodeinfo_timer = lv_timer_create(nodeinfo_timer_cb, 1000, this);
  else                  lv_timer_resume(_nodeinfo_timer);
  lv_scr_load(_nodeinfo_screen);
}

void UITask::nodeinfo_timer_cb(lv_timer_t* t) {
  UITask* self = static_cast<UITask*>(t->user_data);
  if (self) self->refreshNodeInfo();
}

void UITask::nodeinfo_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_nodeinfo_timer) lv_timer_pause(_instance->_nodeinfo_timer);  // stop refresh when hidden
  lv_scr_load(_instance->_home_screen);
}

void UITask::open_nodeinfo_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->openNodeInfo();
}

// ===== Settings tab ======================================================

// A section heading (accent label with an underline divider) in the settings list.
static void addSettingsSection(lv_obj_t* body, const char* title) {
  lv_obj_t* h = lv_label_create(body);
  lv_label_set_text(h, title);
  lv_obj_set_width(h, LV_PCT(100));
  lv_obj_set_style_text_color(h, lv_color_hex(UI_ACCENT), 0);  // blue-400 accent
  lv_obj_set_style_text_font(h, fontHeading(), 0);
  lv_obj_set_style_pad_top(h, 8, 0);
  lv_obj_set_style_pad_bottom(h, 4, 0);
  lv_obj_set_style_border_color(h, lv_color_hex(UI_BORDER), 0);  // gray-700 underline
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
  lv_obj_t* ta = UITask::makeSelTextarea(f);
  lv_textarea_set_one_line(ta, true); lv_obj_add_event_cb(ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_accepted_chars(ta, "0123456789.-");
  lv_obj_set_width(ta, LV_PCT(100));
  lv_obj_add_event_cb(ta, cb, LV_EVENT_ALL, NULL);
  return ta;
}

// ---- Settings: category launcher + per-category panes ---------------------
// The Settings tab is a launcher (owner-profile hero + category rows) plus one
// pane per category, all children of _tab_settings, shown one at a time. Each
// pane is its own three-band view (back/title bar + scrollable body). A single
// shared keyboard overlays whichever pane is active. Fields are still created
// once and loaded/saved by populateSettings() + the existing per-widget cbs.

lv_obj_t* UITask::makeSettingsPane(int idx, const char* title) {
  lv_obj_t* pane = lv_obj_create(_tab_settings);
  lv_obj_remove_style_all(pane);
  lv_obj_set_size(pane, LV_PCT(100), LV_PCT(100));
  lv_obj_align(pane, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pane, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* hdr = lv_obj_create(pane);
  lv_obj_remove_style_all(hdr);
  lv_obj_set_width(hdr, LV_PCT(100));
  lv_obj_set_height(hdr, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(hdr, 6, 0);
  lv_obj_set_style_pad_column(hdr, 6, 0);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  makeBackButton(hdr, settings_back_cb);
  lv_obj_t* t = lv_label_create(hdr);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(t, fontTitle(), 0);

  lv_obj_t* paneBody = lv_obj_create(pane);
  lv_obj_remove_style_all(paneBody);
  lv_obj_set_width(paneBody, LV_PCT(100));
  lv_obj_set_flex_grow(paneBody, 1);
  lv_obj_set_style_pad_all(paneBody, 12, 0);
  lv_obj_set_style_pad_row(paneBody, 8, 0);
  lv_obj_set_flex_flow(paneBody, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_event_cb(paneBody, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);

  _set_pane[idx] = pane;
  _set_pane_body[idx] = paneBody;
  return paneBody;
}

lv_obj_t* UITask::makeCategoryRow(lv_obj_t* parent, const char* icon, const char* title,
                                 const char* desc, int cat) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(row, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(row, 8, 0);
  lv_obj_set_style_pad_all(row, 10, 0);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(row, (void*)(intptr_t)cat);
  lv_obj_add_event_cb(row, category_row_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t* ic = lv_label_create(row);
  lv_label_set_text(ic, icon);
  lv_obj_set_style_text_color(ic, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_text_font(ic, fontTitle(), 0);

  lv_obj_t* col = lv_obj_create(row);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  // Base lv_obj is clickable by default; clear it so taps on the title/description
  // fall through to the row (the whole row is the link, not just the chevron).
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* tl = lv_label_create(col);
  lv_label_set_text(tl, title);
  lv_obj_set_style_text_color(tl, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(tl, fontHeading(), 0);
  lv_obj_t* dl = lv_label_create(col);
  lv_label_set_text(dl, desc);
  lv_obj_set_width(dl, LV_PCT(100));
  lv_label_set_long_mode(dl, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(dl, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(dl, fontCaption(), 0);

  lv_obj_t* chev = lv_label_create(row);
  lv_label_set_text(chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(chev, lv_color_hex(DIM_HEX), 0);
  return row;
}

// Show/hide the home logo+clock header. A settings pane has its own back+title
// bar, so we drop the logo header (it adds nothing on a back-button page and wastes
// scarce vertical space in landscape) and grow the tabview to fill the gap, putting
// the pane's title bar at the very top. Top-level tabs (no back button) keep it.
void UITask::setHomeChrome(bool show) {
  if (!_header || !_tabview) return;
  if (show) lv_obj_clear_flag(_header, LV_OBJ_FLAG_HIDDEN);
  else      lv_obj_add_flag(_header, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(_tabview, _screen_w, show ? _screen_h - HEADER_H : _screen_h);
  lv_obj_align(_tabview, LV_ALIGN_TOP_MID, 0, show ? HEADER_H : 0);
}

void UITask::showSettingsCategory(int cat) {
  if (cat < 0 || cat >= CAT_COUNT || !_set_pane[cat]) return;
  setHomeChrome(false);   // pane's own back+title bar takes the top
  if (_set_launcher) lv_obj_add_flag(_set_launcher, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < CAT_COUNT; i++) if (_set_pane[i]) lv_obj_add_flag(_set_pane[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_set_pane[cat], LV_OBJ_FLAG_HIDDEN);
  _set_active_pane = _set_pane_body[cat];
  if (_set_active_pane) lv_obj_scroll_to_y(_set_active_pane, 0, LV_ANIM_OFF);
  if (_set_kb) lv_obj_add_flag(_set_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::settingsBackToLauncher() {
  for (int i = 0; i < CAT_COUNT; i++) if (_set_pane[i]) lv_obj_add_flag(_set_pane[i], LV_OBJ_FLAG_HIDDEN);
  if (_set_kb) lv_obj_add_flag(_set_kb, LV_OBJ_FLAG_HIDDEN);
  _set_active_pane = NULL;
  if (_set_launcher) lv_obj_clear_flag(_set_launcher, LV_OBJ_FLAG_HIDDEN);
  setHomeChrome(true);    // back at a top-level view -> restore the logo header
}

void UITask::category_row_cb(lv_event_t* e) {
  if (!_instance) return;
  int cat = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
  if (cat == CAT_ABOUT) { _instance->openNodeInfo(); return; }
  _instance->showSettingsCategory(cat);
}

void UITask::profile_hero_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->openProfile(_instance->_home_screen);  // hero -> full-screen Profile
}

void UITask::settings_back_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->settingsBackToLauncher();
}

void UITask::settings_tab_changed_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_set_launcher || !_instance->_tabview) return;
  // Any tab switch lands on a top-level view: reset Settings to its launcher and
  // restore the header (also covers leaving a pane via the tab bar).
  _instance->settingsBackToLauncher();
}

// The owner profile as a full-screen "contact page for yourself": same header bar
// + hero-card design as Contact Info, but with only the owner-relevant edit
// options (name, position, share). Built once during buildSettingsTab so its
// fields are populated by populateSettings().
void UITask::buildProfileScreen() {
  if (_profile_screen) return;
  _profile_screen = lv_obj_create(NULL);
  styleAsDarkScreen(_profile_screen);
  lv_obj_set_style_pad_all(_profile_screen, 0, 0);

  makeHeaderBar(_profile_screen, "Profile", profile_back_cb);

  _profile_body = lv_obj_create(_profile_screen);
  lv_obj_add_event_cb(_profile_body, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb
  lv_obj_set_size(_profile_body, _screen_w, _screen_h - HEADER_H);
  lv_obj_align(_profile_body, LV_ALIGN_TOP_MID, 0, HEADER_H);
  lv_obj_set_style_bg_color(_profile_body, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_profile_body, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_profile_body, 0, 0);
  lv_obj_set_style_radius(_profile_body, 0, 0);
  lv_obj_set_style_pad_all(_profile_body, 12, 0);
  lv_obj_set_style_pad_row(_profile_body, 8, 0);
  lv_obj_set_flex_flow(_profile_body, LV_FLEX_FLOW_COLUMN);

  // Big hero card (avatar + name over the tappable "<pub..key>"), like any contact.
  makeHeroCard(_profile_body, &_prof_avatar, &_prof_avatar_lbl, &_prof_name, &_prof_key, profile_key_cb);

  // ===== Public Info (owner-only edit options) =====
  addSettingsSection(_profile_body, "Public Info");

  lv_obj_t* fn = makeField(_profile_body, "Name");
  _set_name_ta = makeSelTextarea(fn);
  lv_textarea_set_one_line(_set_name_ta, true); lv_obj_add_event_cb(_set_name_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_obj_set_width(_set_name_ta, LV_PCT(100));
  lv_obj_add_event_cb(_set_name_ta, set_name_ta_event_cb, LV_EVENT_ALL, NULL);

  // Position: editable lat/lon (degrees). Affects adverts only when sharing is on.
  lv_obj_t* fpos = makeField(_profile_body, "Position (lat, lon)");
  lv_obj_t* prow = lv_obj_create(fpos);
  makePassive(prow);
  lv_obj_set_width(prow, LV_PCT(100));
  lv_obj_set_height(prow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(prow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(prow, 0, 0);
  lv_obj_set_style_pad_all(prow, 0, 0);
  lv_obj_set_style_pad_column(prow, 6, 0);
  lv_obj_clear_flag(prow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(prow, LV_FLEX_FLOW_ROW);
  _set_lat_ta = makeSelTextarea(prow);
  lv_textarea_set_one_line(_set_lat_ta, true); lv_obj_add_event_cb(_set_lat_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_accepted_chars(_set_lat_ta, "0123456789.-");
  lv_obj_set_flex_grow(_set_lat_ta, 1);
  lv_obj_add_event_cb(_set_lat_ta, set_pos_ta_event_cb, LV_EVENT_ALL, NULL);
  _set_lon_ta = makeSelTextarea(prow);
  lv_textarea_set_one_line(_set_lon_ta, true); lv_obj_add_event_cb(_set_lon_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_accepted_chars(_set_lon_ta, "0123456789.-");
  lv_obj_set_flex_grow(_set_lon_ta, 1);
  lv_obj_add_event_cb(_set_lon_ta, set_pos_ta_event_cb, LV_EVENT_ALL, NULL);

  _set_sharepos = lv_checkbox_create(_profile_body);
  lv_checkbox_set_text(_set_sharepos, "Share position");
  lv_obj_set_style_text_color(_set_sharepos, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_sharepos, set_sharepos_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // Export our own contact as a scannable QR (reuses the share-QR screen).
  lv_obj_t* shareme = lv_btn_create(_profile_body);
  lv_obj_set_width(shareme, LV_PCT(100));
  lv_obj_add_event_cb(shareme, set_shareme_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* sml = lv_label_create(shareme);
  lv_label_set_text(sml, LV_SYMBOL_DOWNLOAD " Share my contact (QR)");
  lv_obj_center(sml);

  _profile_kb = lv_keyboard_create(_profile_screen);
  lv_obj_add_event_cb(_profile_kb, profile_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_profile_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::openProfile(lv_obj_t* return_screen) {
  buildProfileScreen();
  _profile_return_screen = return_screen;
  updateOwnerProfile();   // refresh hero (name/avatar/key) from current prefs
  if (_profile_kb) lv_obj_add_flag(_profile_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_scroll_to_y(_profile_body, 0, LV_ANIM_OFF);
  lv_scr_load(_profile_screen);
}

void UITask::profile_back_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_profile_kb) lv_obj_add_flag(_instance->_profile_kb, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(_instance->_profile_return_screen ? _instance->_profile_return_screen
                                                : _instance->_home_screen);
}

void UITask::profile_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    if (_instance->_set_active_ta == _instance->_set_name_ta) _instance->commitNodeName();
    else if (_instance->_set_active_ta == _instance->_set_lat_ta ||
             _instance->_set_active_ta == _instance->_set_lon_ta) _instance->commitPosition();
    _instance->_set_active_ta = NULL;
    lv_obj_add_flag(_instance->_profile_kb, LV_OBJ_FLAG_HIDDEN);
    _instance->resetKbScroll();
  }
}

void UITask::buildSettingsTab(lv_obj_t* parent) {
  // Launcher + per-category panes (see helpers above). Null the pane tables first
  // so a partial build can't leave dangling pointers.
  _set_active_pane = NULL;
  for (int i = 0; i < CAT_COUNT; i++) { _set_pane[i] = NULL; _set_pane_body[i] = NULL; }
  lv_obj_set_style_pad_all(_tab_settings, 0, 0);
  lv_obj_clear_flag(_tab_settings, LV_OBJ_FLAG_SCROLLABLE);

  _set_launcher = lv_obj_create(_tab_settings);
  lv_obj_remove_style_all(_set_launcher);
  lv_obj_set_size(_set_launcher, LV_PCT(100), LV_PCT(100));
  lv_obj_align(_set_launcher, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_pad_all(_set_launcher, 12, 0);
  lv_obj_set_style_pad_row(_set_launcher, 8, 0);
  lv_obj_set_flex_flow(_set_launcher, LV_FLEX_FLOW_COLUMN);
  lv_obj_add_event_cb(_set_launcher, dismiss_kb_cb, LV_EVENT_CLICKED, NULL);  // tap empty -> hide kb

  lv_obj_t* body = _set_launcher;   // owner-profile hero is added into the launcher

  // Owner profile hero: avatar + node name + key, same look as Contact Info. Tapping
  // it opens Node Info (your details/health). Populated live in populateSettings().
  lv_obj_t* prof = lv_obj_create(body);
  lv_obj_remove_style_all(prof);
  lv_obj_set_width(prof, LV_PCT(100));
  lv_obj_set_height(prof, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(prof, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(prof, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(prof, 8, 0);
  lv_obj_set_style_pad_all(prof, 10, 0);
  lv_obj_set_style_pad_column(prof, 12, 0);
  lv_obj_set_flex_flow(prof, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(prof, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(prof, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(prof, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(prof, profile_hero_cb, LV_EVENT_CLICKED, NULL);  // hero -> Profile settings

  _set_profile_avatar = lv_obj_create(prof);
  lv_obj_remove_style_all(_set_profile_avatar);
  lv_obj_set_size(_set_profile_avatar, 48, 48);
  lv_obj_set_style_radius(_set_profile_avatar, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(_set_profile_avatar, LV_OPA_COVER, 0);
  lv_obj_clear_flag(_set_profile_avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  _set_profile_avatar_lbl = lv_label_create(_set_profile_avatar);
  lv_obj_center(_set_profile_avatar_lbl);
  lv_obj_set_style_text_color(_set_profile_avatar_lbl, lv_color_hex(UI_ON_COLOR), 0);
  lv_obj_set_style_text_font(_set_profile_avatar_lbl, fontTitle(), 0);

  lv_obj_t* pcol = lv_obj_create(prof);
  lv_obj_remove_style_all(pcol);
  lv_obj_set_flex_grow(pcol, 1);
  lv_obj_set_height(pcol, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(pcol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(pcol, 2, 0);
  lv_obj_clear_flag(pcol, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);  // whole hero is the tap target
  _set_profile_name = lv_label_create(pcol);
  lv_obj_set_width(_set_profile_name, LV_PCT(100));
  lv_label_set_long_mode(_set_profile_name, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(_set_profile_name, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(_set_profile_name, fontHeading(), 0);
  _set_profile_key = lv_label_create(pcol);
  lv_obj_set_width(_set_profile_key, LV_PCT(100));
  lv_label_set_long_mode(_set_profile_key, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(_set_profile_key, lv_color_hex(DIM_HEX), 0);
  lv_obj_set_style_text_font(_set_profile_key, fontCaption(), 0);
  // Not interactive here -- the whole launcher card links to the Profile page;
  // the full key + copy lives on that page's hero.

  lv_obj_t* chev = lv_label_create(prof);
  lv_label_set_text(chev, LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(chev, lv_color_hex(DIM_HEX), 0);

  // Category launcher rows -> each drills into the matching pane.
  // (The owner hero above is the Profile entry -> pane 0, so no separate row.)
  makeCategoryRow(_set_launcher, LV_SYMBOL_WIFI,  "Radio & Routing", "Frequency, power, presets, mesh",      CAT_RADIO);
  makeCategoryRow(_set_launcher, LV_SYMBOL_GPS,   "Telemetry & GPS", "Telemetry sharing, GPS module",        CAT_TELEMETRY);
  makeCategoryRow(_set_launcher, LV_SYMBOL_BELL,  "Notifications",   "New-message alerts & sound",           CAT_NOTIFY);
  makeCategoryRow(_set_launcher, LV_SYMBOL_IMAGE, "Display & Time",  "Brightness, rotation, clock, avatars", CAT_DISPLAY);
  makeCategoryRow(_set_launcher, LV_SYMBOL_POWER, "Power & Lock",    "LoRa radio, PIN lock, reboot",         CAT_POWER);
  makeCategoryRow(_set_launcher, LV_SYMBOL_LIST,  "About",           "Device status & telemetry",           CAT_ABOUT);

  // Profile is a full-screen detail page (built below), not an in-tab pane; the
  // launcher hero opens it. The rest are in-tab category panes.
  makeSettingsPane(CAT_RADIO,     "Radio & Routing");
  makeSettingsPane(CAT_TELEMETRY, "Telemetry & GPS");
  makeSettingsPane(CAT_NOTIFY,    "Notifications");
  makeSettingsPane(CAT_DISPLAY,   "Display & Time");
  makeSettingsPane(CAT_POWER,     "Power & Lock");

  // Owner profile is its own full-screen contact page (see buildProfileScreen);
  // built here so its fields are populated by populateSettings() below.
  buildProfileScreen();

  body = _set_pane_body[CAT_RADIO];   // Radio & Routing
  // ===== Radio (edit fields, then a single Apply) =====
  // Header row: "Radio" title on the left, a "Preset" button on the right.
  lv_obj_t* rhdr = lv_obj_create(body);
  makePassive(rhdr);
  lv_obj_set_width(rhdr, LV_PCT(100));
  lv_obj_set_height(rhdr, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(rhdr, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(rhdr, 0, 0);
  lv_obj_set_style_pad_all(rhdr, 0, 0);
  lv_obj_set_style_pad_top(rhdr, 8, 0);
  lv_obj_set_style_pad_bottom(rhdr, 4, 0);
  lv_obj_set_style_border_color(rhdr, lv_color_hex(UI_BORDER), 0);
  lv_obj_set_style_border_side(rhdr, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(rhdr, 1, 0);
  lv_obj_clear_flag(rhdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(rhdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rhdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t* rtitle = lv_label_create(rhdr);
  lv_label_set_text(rtitle, "Radio");
  lv_obj_set_style_text_color(rtitle, lv_color_hex(UI_ACCENT), 0);
  lv_obj_set_style_text_font(rtitle, fontHeading(), 0);
  lv_obj_t* preset = lv_btn_create(rhdr);
  lv_obj_set_style_pad_hor(preset, 10, 0);
  lv_obj_set_style_pad_ver(preset, 4, 0);
  lv_obj_add_event_cb(preset, radio_preset_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* pl = lv_label_create(preset);
  lv_label_set_text(pl, LV_SYMBOL_LIST " Preset");
  lv_obj_center(pl);

  _set_freq_ta = makeNumberField(body, "Frequency (MHz)", set_radio_ta_event_cb);
  _set_bw_dd   = makeDropdownField(body, "Bandwidth (kHz)", BW_OPTS_STR);
  _set_sf_dd   = makeDropdownField(body, "Spreading factor", "5\n6\n7\n8\n9\n10\n11\n12");
  _set_cr_dd   = makeDropdownField(body, "Coding Rate", "5\n6\n7\n8");
  _set_txp_ta  = makeNumberField(body, "TX Power (dBm)", set_radio_ta_event_cb);

  lv_obj_t* apply = lv_btn_create(body);
  lv_obj_add_event_cb(apply, set_radio_apply_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* al = lv_label_create(apply);
  lv_label_set_text(al, LV_SYMBOL_OK " Apply Radio");

  // ===== Routing =====
  addSettingsSection(body, "Routing");
  lv_obj_t* fp = makeField(body, "Path hash mode");
  _set_path_dd = lv_dropdown_create(fp);
  lv_dropdown_set_options(_set_path_dd, "1 byte\n2 bytes\n3 bytes");
  lv_obj_set_width(_set_path_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_path_dd, set_pathmode_cb, LV_EVENT_VALUE_CHANGED, NULL);

  body = _set_pane_body[CAT_TELEMETRY];   // Telemetry & GPS
  // ===== Telemetry policy (who may request our telemetry) =====
  addSettingsSection(body, "Telemetry");
  static const char* TELEM_OPTS = "Deny\nAllow (flagged)\nAllow all";  // -> TELEM_MODE_DENY/ALLOW_FLAGS/ALLOW_ALL
  _set_telem_base_dd = makeDropdownField(body, "Base", TELEM_OPTS);
  lv_obj_set_user_data(_set_telem_base_dd, (void*)(intptr_t)0);
  lv_obj_add_event_cb(_set_telem_base_dd, set_telem_cb, LV_EVENT_VALUE_CHANGED, NULL);
  _set_telem_loc_dd = makeDropdownField(body, "Location", TELEM_OPTS);
  lv_obj_set_user_data(_set_telem_loc_dd, (void*)(intptr_t)1);
  lv_obj_add_event_cb(_set_telem_loc_dd, set_telem_cb, LV_EVENT_VALUE_CHANGED, NULL);
  _set_telem_env_dd = makeDropdownField(body, "Environment", TELEM_OPTS);
  lv_obj_set_user_data(_set_telem_env_dd, (void*)(intptr_t)2);
  lv_obj_add_event_cb(_set_telem_env_dd, set_telem_cb, LV_EVENT_VALUE_CHANGED, NULL);

  body = _set_pane_body[CAT_RADIO];   // back to Radio & Routing
  // ===== Advanced (mesh behaviour) =====
  addSettingsSection(body, "Advanced");
  _set_autoadd_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_autoadd_chk, "Auto-add contacts");
  lv_obj_set_style_text_color(_set_autoadd_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_autoadd_chk, set_autoadd_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* fah = makeField(body, "Max hops");
  _set_autoadd_hops_dd = lv_dropdown_create(fah);   // index 0..4 maps 1:1 to autoadd_max_hops
  lv_dropdown_set_options(_set_autoadd_hops_dd, "No limit\nDirect only\nUp to 1 hop\nUp to 2 hops\nUp to 3 hops");
  lv_obj_set_width(_set_autoadd_hops_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_autoadd_hops_dd, set_autoadd_hops_cb, LV_EVENT_VALUE_CHANGED, NULL);

  _set_rxboost_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_rxboost_chk, "RX boost gain (reboot)");
  lv_obj_set_style_text_color(_set_rxboost_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_rxboost_chk, set_rxboost_cb, LV_EVENT_VALUE_CHANGED, NULL);

  _set_multiack_ta = makeNumberField(body, "Extra ACKs", set_advnum_ta_event_cb);
  _set_rxdelay_ta  = makeNumberField(body, "RX delay (s)", set_advnum_ta_event_cb);
  _set_airtime_ta  = makeNumberField(body, "Airtime factor", set_advnum_ta_event_cb);

#if ENV_INCLUDE_GPS
  body = _set_pane_body[CAT_TELEMETRY];   // Telemetry & GPS
  // ===== GPS (optional module on the rear UART plug) =====
  addSettingsSection(body, "GPS");
  _set_gps_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_gps_chk, "Enable GPS (reboot)");
  lv_obj_set_style_text_color(_set_gps_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_gps_chk, set_gps_cb, LV_EVENT_VALUE_CHANGED, NULL);
  _set_gps_interval_ta = makeNumberField(body, "GPS interval (s)", set_advnum_ta_event_cb);
#endif

  body = _set_pane_body[CAT_NOTIFY];   // Notifications
  // ===== Notifications =====
  addSettingsSection(body, "Notifications");
  _set_notify_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_notify_chk, "New message alerts");
  lv_obj_set_style_text_color(_set_notify_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_notify_chk, set_notify_cb, LV_EVENT_VALUE_CHANGED, NULL);

  body = _set_pane_body[CAT_POWER];   // Power & Lock
  // ===== Power & Lock =====
  addSettingsSection(body, "Power & Lock");
  lv_obj_t* frd = makeField(body, "LoRa radio");
  _set_radio_sw = lv_switch_create(frd);
  lv_obj_add_event_cb(_set_radio_sw, set_radio_sw_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* setpinb = lv_btn_create(body);
  lv_obj_set_width(setpinb, LV_PCT(100));
  lv_obj_add_event_cb(setpinb, set_pin_btn_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* spl = lv_label_create(setpinb);
  lv_label_set_text(spl, LV_SYMBOL_KEYBOARD " Set lock PIN");
  lv_obj_center(spl);

  lv_obj_t* lockb = lv_btn_create(body);
  lv_obj_set_width(lockb, LV_PCT(100));
  lv_obj_add_event_cb(lockb, lock_now_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* lockl = lv_label_create(lockb);
  lv_label_set_text(lockl, LV_SYMBOL_SETTINGS " Lock screen");
  lv_obj_center(lockl);

  body = _set_pane_body[CAT_DISPLAY];   // Display & Time
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

  lv_obj_t* fr = makeField(body, "Rotation (restart)");
  _set_rot_dd = lv_dropdown_create(fr);
  lv_dropdown_set_options(_set_rot_dd, "0\n90\n180\n270");
  lv_obj_set_width(_set_rot_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_rot_dd, set_rot_cb, LV_EVENT_VALUE_CHANGED, NULL);

  lv_obj_t* fto = makeField(body, "Screen timeout");
  _set_screen_dd = lv_dropdown_create(fto);
  lv_dropdown_set_options(_set_screen_dd, "Never\n15 s\n30 s\n1 min\n2 min\n5 min");
  lv_obj_set_width(_set_screen_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_screen_dd, set_screen_cb, LV_EVENT_VALUE_CHANGED, NULL);

  addSettingsSection(body, "Time");
  // Local-time offset for the header clock. Entered in hours (decimals OK for
  // half/quarter-hour zones, e.g. 5.5, 5.75, -3.5); stored as minutes.
  _set_tz_ta = makeNumberField(body, "UTC offset (h)", set_tz_ta_event_cb);

  _set_clock_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_clock_chk, "12-hour clock");
  lv_obj_set_style_text_color(_set_clock_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_clock_chk, set_clock_cb, LV_EVENT_VALUE_CHANGED, NULL);

  addSettingsSection(body, "Appearance");
  // Contact avatar color scheme: our curated palette, or iOS-app parity (same
  // color as the phone app for a given name). See nameColor() in ui_theme.h.
  lv_obj_t* fav = makeField(body, "Avatar colors");
  _set_avatar_dd = lv_dropdown_create(fav);
  lv_dropdown_set_options(_set_avatar_dd, "Default\niOS app");
  lv_obj_set_width(_set_avatar_dd, LV_PCT(100));
  lv_obj_add_event_cb(_set_avatar_dd, set_avatar_cb, LV_EVENT_VALUE_CHANGED, NULL);

#ifdef HAS_SD_CARD
  _set_history_chk = lv_checkbox_create(body);
  lv_checkbox_set_text(_set_history_chk, "Save chat history");
  lv_obj_set_style_text_color(_set_history_chk, lv_color_hex(FG_HEX), 0);
  lv_obj_add_event_cb(_set_history_chk, set_history_cb, LV_EVENT_VALUE_CHANGED, NULL);
#endif

  body = _set_pane_body[CAT_POWER];   // reboot belongs under Power & Lock
  // Reboot button -- handy on battery, and a clean software restart (esp_restart)
  // vs the hardware RESET line.
  lv_obj_t* reboot = lv_btn_create(body);
  lv_obj_set_width(reboot, LV_PCT(100));
  lv_obj_set_style_bg_color(reboot, lv_color_hex(UI_DANGER), 0);
  lv_obj_add_event_cb(reboot, [](lv_event_t*){ esp_restart(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t* reboot_lbl = lv_label_create(reboot);
  lv_label_set_text(reboot_lbl, LV_SYMBOL_POWER " Reboot");
  lv_obj_center(reboot_lbl);

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

  updateOwnerProfile();


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

  if (_set_screen_dd) {
    const uint16_t secs[] = {0, 15, 30, 60, 120, 300};   // matches the options + set_screen_cb
    int idx = 0;
    for (int i = 0; i < (int)(sizeof(secs) / sizeof(secs[0])); i++)
      if (secs[i] == _node_prefs->screen_timeout_s) { idx = i; break; }
    lv_dropdown_set_selected(_set_screen_dd, idx);
  }

  if (_set_tz_ta) {
    char tb[16];
    snprintf(tb, sizeof(tb), "%g", _node_prefs->tz_offset_minutes / 60.0);
    lv_textarea_set_text(_set_tz_ta, tb);
  }
  if (_set_clock_chk) {
    if (_node_prefs->clock_12h) lv_obj_add_state(_set_clock_chk, LV_STATE_CHECKED);
    else                        lv_obj_clear_state(_set_clock_chk, LV_STATE_CHECKED);
  }
  if (_set_avatar_dd) lv_dropdown_set_selected(_set_avatar_dd, _node_prefs->avatar_palette ? 1 : 0);
  if (_set_history_chk) {
    if (_node_prefs->persist_history != 0) lv_obj_add_state(_set_history_chk, LV_STATE_CHECKED);  // 0xFF/1 = on
    else                                   lv_obj_clear_state(_set_history_chk, LV_STATE_CHECKED);
  }
  if (_set_notify_chk) {
    if (_node_prefs->notify_enable != 0) lv_obj_add_state(_set_notify_chk, LV_STATE_CHECKED);
    else                                 lv_obj_clear_state(_set_notify_chk, LV_STATE_CHECKED);
  }

  // Telemetry policy dropdowns (mode value maps 1:1 to the dropdown index).
  if (_set_telem_base_dd) lv_dropdown_set_selected(_set_telem_base_dd, _node_prefs->telemetry_mode_base <= 2 ? _node_prefs->telemetry_mode_base : 0);
  if (_set_telem_loc_dd)  lv_dropdown_set_selected(_set_telem_loc_dd,  _node_prefs->telemetry_mode_loc  <= 2 ? _node_prefs->telemetry_mode_loc  : 0);
  if (_set_telem_env_dd)  lv_dropdown_set_selected(_set_telem_env_dd,  _node_prefs->telemetry_mode_env  <= 2 ? _node_prefs->telemetry_mode_env  : 0);

  // Advanced
  if (_set_autoadd_chk) {
    if (!_node_prefs->manual_add_contacts) lv_obj_add_state(_set_autoadd_chk, LV_STATE_CHECKED);  // auto-add on == manual off
    else                                   lv_obj_clear_state(_set_autoadd_chk, LV_STATE_CHECKED);
  }
  if (_set_autoadd_hops_dd) lv_dropdown_set_selected(_set_autoadd_hops_dd, _node_prefs->autoadd_max_hops <= 4 ? _node_prefs->autoadd_max_hops : 0);
  if (_set_rxboost_chk) {
    if (_node_prefs->rx_boosted_gain) lv_obj_add_state(_set_rxboost_chk, LV_STATE_CHECKED);
    else                              lv_obj_clear_state(_set_rxboost_chk, LV_STATE_CHECKED);
  }
  char nb[24];
  if (_set_multiack_ta) { snprintf(nb, sizeof(nb), "%u", _node_prefs->multi_acks);    lv_textarea_set_text(_set_multiack_ta, nb); }
  if (_set_rxdelay_ta)  { snprintf(nb, sizeof(nb), "%g", _node_prefs->rx_delay_base);  lv_textarea_set_text(_set_rxdelay_ta, nb); }
  if (_set_airtime_ta)  { snprintf(nb, sizeof(nb), "%g", _node_prefs->airtime_factor); lv_textarea_set_text(_set_airtime_ta, nb); }

  if (_set_gps_chk) {
    if (_node_prefs->gps_enabled) lv_obj_add_state(_set_gps_chk, LV_STATE_CHECKED);
    else                          lv_obj_clear_state(_set_gps_chk, LV_STATE_CHECKED);
  }
  if (_set_gps_interval_ta) { snprintf(nb, sizeof(nb), "%u", _node_prefs->gps_interval); lv_textarea_set_text(_set_gps_interval_ta, nb); }

  if (_set_radio_sw) {
    if (_node_prefs->radio_off) lv_obj_clear_state(_set_radio_sw, LV_STATE_CHECKED);  // checked = radio on
    else                        lv_obj_add_state(_set_radio_sw, LV_STATE_CHECKED);
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
  updateOwnerProfile();   // refresh the profile hero (name + avatar)
  showToast("Name saved & advertised");
}

// Fill the Settings owner-profile hero from the current node name + self key. Same
// avatar scheme as the contact list (name-seeded color + first grapheme).
void UITask::updateOwnerProfile() {
  if (!_set_profile_name || !_node_prefs) return;
  const char* who = _node_prefs->node_name[0] ? _node_prefs->node_name : "(unnamed)";
  char clean[CHAT_PEER_NAME_MAX + 4];
  sanitizeForFont(who, clean, sizeof(clean));
  lv_label_set_text(_set_profile_name, clean);
  char g[8]; firstGrapheme(clean, g, sizeof(g));
  lv_label_set_text(_set_profile_avatar_lbl, g[0] ? g : "?");
  lv_obj_set_style_bg_color(_set_profile_avatar, lv_color_hex(nameColor(who)), 0);
  char keyhex[2 * PUB_KEY_SIZE + 1] = "";
  if (mproxy::selfPubKey()) mesh::Utils::toHex(keyhex, mproxy::selfPubKey(), PUB_KEY_SIZE);
  // Launcher card: plain "<aabbcc...ddeeff>" (the whole card just links to Profile).
  char plain[24];
  if (keyhex[0]) snprintf(plain, sizeof(plain), "<%.6s...%.6s>", keyhex, keyhex + 2 * PUB_KEY_SIZE - 6);
  else           plain[0] = 0;
  lv_label_set_text(_set_profile_key, plain);

  // Profile page hero: same owner identity, but its key line is tappable (full key
  // + copy), so it gets the copy glyph.
  if (_prof_name) {
    char snip[32];
    if (keyhex[0]) snprintf(snip, sizeof(snip), "<%.6s...%.6s>  " LV_SYMBOL_COPY, keyhex, keyhex + 2 * PUB_KEY_SIZE - 6);
    else           snip[0] = 0;
    lv_label_set_text(_prof_name, clean);
    lv_label_set_text(_prof_avatar_lbl, g[0] ? g : "?");
    lv_obj_set_style_bg_color(_prof_avatar, lv_color_hex(nameColor(who)), 0);
    lv_label_set_text(_prof_key, snip);
  }
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
    lv_keyboard_set_textarea(_instance->_profile_kb, ta);
    lv_keyboard_set_mode(_instance->_profile_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(_instance->_profile_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_profile_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_profile_kb);
    _instance->raiseFieldForKb(_instance->_profile_body, _instance->_profile_kb, ta);
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
    _instance->raiseFieldForKb(_instance->_set_active_pane ? _instance->_set_active_pane
                                                           : _instance->_tab_settings,
                               _instance->_set_kb, ta);
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
  _instance->_backlight_duty = duty;   // also the level restored on wake from idle-off
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

// Screen-timeout dropdown <-> seconds (index order must match the options string).
static const uint16_t SCREEN_TIMEOUT_SECS[] = {0, 15, 30, 60, 120, 300};
static constexpr int SCREEN_TIMEOUT_N = sizeof(SCREEN_TIMEOUT_SECS) / sizeof(SCREEN_TIMEOUT_SECS[0]);

void UITask::set_screen_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
  if (sel >= SCREEN_TIMEOUT_N) sel = 0;
  _instance->_node_prefs->screen_timeout_s = SCREEN_TIMEOUT_SECS[sel];
  _instance->_last_input_ms = millis();   // restart the idle countdown from now
  pushPrefs();
}

void UITask::clipSet(uint8_t kind, const char* text, const uint8_t* pubkey,
                     const char* name, uint8_t type) {
  if (!text) text = "";
  strncpy(_clip_text, text, sizeof(_clip_text) - 1);
  _clip_text[sizeof(_clip_text) - 1] = 0;
  _clip_kind = kind;
  if (pubkey) memcpy(_clip_pubkey, pubkey, PUB_KEY_SIZE);
  else        memset(_clip_pubkey, 0, PUB_KEY_SIZE);
  if (name) { strncpy(_clip_name, name, sizeof(_clip_name) - 1); _clip_name[sizeof(_clip_name) - 1] = 0; }
  else      _clip_name[0] = 0;
  _clip_type = type;
}

void UITask::copyToClipboard(const char* text) {
  clipSet(CLIP_PLAIN, text, nullptr, nullptr, 0);
}

// Semantic kind of a field, resolved by identity (no user_data juggling).
uint8_t UITask::fieldKindOf(lv_obj_t* ta) {
  if (ta == _chat_input) return FK_CHAT_COMPOSE;
  if (ta == _cinfo_key_ta || ta == _newchan_key_ta) return FK_HEX;
  if (ta == _cinfo_name_ta || ta == _set_name_ta || ta == _newchan_name_ta) return FK_NAME;
  return FK_PLAIN;
}

// What to actually insert when pasting the current clipboard into a field of the
// given kind (the smart-paste transform table).
const char* UITask::pasteTextFor(uint8_t field_kind) {
  if (_clip_kind == CLIP_CONTACT_REF) {
    switch (field_kind) {
      case FK_HEX: {
        static char hex[2 * PUB_KEY_SIZE + 1];
        mesh::Utils::toHex(hex, _clip_pubkey, PUB_KEY_SIZE);
        return hex;                                    // just the 64-hex key
      }
      case FK_NAME:
      case FK_PLAIN:        return _clip_name;          // just the contact name
      case FK_CHAT_COMPOSE:
      default:              return _clip_text;          // whole <hex:type:name> token (re-renders as a card)
    }
  }
  return _clip_text;                                    // CLIP_PLAIN -> verbatim everywhere
}

void UITask::insert_paste_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  if (_instance->_clip_kind != CLIP_EMPTY && _instance->_chat_input)
    lv_textarea_add_text(_instance->_chat_input,
                         _instance->pasteTextFor(_instance->fieldKindOf(_instance->_chat_input)));
  _instance->closeInsertPopup();
}

// =====================================================================
// Universal text selection -- Phase 1: read-only chat bubbles (recolor text
// labels + atomic contact cards). Long-press -> drag to widen -> release shows a
// floating Copy / Select All menu. Textareas, smart paste, word-select, and
// handle re-grab come in later phases.
// =====================================================================
static const lv_coord_t SEL_HANDLE_D = 12;   // grab-dot diameter

// Faithful replica of LVGL's recolor command state machine (misc/lv_txt.c
// _lv_txt_is_cmd), so we can map a native label selection (char-id range over the
// recolor MARKUP) back to the visible/logical text for copy.
namespace { enum { TCMD_WAIT, TCMD_PAR, TCMD_IN }; }
static bool selTxtIsCmd(uint8_t& st, char c) {
  bool ret = false;
  if (c == '#') {
    if (st == TCMD_WAIT)      { st = TCMD_PAR; ret = true; }
    else if (st == TCMD_PAR)  { st = TCMD_WAIT; }            // "##" -> escaped literal '#'
    else if (st == TCMD_IN)   { st = TCMD_WAIT; ret = true; }
  }
  if (st == TCMD_PAR) { if (c == ' ') st = TCMD_IN; ret = true; }
  return ret;
}

// Extract the visible substring for selection char-id range [lo,hi): walk the
// markup one codepoint at a time (matching get_letter_on's char-id space), skip
// recolor command chars, and collapse the escaped "##" to one '#'.
static void labelSelToLogical(const char* markup, uint32_t lo, uint32_t hi, char* out, size_t cap) {
  uint8_t st = TCMD_WAIT;
  uint32_t cid = 0;
  size_t o = 0;
  const char* p = markup;
  while (*p) {
    unsigned char c0 = (unsigned char)*p;
    uint32_t step = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
    bool cmd = selTxtIsCmd(st, (char)c0);
    if (!cmd && cid >= lo && cid < hi)
      for (uint32_t k = 0; k < step && p[k] && o + 1 < cap; k++) out[o++] = p[k];
    cid++;
    p += step;
  }
  if (cap) out[o < cap ? o : cap - 1] = 0;
}

uint32_t UITask::labelCharAt(lv_obj_t* lbl, lv_point_t abs_pt) {
  lv_area_t a; lv_obj_get_coords(lbl, &a);
  lv_point_t rel = { (lv_coord_t)(abs_pt.x - a.x1), (lv_coord_t)(abs_pt.y - a.y1) };
  return lv_label_get_letter_on(lbl, &rel);
}

// ----- Chip tap resolution -------------------------------------------------
// UTF-8 codepoints in [s, e).
static uint32_t cpCount(const char* s, const char* e) {
  uint32_t n = 0;
  while (s < e) { unsigned char c = (unsigned char)*s; s += (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1; n++; }
  return n;
}

// Map a markup char-id (what lv_label_get_letter_on returns -- codepoint index
// including recolor command chars) to a LOGICAL offset (count of visible
// codepoints before it), i.e. an index into the displayed text.
static uint32_t markupToLogical(const char* markup, uint32_t char_id) {
  uint8_t st = TCMD_WAIT;
  uint32_t cid = 0, loff = 0;
  const char* p = markup;
  while (*p && cid < char_id) {
    unsigned char c0 = (unsigned char)*p;
    uint32_t step = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
    if (!selTxtIsCmd(st, (char)c0)) loff++;
    cid++;
    p += step;
  }
  return loff;
}

// Find the chip (mention / hashtag) covering logical offset `off` in the source
// text. Logical text = plain 1:1, "@[name]" shown as "@name", "#name" as "#name".
// Returns true + kind + the bare name (no @/#).
static bool tokenAtLogical(const char* src, uint32_t off, bool* is_hashtag, char* name, size_t cap) {
  const char* p = src;
  uint32_t loff = 0;
  while (*p) {
    const char* at = strstr(p, "@[");
    const char* atclose = at ? strchr(at + 2, ']') : nullptr;
    if (at && !atclose) at = nullptr;
    const char* htend = nullptr;
    const char* ht = findHashtag(p, src, &htend);
    const char* tok = at; bool mention = (at != nullptr);
    if (ht && (!tok || ht < tok)) { tok = ht; mention = false; }
    if (!tok) return false;                              // only plain text remains

    uint32_t run = cpCount(p, tok);                      // plain text before the token
    if (off < loff + run) return false;                  // tap landed in plain text
    loff += run;

    const char* nm_s = mention ? at + 2 : tok + 1;       // bare name (after "@[" or "#")
    const char* nm_e = mention ? atclose : htend;
    uint32_t toklen = 1 + cpCount(nm_s, nm_e);           // shown as "@name" / "#name"
    if (off < loff + toklen) {
      size_t n = (size_t)(nm_e - nm_s);
      if (n > cap - 1) n = cap - 1;
      memcpy(name, nm_s, n); name[n] = 0;
      *is_hashtag = !mention;
      return true;
    }
    loff += toklen;
    p = mention ? atclose + 1 : htend;
  }
  return false;
}

// Act on a tapped chip: open the contact (mention) or open/join the channel (hashtag).
void UITask::resolveChip(uint8_t kind, const char* name) {
  if (kind == CHIP_HASHTAG) { openOrJoinHashtag(name); return; }
  // mention -> resolve the name to a local contact (exact match) and open it
  int total = mproxy::getNumContacts();
  ContactInfo c;
  for (int i = 0; i < total; i++) {
    if (!mproxy::getContactByIdx(i, c)) continue;
    if (strcmp(c.name, name) == 0) { openContactInfo(c.id.pub_key, _chat_screen); return; }
  }
  // unknown contact -> no action (the name renders highlighted but isn't a contact)
}

void UITask::makeLabelSelectable(lv_obj_t* lbl) {
  // Clickable so it receives press/long-press; EVENT_BUBBLE so a normal tap still
  // reaches the bubble's mention/resend handler (selection long-press does not, and
  // those handlers bail while a selection is active).
  lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_bg_color(lbl, lv_color_hex(UI_ACCENT), LV_PART_SELECTED);
  lv_obj_set_style_text_color(lbl, lv_color_hex(UI_BG), LV_PART_SELECTED);
  lv_obj_add_event_cb(lbl, sel_event_cb, LV_EVENT_ALL, NULL);
}

void UITask::makeCardSelectable(lv_obj_t* card) {
  lv_obj_add_event_cb(card, sel_event_cb, LV_EVENT_ALL, NULL);
}

void UITask::ensureSelHandles() {
  if (_sel.h_start) return;
  for (int i = 0; i < 2; i++) {
    lv_obj_t* h = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, SEL_HANDLE_D, SEL_HANDLE_D);
    lv_obj_set_style_bg_color(h, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);    // grab dot (triangle styling: TBD)
    lv_obj_add_flag(h, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_ext_click_area(h, 14);   // fat touch target around the small dot
    lv_obj_add_event_cb(h, sel_handle_cb, LV_EVENT_ALL, NULL);
    if (i == 0) _sel.h_start = h; else _sel.h_end = h;
  }
}

void UITask::beginLabelSel(lv_obj_t* lbl, lv_point_t abs_pt) {
  endSelection();
  _sel.kind = SEL_LABEL;
  _sel.target = lbl;
  _sel.whole_obj = false;
  uint32_t idx = labelCharAt(lbl, abs_pt);
  _sel.anchor = idx; _sel.sel_lo = idx; _sel.sel_hi = idx;
  if (_chat_history) lv_obj_clear_flag(_chat_history, LV_OBJ_FLAG_SCROLLABLE);  // drag selects, not scrolls
  applyLabelSel();
  ensureSelHandles();
  positionSelHandles();
  lv_obj_clear_flag(_sel.h_start, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_sel.h_end, LV_OBJ_FLAG_HIDDEN);
  _sel.state = SS_MARKING;
}

// Word boundaries (in markup char-id space) around char-id `idx`: the maximal run
// of visible non-whitespace codepoints containing it. Command chars and spaces
// break words. Multibyte (emoji/CJK) count as word chars.
static void labelWordAt(const char* markup, uint32_t idx, uint32_t* lo, uint32_t* hi) {
  static bool word[640];
  uint32_t n = 0;
  uint8_t st = TCMD_WAIT;
  const char* p = markup;
  while (*p && n < 640) {
    unsigned char c0 = (unsigned char)*p;
    uint32_t step = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
    bool cmd = selTxtIsCmd(st, (char)c0);
    word[n++] = !cmd && (step > 1 || !isspace(c0));
    p += step;
  }
  if (idx >= n || !word[idx]) { *lo = idx; *hi = idx; return; }   // off the end / on a space -> caret
  uint32_t a = idx, b = idx;
  while (a > 0 && word[a - 1]) a--;
  while (b + 1 < n && word[b + 1]) b++;
  *lo = a; *hi = b + 1;
}

void UITask::selectWordAt(lv_obj_t* lbl, lv_point_t abs_pt) {
  endSelection();
  _sel.kind = SEL_LABEL;
  _sel.target = lbl;
  _sel.whole_obj = false;
  uint32_t idx = labelCharAt(lbl, abs_pt);
  uint32_t lo, hi;
  labelWordAt(lv_label_get_text(lbl), idx, &lo, &hi);
  _sel.anchor = lo; _sel.sel_lo = lo; _sel.sel_hi = hi;
  if (_chat_history) lv_obj_clear_flag(_chat_history, LV_OBJ_FLAG_SCROLLABLE);
  applyLabelSel();
  ensureSelHandles();
  positionSelHandles();
  lv_obj_clear_flag(_sel.h_start, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_sel.h_end, LV_OBJ_FLAG_HIDDEN);
  finishSel();   // show the toolbar straight away
}

void UITask::beginCardSel(lv_obj_t* card) {
  endSelection();
  _sel.kind = SEL_CARD;
  _sel.target = card;
  _sel.whole_obj = true;
  lv_obj_set_style_border_color(card, lv_color_hex(UI_ACCENT), 0);  // selected look
  lv_obj_set_style_border_width(card, 2, 0);
  if (_chat_history) lv_obj_clear_flag(_chat_history, LV_OBJ_FLAG_SCROLLABLE);
  ensureSelHandles();
  positionSelHandles();
  lv_obj_clear_flag(_sel.h_start, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_sel.h_end, LV_OBJ_FLAG_HIDDEN);
  _sel.state = SS_MARKING;
}

void UITask::updateSelDrag(lv_point_t abs_pt) {
  if (_sel.kind != SEL_LABEL || !_sel.target) return;
  uint32_t idx = labelCharAt(_sel.target, abs_pt);
  uint32_t lo = idx < _sel.anchor ? idx : _sel.anchor;
  uint32_t hi = idx < _sel.anchor ? _sel.anchor : idx;
  if (lo == _sel.sel_lo && hi == _sel.sel_hi) return;   // cheap: only redraw on change
  _sel.sel_lo = lo; _sel.sel_hi = hi;
  applyLabelSel();
  positionSelHandles();
  _sel.state = SS_DRAGGING;
}

void UITask::applyLabelSel() {
  if (_sel.kind != SEL_LABEL || !_sel.target) return;
  if (_sel.sel_lo == _sel.sel_hi) {
    lv_label_set_text_sel_start(_sel.target, LV_LABEL_TEXT_SELECTION_OFF);
    lv_label_set_text_sel_end(_sel.target, LV_LABEL_TEXT_SELECTION_OFF);
  } else {
    lv_label_set_text_sel_start(_sel.target, _sel.sel_lo);
    lv_label_set_text_sel_end(_sel.target, _sel.sel_hi);
  }
}

void UITask::positionSelHandles() {
  if (!_sel.h_start || !_sel.target) return;
  if (_sel.kind == SEL_LABEL) {
    lv_area_t a; lv_obj_get_coords(_sel.target, &a);
    const lv_font_t* font = lv_obj_get_style_text_font(_sel.target, LV_PART_MAIN);
    lv_coord_t lh = lv_font_get_line_height(font);
    lv_point_t ps, pe;
    lv_label_get_letter_pos(_sel.target, _sel.sel_lo, &ps);
    lv_label_get_letter_pos(_sel.target, _sel.sel_hi, &pe);
    lv_obj_set_pos(_sel.h_start, a.x1 + ps.x - SEL_HANDLE_D / 2, a.y1 + ps.y - SEL_HANDLE_D);  // above start
    lv_obj_set_pos(_sel.h_end,   a.x1 + pe.x - SEL_HANDLE_D / 2, a.y1 + pe.y + lh);            // below end
  } else {  // whole card: dots at the corners
    lv_area_t a; lv_obj_get_coords(_sel.target, &a);
    lv_obj_set_pos(_sel.h_start, a.x1 - SEL_HANDLE_D / 2, a.y1 - SEL_HANDLE_D / 2);
    lv_obj_set_pos(_sel.h_end,   a.x2 - SEL_HANDLE_D / 2, a.y2 - SEL_HANDLE_D / 2);
  }
}

// Screen-space bounding box of the current selection (the highlighted text span,
// or the whole card). Used to anchor the toolbar and to test taps for dismissal.
void UITask::selAnchorRect(lv_area_t* out) {
  if (!_sel.target) { lv_area_set(out, 0, 0, 0, 0); return; }
  if (_sel.kind == SEL_LABEL) {
    lv_area_t a; lv_obj_get_coords(_sel.target, &a);
    const lv_font_t* font = lv_obj_get_style_text_font(_sel.target, LV_PART_MAIN);
    lv_coord_t lh = lv_font_get_line_height(font);
    lv_point_t ps, pe;
    lv_label_get_letter_pos(_sel.target, _sel.sel_lo, &ps);
    lv_label_get_letter_pos(_sel.target, _sel.sel_hi, &pe);
    // Span may wrap across lines; use the union of both endpoints' rows. x spans
    // the whole label width when multi-line so the bbox covers the selection.
    bool multiline = (pe.y > ps.y);
    out->x1 = multiline ? a.x1 : a.x1 + LV_MIN(ps.x, pe.x);
    out->x2 = multiline ? a.x2 : a.x1 + LV_MAX(ps.x, pe.x);
    out->y1 = a.y1 + ps.y;
    out->y2 = a.y1 + pe.y + lh;
  } else if (_sel.kind == SEL_TEXTAREA) {
    // Anchor on the actual paste target: the cursor (or the selection span if one
    // exists), via the textarea's internal label -- not the middle of the field.
    lv_obj_t* lbl = lv_textarea_get_label(_sel.target);
    lv_area_t la; lv_obj_get_coords(lbl ? lbl : _sel.target, &la);
    const lv_font_t* font = lv_obj_get_style_text_font(lbl ? lbl : _sel.target, LV_PART_MAIN);
    lv_coord_t lh = lv_font_get_line_height(font);
    lv_point_t ps, pe;
    uint32_t s, e;
    if (lbl && taSelRange(_sel.target, &s, &e)) {
      lv_label_get_letter_pos(lbl, s, &ps);
      lv_label_get_letter_pos(lbl, e, &pe);
    } else if (lbl) {
      lv_label_get_letter_pos(lbl, lv_textarea_get_cursor_pos(_sel.target), &ps);
      pe = ps;                                          // zero-width caret
    } else { ps.x = ps.y = pe.x = pe.y = 0; }
    bool multiline = (pe.y > ps.y);
    out->x1 = multiline ? la.x1 : la.x1 + LV_MIN(ps.x, pe.x);
    out->x2 = multiline ? la.x2 : la.x1 + LV_MAX(ps.x, pe.x);
    out->y1 = la.y1 + ps.y;
    out->y2 = la.y1 + pe.y + lh;
  } else {
    lv_obj_get_coords(_sel.target, out);   // whole-card selection
  }
}

// Screen point of the selection START (or the caret) + that line's height. The
// context toolbar anchors here -- by the actual paste/selection start, not the
// middle of a span or field.
void UITask::selStart(lv_point_t* tip, lv_coord_t* line_h) {
  tip->x = 0; tip->y = 0; *line_h = 0;
  if (!_sel.target) return;
  if (_sel.kind == SEL_LABEL) {
    lv_area_t a; lv_obj_get_coords(_sel.target, &a);
    const lv_font_t* font = lv_obj_get_style_text_font(_sel.target, LV_PART_MAIN);
    *line_h = lv_font_get_line_height(font);
    lv_point_t ps; lv_label_get_letter_pos(_sel.target, _sel.sel_lo, &ps);
    tip->x = a.x1 + ps.x; tip->y = a.y1 + ps.y;
  } else if (_sel.kind == SEL_TEXTAREA) {
    lv_obj_t* lbl = lv_textarea_get_label(_sel.target);
    lv_obj_t* ref = lbl ? lbl : _sel.target;
    lv_area_t la; lv_obj_get_coords(ref, &la);
    *line_h = lv_font_get_line_height(lv_obj_get_style_text_font(ref, LV_PART_MAIN));
    lv_point_t ps = {0, 0};
    if (lbl) {
      uint32_t s, e;
      if (taSelRange(_sel.target, &s, &e)) lv_label_get_letter_pos(lbl, s, &ps);
      else lv_label_get_letter_pos(lbl, lv_textarea_get_cursor_pos(_sel.target), &ps);
    }
    tip->x = la.x1 + ps.x; tip->y = la.y1 + ps.y;
  } else {   // whole card: top-left corner
    lv_area_t a; lv_obj_get_coords(_sel.target, &a);
    tip->x = a.x1; tip->y = a.y1; *line_h = a.y2 - a.y1;
  }
}

bool UITask::selPointInside(lv_point_t p) {
  if (!_sel.target) return false;
  lv_area_t r; selAnchorRect(&r);
  return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
}

void UITask::finishSel() {
  if (!_sel.target) { endSelection(); return; }
  _sel.state = SS_MENU;
  showSelMenu();
}

// Floating horizontal toolbar of icon buttons (Copy / Select All; Cut/Paste come
// with textareas in Phase 2). No screen dim: a transparent full-screen catcher
// behind the bar dismisses the selection only when a tap lands OUTSIDE it, so the
// selection stays live and adjustable (drag the handles) while the bar is up.
void UITask::showSelMenu() {
  if (!_sel.catcher) {
    _sel.catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(_sel.catcher);
    lv_obj_set_size(_sel.catcher, _screen_w, _screen_h);
    lv_obj_set_pos(_sel.catcher, 0, 0);
    lv_obj_add_flag(_sel.catcher, LV_OBJ_FLAG_CLICKABLE);   // transparent, no dim
    lv_obj_clear_flag(_sel.catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(_sel.catcher, sel_catcher_cb, LV_EVENT_CLICKED, NULL);

    _sel.menu = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(_sel.menu);
    lv_obj_set_size(_sel.menu, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(_sel.menu, lv_color_hex(UI_SURFACE), 0);
    lv_obj_set_style_bg_opa(_sel.menu, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(_sel.menu, lv_color_hex(UI_BORDER), 0);
    lv_obj_set_style_border_width(_sel.menu, 1, 0);
    lv_obj_set_style_radius(_sel.menu, 8, 0);
    lv_obj_set_style_pad_all(_sel.menu, 2, 0);
    lv_obj_set_style_pad_column(_sel.menu, 2, 0);
    lv_obj_set_flex_flow(_sel.menu, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(_sel.menu, LV_OBJ_FLAG_SCROLLABLE);
  }
  lv_obj_clean(_sel.menu);
  auto addBtn = [&](const char* sym, lv_event_cb_t cb) {
    lv_obj_t* b = lv_btn_create(_sel.menu);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_FG_BRIGHT), 0);
    lv_obj_center(l);
  };
  if (_sel.kind == SEL_TEXTAREA) {
    uint32_t s, en;
    bool has = taSelRange(_sel.target, &s, &en);
    if (has) { addBtn(LV_SYMBOL_CUT, selmenu_cut_cb); addBtn(LV_SYMBOL_COPY, selmenu_copy_cb); }
    if (_clip_kind != CLIP_EMPTY) addBtn(LV_SYMBOL_PASTE, selmenu_paste_cb);
    addBtn(LV_SYMBOL_LIST, selmenu_selectall_cb);
  } else {
    bool has_sel = (_sel.kind == SEL_CARD) || (_sel.sel_hi > _sel.sel_lo);
    if (has_sel) addBtn(LV_SYMBOL_COPY, selmenu_copy_cb);
    if (_sel.kind == SEL_LABEL) addBtn(LV_SYMBOL_LIST, selmenu_selectall_cb);
  }

  // z-order: catcher (bottom) < handles < toolbar (top)
  lv_obj_clear_flag(_sel.catcher, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_sel.catcher);
  if (_sel.h_start) lv_obj_move_foreground(_sel.h_start);
  if (_sel.h_end)   lv_obj_move_foreground(_sel.h_end);
  lv_obj_clear_flag(_sel.menu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_sel.menu);

  lv_point_t tip; lv_coord_t lh; selStart(&tip, &lh);   // the cursor / start of the selection
  lv_obj_update_layout(_sel.menu);
  lv_coord_t mw = lv_obj_get_width(_sel.menu);
  lv_coord_t mh = lv_obj_get_height(_sel.menu);
  const lv_coord_t GAP = 6;
  const lv_coord_t HANDLE_CLR = (_sel.kind == SEL_TEXTAREA) ? 2 : SEL_HANDLE_D + 2;  // clear the handle
  lv_coord_t y = tip.y - mh - GAP - HANDLE_CLR;           // prefer above the start line
  if (_sel.kind == SEL_TEXTAREA) {
    // A field usually has the keyboard up across the bottom: stay above, clamp
    // under the header rather than dropping into the keyboard.
    if (y < HEADER_H + GAP) y = HEADER_H + GAP;
  } else {
    if (y < HEADER_H + GAP) y = tip.y + lh + GAP + HANDLE_CLR;  // no room above -> below the start line
    if (y + mh > _screen_h - GAP) y = _screen_h - mh - GAP;     // clamp on-screen
    if (y < HEADER_H + GAP) y = HEADER_H + GAP;
  }
  lv_coord_t x = tip.x;                                   // left edge at the cursor / selection start
  if (x + mw > _screen_w - GAP) x = _screen_w - mw - GAP;
  if (x < GAP) x = GAP;
  lv_obj_set_pos(_sel.menu, x, y);
}

void UITask::copySelection() {
  if (_sel.kind == SEL_CARD && _sel.target) {
    const CardTarget* t = (const CardTarget*)lv_obj_get_user_data(_sel.target);
    if (t) {
      char hex[2 * PUB_KEY_SIZE + 1]; mesh::Utils::toHex(hex, t->pubkey, PUB_KEY_SIZE);
      char ref[2 * PUB_KEY_SIZE + 48];
      snprintf(ref, sizeof(ref), "<%s:%u:%s>", hex, (unsigned)t->type, t->name);
      clipSet(CLIP_CONTACT_REF, ref, t->pubkey, t->name, t->type);
      showToast("Contact copied");
    }
  } else if (_sel.kind == SEL_LABEL && _sel.target && _sel.sel_hi > _sel.sel_lo) {
    char out[1024];
    labelSelToLogical(lv_label_get_text(_sel.target), _sel.sel_lo, _sel.sel_hi, out, sizeof(out));
    clipSet(CLIP_PLAIN, out, nullptr, nullptr, 0);
    showToast("Copied");
  } else if (_sel.kind == SEL_TEXTAREA && _sel.target) {
    uint32_t s, en;
    if (taSelRange(_sel.target, &s, &en)) {
      const char* txt = lv_textarea_get_text(_sel.target);   // plain (no recolor) -> char-id == codepoint
      char out[1024];
      uint32_t cid = 0; size_t o = 0; const char* p = txt;
      while (*p && o + 4 < sizeof(out)) {
        unsigned char c0 = (unsigned char)*p;
        uint32_t step = (c0 >= 0xF0) ? 4 : (c0 >= 0xE0) ? 3 : (c0 >= 0xC0) ? 2 : 1;
        if (cid >= s && cid < en) for (uint32_t k = 0; k < step && p[k]; k++) out[o++] = p[k];
        cid++; p += step;
      }
      out[o] = 0;
      clipSet(CLIP_PLAIN, out, nullptr, nullptr, 0);
      showToast("Copied");
    }
  }
}

bool UITask::taSelRange(lv_obj_t* ta, uint32_t* s, uint32_t* e) {
  lv_obj_t* lbl = lv_textarea_get_label(ta);
  if (!lbl) return false;
  uint32_t a = lv_label_get_text_selection_start(lbl);
  uint32_t b = lv_label_get_text_selection_end(lbl);
  if (a == LV_LABEL_TEXT_SELECTION_OFF || b == LV_LABEL_TEXT_SELECTION_OFF || a == b) return false;
  if (a > b) { uint32_t t = a; a = b; b = t; }
  *s = a; *e = b;
  return true;
}

void UITask::beginTextareaSel(lv_obj_t* ta) {
  endSelection();
  _sel.kind = SEL_TEXTAREA;
  _sel.target = ta;
  _sel.state = SS_MENU;
  showSelMenu();
}

void UITask::endSelection() {
  if (_sel.kind == SEL_LABEL && _sel.target) {
    lv_label_set_text_sel_start(_sel.target, LV_LABEL_TEXT_SELECTION_OFF);
    lv_label_set_text_sel_end(_sel.target, LV_LABEL_TEXT_SELECTION_OFF);
  } else if (_sel.kind == SEL_CARD && _sel.target) {
    lv_obj_set_style_border_color(_sel.target, lv_color_hex(UI_BORDER), 0);  // restore card default
    lv_obj_set_style_border_width(_sel.target, 1, 0);
  }
  if (_sel.h_start) lv_obj_add_flag(_sel.h_start, LV_OBJ_FLAG_HIDDEN);
  if (_sel.h_end)   lv_obj_add_flag(_sel.h_end, LV_OBJ_FLAG_HIDDEN);
  if (_sel.menu)    lv_obj_add_flag(_sel.menu, LV_OBJ_FLAG_HIDDEN);
  if (_sel.catcher) lv_obj_add_flag(_sel.catcher, LV_OBJ_FLAG_HIDDEN);
  if (_chat_history) lv_obj_add_flag(_chat_history, LV_OBJ_FLAG_SCROLLABLE);  // re-enable scroll
  _sel.state = SS_IDLE;
  _sel.kind = SEL_NONE;
  _sel.target = nullptr;
  _sel.whole_obj = false;
  _sel.sel_lo = _sel.sel_hi = _sel.anchor = 0;
}

void UITask::sel_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_obj_t* target = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  bool is_label = lv_obj_check_type(target, &lv_label_class);
  switch (code) {
    case LV_EVENT_LONG_PRESSED: {
      lv_point_t p; lv_indev_get_point(lv_indev_get_act(), &p);
      if (is_label) _instance->beginLabelSel(target, p);
      else          _instance->beginCardSel(target);
      break;
    }
    case LV_EVENT_PRESSING:
      if ((_instance->_sel.state == SS_MARKING || _instance->_sel.state == SS_DRAGGING) &&
          _instance->_sel.kind == SEL_LABEL && target == _instance->_sel.target) {
        lv_point_t p; lv_indev_get_point(lv_indev_get_act(), &p);
        _instance->updateSelDrag(p);
      }
      break;
    case LV_EVENT_RELEASED:
      if (_instance->_sel.state == SS_MARKING || _instance->_sel.state == SS_DRAGGING)
        _instance->finishSel();
      break;
    case LV_EVENT_CLICKED: {
      if (!is_label || _instance->_sel.state != SS_IDLE) break;
      lv_point_t p; lv_indev_get_point(lv_indev_get_act(), &p);
      // First: did the tap land on a chip (@mention / #hashtag)? The label stores
      // its logical source text in user_data when it has tappable tokens. Resolve
      // the exact chip under the finger (per-chip, no picker).
      const char* src = (const char*)lv_obj_get_user_data(target);
      if (src) {
        uint32_t off = markupToLogical(lv_label_get_text(target), labelCharAt(target, p));
        bool is_hashtag; char nm[CHAT_PEER_NAME_MAX];
        if (tokenAtLogical(src, off, &is_hashtag, nm, sizeof(nm))) {
          _instance->resolveChip(is_hashtag ? CHIP_HASHTAG : CHIP_MENTION, nm);
          lv_event_stop_bubbling(e);   // don't also fire the bubble's resend handler
          break;
        }
      }
      // Otherwise: double-tap a word selects it; single tap on plain text does nothing.
      SelectionCtl& s = _instance->_sel;
      uint32_t now = millis();
      bool dbl = (s.last_tap_obj == target) && (now - s.last_tap_ms < 300) &&
                 (LV_ABS(p.x - s.last_tap_x) < 20) && (LV_ABS(p.y - s.last_tap_y) < 20);
      if (dbl) {
        s.last_tap_obj = nullptr;
        _instance->selectWordAt(target, p);
      } else {
        s.last_tap_obj = target; s.last_tap_ms = now; s.last_tap_x = p.x; s.last_tap_y = p.y;
      }
      break;
    }
    default: break;
  }
}

// Drag a handle to re-adjust that endpoint while the toolbar stays up. The handle
// being dragged moves its endpoint; the opposite endpoint is the fixed anchor.
void UITask::sel_handle_cb(lv_event_t* e) {
  if (!_instance) return;
  SelectionCtl& s = _instance->_sel;
  if (s.kind != SEL_LABEL || !s.target) return;
  lv_obj_t* h = lv_event_get_target(e);
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    s.anchor = (h == s.h_start) ? s.sel_hi : s.sel_lo;   // grab: opposite end is fixed
  } else if (code == LV_EVENT_PRESSING) {
    lv_point_t p; lv_indev_get_point(lv_indev_get_act(), &p);
    _instance->updateSelDrag(p);   // re-hit-test against the label; reorders lo/hi vs anchor
  } else if (code == LV_EVENT_RELEASED) {
    _instance->showSelMenu();      // re-anchor the toolbar over the new selection
  }
}

// Tap on the transparent catcher: dismiss the selection only if the tap landed
// OUTSIDE the selection (taps inside keep it live so the user can keep adjusting).
void UITask::sel_catcher_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  lv_point_t p; lv_indev_get_point(lv_indev_get_act(), &p);
  if (!_instance->selPointInside(p)) _instance->endSelection();
}

void UITask::ta_longpress_cb(lv_event_t* e) {
  if (!_instance) return;
  _instance->beginTextareaSel(lv_event_get_target(e));
}

void UITask::selmenu_cut_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  SelectionCtl& s = _instance->_sel;
  if (s.kind != SEL_TEXTAREA || !s.target) { _instance->endSelection(); return; }
  _instance->copySelection();              // fill clipboard from the selection
  uint32_t a, b;
  if (_instance->taSelRange(s.target, &a, &b)) {
    lv_textarea_set_cursor_pos(s.target, b);
    for (uint32_t i = 0; i < b - a; i++) lv_textarea_del_char(s.target);
  }
  _instance->endSelection();
}

void UITask::selmenu_paste_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  SelectionCtl& s = _instance->_sel;
  if (s.kind != SEL_TEXTAREA || !s.target) { _instance->endSelection(); return; }
  if (_instance->_clip_kind != CLIP_EMPTY) {
    uint32_t a, b;
    if (_instance->taSelRange(s.target, &a, &b)) {     // paste replaces the selection
      lv_textarea_set_cursor_pos(s.target, b);
      for (uint32_t i = 0; i < b - a; i++) lv_textarea_del_char(s.target);
    }
    lv_textarea_add_text(s.target, _instance->pasteTextFor(_instance->fieldKindOf(s.target)));
  }
  _instance->endSelection();
}

void UITask::selmenu_copy_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  _instance->copySelection();
  _instance->endSelection();
}

void UITask::selmenu_selectall_cb(lv_event_t* e) {
  (void)e;
  if (!_instance) return;
  SelectionCtl& s = _instance->_sel;
  if (s.kind == SEL_TEXTAREA && s.target) {
    lv_obj_t* lbl = lv_textarea_get_label(s.target);
    uint32_t n = _lv_txt_get_encoded_length(lv_textarea_get_text(s.target));
    if (lbl && n > 0) { lv_label_set_text_sel_start(lbl, 0); lv_label_set_text_sel_end(lbl, n); }
    _instance->showSelMenu();   // now offers Cut/Copy
    return;
  }
  if (s.kind != SEL_LABEL || !s.target) return;
  uint32_t n = _lv_txt_get_encoded_length(lv_label_get_text(s.target));
  s.anchor = 0; s.sel_lo = 0; s.sel_hi = n;
  _instance->applyLabelSel();
  _instance->positionSelHandles();
  _instance->showSelMenu();   // re-anchor toolbar over the now-full selection
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

void UITask::set_avatar_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  uint8_t mode = lv_dropdown_get_selected(lv_event_get_target(e)) ? 1 : 0;
  _instance->_node_prefs->avatar_palette = mode;
  g_avatar_palette_mode = mode;            // nameColor() reads this immediately
  pushPrefs();
  _instance->_contacts_dirty = true;       // recolor the contact rows on next build
  _instance->updateOwnerProfile();         // live-refresh the avatar visible on Settings
}

void UITask::set_notify_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  _instance->_node_prefs->notify_enable = on ? 1 : 0;
  pushPrefs();
#ifdef PIN_BUZZER
  // Master off silences the chime too (banner/wake are gated in drainEvents); the
  // separate buzzer_quiet still applies when notifications are on.
  _instance->_buzzer.quiet(!on || _instance->_node_prefs->buzzer_quiet);
#endif
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

void UITask::set_telem_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  lv_obj_t* dd = lv_event_get_target(e);
  int which = (int)(intptr_t)lv_obj_get_user_data(dd);
  uint8_t v = (uint8_t)lv_dropdown_get_selected(dd);   // 0/1/2 = Deny/Allow-flagged/Allow-all
  if      (which == 0) _instance->_node_prefs->telemetry_mode_base = v;
  else if (which == 1) _instance->_node_prefs->telemetry_mode_loc  = v;
  else                 _instance->_node_prefs->telemetry_mode_env  = v;
  pushPrefs();
}

void UITask::set_autoadd_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  _instance->_node_prefs->manual_add_contacts = on ? 0 : 1;  // auto-add on => manual add off
  pushPrefs();
}

void UITask::set_autoadd_hops_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->autoadd_max_hops = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
  pushPrefs();
}

void UITask::set_rxboost_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->rx_boosted_gain = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
  pushPrefs();   // applied at radio init; nudge the user to reboot
  _instance->showToast("Saved (reboot to apply RX gain)");
}

// Shared focus/commit handler for the Advanced numeric fields (multi-ack, RX delay, airtime).
void UITask::set_advnum_ta_event_cb(lv_event_t* e) {
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
    _instance->raiseFieldForKb(_instance->_set_active_pane ? _instance->_set_active_pane
                                                           : _instance->_tab_settings,
                               _instance->_set_kb, ta);
  } else if (code == LV_EVENT_DEFOCUSED) {
    _instance->commitAdvNumbers();
  }
}

void UITask::commitAdvNumbers() {
  if (!_node_prefs) return;
  if (_set_multiack_ta) {
    int v = atoi(lv_textarea_get_text(_set_multiack_ta));
    if (v < 0) v = 0; if (v > 5) v = 5;
    _node_prefs->multi_acks = (uint8_t)v;
  }
  if (_set_rxdelay_ta) {
    float v = atof(lv_textarea_get_text(_set_rxdelay_ta));
    if (v < 0) v = 0; if (v > 20.0f) v = 20.0f;     // matches MyMesh::begin() clamp
    _node_prefs->rx_delay_base = v;
  }
  if (_set_airtime_ta) {
    float v = atof(lv_textarea_get_text(_set_airtime_ta));
    if (v < 0) v = 0; if (v > 9.0f) v = 9.0f;        // matches MyMesh::begin() clamp
    _node_prefs->airtime_factor = v;
  }
  if (_set_gps_interval_ta) {
    long v = atol(lv_textarea_get_text(_set_gps_interval_ta));
    if (v < 0) v = 0; if (v > 86400) v = 86400;      // matches MyMesh::begin() clamp (<=24h)
    _node_prefs->gps_interval = (uint32_t)v;
  }
  pushPrefs();
}

void UITask::set_gps_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  _instance->_node_prefs->gps_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
  pushPrefs();   // GPS is started from prefs at boot (applyGpsPrefs); apply on reboot
  _instance->showToast("Saved (reboot to apply GPS)");
}

void UITask::set_radio_sw_cb(lv_event_t* e) {
  if (!_instance || !_instance->_node_prefs) return;
  bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  _instance->_node_prefs->radio_off = on ? 0 : 1;
  mproxy::MeshCmd c{};
  c.kind = mproxy::CmdKind::SetRadio;       // backend sleeps/wakes the radio + persists
  c.prefs = *_instance->_node_prefs;
  mproxy::postCommand(c);
  _instance->showToast(on ? "Radio ON" : "Radio OFF - safe to detach antenna");
}

void UITask::set_pin_btn_cb(lv_event_t* e) {
  (void)e;
  if (_instance) _instance->openPinSet();
}

void UITask::buildPinSetPopup() {
  if (_pinset_popup) return;
  lv_obj_t* card = makeModalCard(&_pinset_popup, pinset_dismiss_cb);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, HEADER_H + 8);   // top -> room for the keyboard below
  lv_obj_add_flag(_pinset_popup, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* title = lv_label_create(card);
  lv_label_set_text(title, "Set lock PIN (4-6 digits)");
  lv_obj_set_style_text_color(title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(title, fontHeading(), 0);

  lv_obj_t* f1 = makeField(card, "PIN");
  _pinset_ta1 = makeSelTextarea(f1);
  lv_textarea_set_one_line(_pinset_ta1, true); lv_obj_add_event_cb(_pinset_ta1, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_max_length(_pinset_ta1, 6);
  lv_textarea_set_accepted_chars(_pinset_ta1, "0123456789");
  lv_obj_set_width(_pinset_ta1, LV_PCT(100));
  lv_obj_add_event_cb(_pinset_ta1, pinset_ta_event_cb, LV_EVENT_ALL, NULL);
  attachInlineEye(_pinset_ta1);

  lv_obj_t* f2 = makeField(card, "Confirm PIN");
  _pinset_ta2 = makeSelTextarea(f2);
  lv_textarea_set_one_line(_pinset_ta2, true); lv_obj_add_event_cb(_pinset_ta2, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_max_length(_pinset_ta2, 6);
  lv_textarea_set_accepted_chars(_pinset_ta2, "0123456789");
  lv_obj_set_width(_pinset_ta2, LV_PCT(100));
  lv_obj_add_event_cb(_pinset_ta2, pinset_ta_event_cb, LV_EVENT_ALL, NULL);
  attachInlineEye(_pinset_ta2);

  _pinset_err = lv_label_create(card);
  lv_label_set_text(_pinset_err, "");
  lv_obj_set_style_text_color(_pinset_err, lv_color_hex(UI_ERROR), 0);
  lv_obj_add_flag(_pinset_err, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* save = lv_btn_create(card);
  lv_obj_set_width(save, LV_PCT(100));
  lv_obj_add_event_cb(save, pinset_save_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* sl = lv_label_create(save);
  lv_label_set_text(sl, LV_SYMBOL_OK " Save");
  lv_obj_center(sl);

  _pinset_kb = lv_keyboard_create(_pinset_popup);
  lv_keyboard_set_mode(_pinset_kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_add_event_cb(_pinset_kb, pinset_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_pinset_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::openPinSet() {
  buildPinSetPopup();
  lv_textarea_set_text(_pinset_ta1, "");
  lv_textarea_set_text(_pinset_ta2, "");
  lv_obj_add_flag(_pinset_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_pinset_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_pinset_popup, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_pinset_popup);
}

void UITask::pinset_dismiss_cb(lv_event_t* e) {
  if (!_instance) return;
  if (lv_event_get_target(e) != _instance->_pinset_popup) return;
  lv_obj_add_flag(_instance->_pinset_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_instance->_pinset_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::pinset_save_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_node_prefs) return;
  const char* a = lv_textarea_get_text(_instance->_pinset_ta1);
  const char* b = lv_textarea_get_text(_instance->_pinset_ta2);
  size_t la = a ? strlen(a) : 0;
  auto err = [&](const char* m){ lv_label_set_text(_instance->_pinset_err, m); lv_obj_clear_flag(_instance->_pinset_err, LV_OBJ_FLAG_HIDDEN); };
  if (la == 0 && (!b || !b[0])) {                       // both blank -> clear the PIN
    _instance->_node_prefs->lock_pin[0] = 0;
    pushPrefs();
    _instance->showToast("PIN cleared");
  } else {
    if (la < 4 || la > 6) { err("PIN must be 4-6 digits"); return; }
    if (strcmp(a, b ? b : "") != 0) { err("PINs don't match"); return; }
    strncpy(_instance->_node_prefs->lock_pin, a, sizeof(_instance->_node_prefs->lock_pin) - 1);
    _instance->_node_prefs->lock_pin[sizeof(_instance->_node_prefs->lock_pin) - 1] = 0;
    pushPrefs();
    _instance->showToast("PIN saved");
  }
  lv_obj_add_flag(_instance->_pinset_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_instance->_pinset_popup, LV_OBJ_FLAG_HIDDEN);
}

void UITask::pinset_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED || lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_pinset_kb, lv_event_get_target(e));
    lv_keyboard_set_mode(_instance->_pinset_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_instance->_pinset_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_pinset_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_pinset_kb);
  }
}

void UITask::pinset_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL)
    lv_obj_add_flag(_instance->_pinset_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::lock_now_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_node_prefs) return;
  const char* pin = _instance->_node_prefs->lock_pin;
  if (!pin[0] || strlen(pin) < 4) { _instance->showToast("Set a PIN first"); return; }
  _instance->showLock();
}

void UITask::buildLockScreen() {
  if (_lock_screen) return;
  _lock_screen = lv_obj_create(lv_layer_top());   // opaque full-screen overlay; blocks the UI
  lv_obj_set_size(_lock_screen, _screen_w, _screen_h);
  lv_obj_set_pos(_lock_screen, 0, 0);
  lv_obj_set_style_bg_color(_lock_screen, lv_color_hex(BG_HEX), 0);
  lv_obj_set_style_bg_opa(_lock_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_lock_screen, 0, 0);
  lv_obj_set_style_pad_all(_lock_screen, 0, 0);
  lv_obj_add_flag(_lock_screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(_lock_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(_lock_screen, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* card = lv_obj_create(_lock_screen);
  lv_obj_set_width(card, LV_PCT(80));
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, HEADER_H + 8);
  lv_obj_set_style_bg_color(card, lv_color_hex(UI_SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 8, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_style_pad_row(card, 8, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* title = lv_label_create(card);
  lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Locked - enter PIN");
  lv_obj_set_style_text_color(title, lv_color_hex(FG_HEX), 0);
  lv_obj_set_style_text_font(title, fontHeading(), 0);

  lv_obj_t* row = lv_obj_create(card);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  _lock_pin_ta = makeSelTextarea(row);
  lv_textarea_set_one_line(_lock_pin_ta, true); lv_obj_add_event_cb(_lock_pin_ta, UITask::ta_done_cb, LV_EVENT_READY, NULL);
  lv_textarea_set_max_length(_lock_pin_ta, 6);
  lv_textarea_set_accepted_chars(_lock_pin_ta, "0123456789");
  lv_obj_set_flex_grow(_lock_pin_ta, 1);
  lv_obj_add_event_cb(_lock_pin_ta, lock_ta_event_cb, LV_EVENT_ALL, NULL);
  attachInlineEye(_lock_pin_ta);   // hidden by default + eye toggle
  lv_obj_t* ok = lv_btn_create(row);
  lv_obj_add_event_cb(ok, lock_unlock_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* okl = lv_label_create(ok);
  lv_label_set_text(okl, LV_SYMBOL_OK);
  lv_obj_center(okl);

  _lock_err = lv_label_create(card);
  lv_label_set_text(_lock_err, "");
  lv_obj_set_style_text_color(_lock_err, lv_color_hex(UI_ERROR), 0);
  lv_obj_add_flag(_lock_err, LV_OBJ_FLAG_HIDDEN);

  _lock_kb = lv_keyboard_create(_lock_screen);
  lv_keyboard_set_mode(_lock_kb, LV_KEYBOARD_MODE_NUMBER);
  lv_obj_add_event_cb(_lock_kb, lock_kb_event_cb, LV_EVENT_ALL, NULL);
  lv_obj_add_flag(_lock_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::showLock() {
  buildLockScreen();
  _locked = true;
  lv_textarea_set_text(_lock_pin_ta, "");
  lv_obj_add_flag(_lock_err, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_lock_kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_lock_screen, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_lock_screen);
}

void UITask::lock_unlock_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_node_prefs) return;
  const char* entered = lv_textarea_get_text(_instance->_lock_pin_ta);
  if (entered && strcmp(entered, _instance->_node_prefs->lock_pin) == 0) {
    _instance->_locked = false;
    lv_obj_add_flag(_instance->_lock_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_instance->_lock_screen, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(_instance->_lock_err, "Wrong PIN");
    lv_obj_clear_flag(_instance->_lock_err, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(_instance->_lock_pin_ta, "");
  }
}

void UITask::lock_ta_event_cb(lv_event_t* e) {
  if (!_instance) return;
  if (lv_event_get_code(e) == LV_EVENT_FOCUSED || lv_event_get_code(e) == LV_EVENT_CLICKED) {
    lv_keyboard_set_textarea(_instance->_lock_kb, _instance->_lock_pin_ta);
    lv_keyboard_set_mode(_instance->_lock_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_instance->_lock_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_lock_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_lock_kb);
  }
}

void UITask::lock_kb_event_cb(lv_event_t* e) {
  if (!_instance) return;
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) lock_unlock_cb(e);        // "enter" tries to unlock
  else if (code == LV_EVENT_CANCEL) lv_obj_add_flag(_instance->_lock_kb, LV_OBJ_FLAG_HIDDEN);
}

void UITask::set_shareme_cb(lv_event_t* e) {
  (void)e;
  if (!_instance || !_instance->_node_prefs) return;
  _instance->openShareQR(mproxy::selfPubKey(), ADV_TYPE_CHAT,
                         _instance->_node_prefs->node_name, _instance->_profile_screen);
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
    _instance->raiseFieldForKb(_instance->_set_active_pane ? _instance->_set_active_pane
                                                           : _instance->_tab_settings,
                               _instance->_set_kb, ta);
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
    lv_keyboard_set_textarea(_instance->_profile_kb, ta);
    lv_keyboard_set_mode(_instance->_profile_kb, LV_KEYBOARD_MODE_NUMBER);
    lv_obj_clear_flag(_instance->_profile_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(_instance->_profile_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_move_foreground(_instance->_profile_kb);
    _instance->raiseFieldForKb(_instance->_profile_body, _instance->_profile_kb, ta);
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
    lv_obj_t* card = makeModalCard(&_confirm_popup, NULL);  // explicit buttons; no tap-to-dismiss

    lv_obj_t* warn = lv_label_create(card);
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn, LV_PCT(100));
    lv_obj_set_style_text_color(warn, lv_color_hex(UI_FG_BRIGHT), 0);
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
    lv_obj_set_style_bg_color(ok, lv_color_hex(UI_PRIMARY), 0);
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
    lv_obj_t* card = makeModalCard(&_info_popup, [](lv_event_t* ev) {  // tap backdrop closes
      (void)ev;
      if (_instance) lv_obj_add_flag(_instance->_info_popup, LV_OBJ_FLAG_HIDDEN);
    });

    _info_title_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(_info_title_lbl, lv_color_hex(UI_ACCENT), 0);
    lv_obj_set_style_text_font(_info_title_lbl, fontHeading(), 0);

    _info_body_lbl = lv_label_create(card);
    lv_label_set_long_mode(_info_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_info_body_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_info_body_lbl, lv_color_hex(UI_FG_BRIGHT), 0);

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

#ifdef PIN_BUZZER
  _buzzer.loop();   // non-blocking RTTTL state-stepping; run every pass, even display-off
#endif

  // Pin the published snapshot for this UI pass, then apply backend events
  // (new/sent msgs, delivery, telemetry) to the store/LVGL on this (UI) core.
  mproxy::beginUiRead();
  drainEvents();

  if (_crash_note[0]) { showToast(_crash_note); _crash_note[0] = 0; }   // one-shot, from last boot's panic

  // Backlight idle-off (battery). Touch resets _last_input_ms in touchpad_read_cb,
  // which also wakes it. The radio/mesh keep running on core 0 the whole time.
  uint16_t timeout_s = _node_prefs ? _node_prefs->screen_timeout_s : 0;
  // `now` is sampled at the top of loop(); a notification wake (drainEvents) may have
  // set _last_input_ms slightly LATER than `now`, so clamp to avoid the unsigned
  // underflow that would instantly re-sleep the screen we just woke.
  uint32_t idle_ms = (now >= _last_input_ms) ? (now - _last_input_ms) : 0;
  if (timeout_s && !_display_off && idle_ms > (uint32_t)timeout_s * 1000) {
    board_set_backlight(0);
    _display_off = true;
  }
  // While the screen is off, skip the clock repaint and list rebuilds: nothing is
  // visible, and a per-second clock update would needlessly invalidate + flush.
  // lv_timer_handler still runs below so touch can wake us.
  if (!_display_off) {

  // Live header clock (1 Hz). rtcSeconds() is the internal RTC -- no bus traffic.
  if (_clock_label) {
    uint32_t secs = mproxy::rtcSeconds();
    if (secs != _clock_last) {
      _clock_last = secs;
      time_t t = (time_t)secs + (_node_prefs ? _node_prefs->tz_offset_minutes * 60 : 0);
      struct tm tmv;
      gmtime_r(&t, &tmv);  // gmtime of (UTC + offset) = local wall time
      char buf[16];
      if (_node_prefs && _node_prefs->clock_12h) {
        strftime(buf, sizeof(buf), "%I:%M %p", &tmv);  // e.g. "09:05 AM"
        if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));  // -> "9:05 AM"
      } else {
        strftime(buf, sizeof(buf), "%H:%M", &tmv);     // e.g. "21:05"
      }
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
    // Keep the chat header live (route learned/changed, rename) while it's on screen.
    if (_chat_screen && lv_scr_act() == _chat_screen) updateChatHeader();
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

  }  // end if(!_display_off)

  lv_timer_handler();   // always: renders dirty areas (none while off) + polls touch (wakes us)

  // Fire any pending notification chime AFTER the wake + banner draw above, so the
  // slow first flush precedes the first note instead of stretching it. Subsequent
  // frames are cheap (banner is static), so _buzzer.loop() ends each note on time.
  if (_pending_chime != UIEventType::none) {
    notify(_pending_chime);
    _pending_chime = UIEventType::none;
  }

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
        // "Viewing" = the chat screen is the active screen AND it's this conversation
        // (not merely that we last opened it -- you may have navigated back home).
        bool is_channel = strncmp(ev.conv_key, "ch_", 3) == 0;
        bool viewing = _chat_screen && lv_scr_act() == _chat_screen &&
                       strncmp(ev.conv_key, _chat_key, CHAT_PEER_NAME_MAX) == 0;
        if (viewing) rebuildChatHistory();
        if (!ev.outgoing) {
          bool muted = isMuted(ev.conv_key);
          if (!viewing) markUnread(ev.conv_key);   // unread mark only for chats you're not in
          if (notifyEnabled() && !muted) {
            // Phone-style: chime on every non-muted message (viewed or not). The
            // banner + screen-wake only fire when you're NOT already looking at it.
            if (!viewing) onIncomingNotify(ev.conv_key, ev.sender, ev.text, is_channel);
            _pending_chime = is_channel ? UIEventType::channelMessage : UIEventType::contactMessage;
          }
        }
        // Repaint the list that owns this conversation so its unread mark updates.
        if (is_channel) _channels_pending = true;
        else            _contacts_dirty = true;   // also covers the "latest message" re-sort
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
      case mproxy::EvKind::LoginResult: {
        if (ev.ok) {
          char m[48];
          snprintf(m, sizeof(m), "Logged in%s", ev.is_admin ? " (admin)" : "");
          showToast(m);
        } else {
          showToast("Login failed");
        }
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
