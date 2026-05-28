#include "UITask.h"
#include "meshcore_assets.h"
#include <esp_heap_caps.h>

UITask* UITask::_instance = NULL;

void UITask::disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (!_instance || !_instance->_lgfx) {
    lv_disp_flush_ready(drv);
    return;
  }
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  LGFX_Device& lcd = *_instance->_lgfx;
  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.writePixels((lgfx::rgb565_t*)color_p, w * h);
  lcd.endWrite();
  lv_disp_flush_ready(drv);
}

void UITask::touchpad_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  if (!_instance || !_instance->_lgfx) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  int32_t x, y;
  uint16_t tx = 0, ty = 0;
  if (_instance->_lgfx->getTouch(&tx, &ty)) {
    x = tx; y = ty;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
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

lv_obj_t* UITask::buildHomeScreen() {
  lv_obj_t* scr = lv_obj_create(NULL);
  styleAsDarkScreen(scr);

  // Small wordmark as a header until we have a real status bar
  lv_obj_t* header = lv_img_create(scr);
  lv_img_set_src(header, &meshcore_logo_img);
  lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 12);

  _status_label = lv_label_create(scr);
  lv_label_set_text(_status_label, "waiting for first packet...");
  lv_label_set_long_mode(_status_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_status_label, _screen_w - 24);
  lv_obj_set_style_text_color(_status_label, lv_color_hex(0xD1D5DB), 0);
  lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 0);

  return scr;
}

void UITask::splash_dismiss_cb(lv_timer_t* t) {
  UITask* self = static_cast<UITask*>(t->user_data);
  if (self->_home_screen) {
    lv_scr_load_anim(self->_home_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
  }
  lv_timer_del(t);
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

  const size_t buf_pixels = (size_t)_screen_w * kBufferLines;
  _buf1 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  _buf2 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;
  if (_status_label) {
    char buf[128];
    snprintf(buf, sizeof(buf), "[%u] %s: %s", (unsigned)msgcount,
             from_name ? from_name : "?", text ? text : "");
    lv_label_set_text(_status_label, buf);
  }
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
  lv_timer_handler();
}
