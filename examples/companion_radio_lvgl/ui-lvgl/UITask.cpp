#include "UITask.h"
#include <esp_heap_caps.h>

UITask* UITask::_instance = NULL;

void UITask::disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  if (!_instance || !_instance->_lgfx) {
    lv_disp_flush_ready(drv);
    return;
  }
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  CrowPanelLGFX& lcd = *_instance->_lgfx;
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

void UITask::buildHomeScreen() {
  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), 0);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "MeshCore");
  lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  lv_obj_t* sub = lv_label_create(scr);
  lv_label_set_text(sub, "CrowPanel 3.5 — LVGL scaffold");
  lv_obj_set_style_text_color(sub, lv_color_hex(0x808080), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 56);

  _status_label = lv_label_create(scr);
  lv_label_set_text(_status_label, "waiting for first packet...");
  lv_obj_set_style_text_color(_status_label, lv_color_hex(0xE0E0E0), 0);
  lv_obj_align(_status_label, LV_ALIGN_CENTER, 0, 0);
}

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  (void)display;  // DisplayDriver bypassed; LVGL draws directly to LGFX
  _sensors = sensors;
  _node_prefs = node_prefs;
  _instance = this;

  _lgfx = &((CrowPanelDisplay*)display)->getLgfx();
  if (!_lgfx->init()) return;
  _lgfx->setBrightness(255);
  _lgfx->fillScreen(0x0000);

  lv_init();

  const size_t buf_pixels = kScreenW * kBufferLines;
  _buf1 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  _buf2 = (lv_color_t*)heap_caps_malloc(buf_pixels * sizeof(lv_color_t),
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&_draw_buf, _buf1, _buf2, buf_pixels);

  lv_disp_drv_init(&_disp_drv);
  _disp_drv.hor_res  = kScreenW;
  _disp_drv.ver_res  = kScreenH;
  _disp_drv.flush_cb = disp_flush_cb;
  _disp_drv.draw_buf = &_draw_buf;
  lv_disp_drv_register(&_disp_drv);

  lv_indev_drv_init(&_indev_drv);
  _indev_drv.type    = LV_INDEV_TYPE_POINTER;
  _indev_drv.read_cb = touchpad_read_cb;
  lv_indev_drv_register(&_indev_drv);

  buildHomeScreen();

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
