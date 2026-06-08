#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
// The ESP32-S3 RGB-parallel bus/panel classes aren't pulled in by the umbrella header.
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>

// CrowPanel Advance 4.3" / 5.0" / 7.0" LVGL display (shared RGB-tier config).
//  Display: 16-bit RGB-parallel, 800x480, framebuffer in PSRAM (ESP32-S3 LCD_CAM).
//  Touch:   GT911 capacitive on I2C0 (SDA15/SCL16), polled (no INT defined for this tier).
//  Backlight: gated by an I2C IO-expander (PCA9557) / STC8H MCU on these units -- handled
//             (best-effort) in CrowPanelBoard (CROWPANEL_RGB branch), NOT here.
//
// BLIND BUILD -- VERIFY ON DEVICE (these are first-attempt, untested):
//  * RGB data-pin ORDER (red/blue may be swapped) -- from the Elecrow repo's documented bus.
//  * Panel TIMINGS (porch/pulse/pclk) are generic 800x480 values; port the exact values from the
//    size's Elecrow Arduino_GFX config (4.3=ST7265, 5.0=ST7262, 7.0=SC7277).
//  * GT911 address (0x5D vs 0x14) + whether an INT/RST pin exists.
//  * Backlight bring-up (expander) -- screen may stay dark until that's added.

#ifndef CROWPANEL_TFT_ROTATION
  #define CROWPANEL_TFT_ROTATION 0   // 800x480 panels are natively landscape
#endif

class CrowPanelLGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB     _bus;
  lgfx::Panel_RGB   _panel;
  lgfx::Touch_GT911 _touch;

public:
  static constexpr uint16_t native_w = 800;
  static constexpr uint16_t native_h = 480;

  CrowPanelLGFX() {
    {
      auto cfg = _panel.config();
      cfg.memory_width  = native_w;
      cfg.memory_height = native_h;
      cfg.panel_width   = native_w;
      cfg.panel_height  = native_h;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      _panel.config(cfg);
    }
    {
      auto cfg = _panel.config_detail();
      cfg.use_psram = 1;            // framebuffer in PSRAM (~768 KB for 800x480x2)
      _panel.config_detail(cfg);
    }
    {
      auto cfg = _bus.config();
      cfg.panel = &_panel;
      // 16-bit RGB data bus (documented Elecrow order). VERIFY red/blue grouping on device.
      cfg.pin_d0  = 21; cfg.pin_d1  = 47; cfg.pin_d2  = 48; cfg.pin_d3  = 45;
      cfg.pin_d4  = 38; cfg.pin_d5  = 9;  cfg.pin_d6  = 10; cfg.pin_d7  = 11;
      cfg.pin_d8  = 12; cfg.pin_d9  = 13; cfg.pin_d10 = 14; cfg.pin_d11 = 7;
      cfg.pin_d12 = 17; cfg.pin_d13 = 18; cfg.pin_d14 = 3;  cfg.pin_d15 = 46;
      cfg.pin_henable = 42;   // DE
      cfg.pin_vsync   = 41;
      cfg.pin_hsync   = 40;
      cfg.pin_pclk    = 39;
      cfg.freq_write  = 12000000;
      // Timings from LovyanGFX's bundled Elecrow 800x480 RGB config (WZ8048C050); the panels are
      // likely the same glass. VERIFY/port the size-exact values from the Advance Elecrow repo.
      cfg.hsync_polarity    = 0;
      cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4;
      cfg.hsync_back_porch  = 43;
      cfg.vsync_polarity    = 0;
      cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4;
      cfg.vsync_back_porch  = 12;
      cfg.pclk_active_neg   = 1;
      cfg.de_idle_high      = 0;
      cfg.pclk_idle_high    = 0;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min       = 0;
      cfg.x_max       = native_w - 1;
      cfg.y_min       = 0;
      cfg.y_max       = native_h - 1;
      cfg.pin_int     = -1;     // polled (no touch INT on this tier per Meshtastic)
      cfg.pin_rst     = -1;
      cfg.bus_shared  = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port    = 0;
      cfg.i2c_addr    = 0x14;   // VERIFY (Elecrow RGB configs use 0x14; may be 0x5D)
      cfg.pin_sda     = 15;
      cfg.pin_scl     = 16;
      cfg.freq        = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

// Natively-landscape 800x480: pass (native_w, native_h) so the UI canvas is 800 wide x 480 tall.
class CrowPanelDisplay : public LGFXDisplay {
  CrowPanelLGFX _disp;
public:
  CrowPanelDisplay() : LGFXDisplay(CrowPanelLGFX::native_w, CrowPanelLGFX::native_h, _disp) {}
  bool begin() {
    bool ok = LGFXDisplay::begin();
    if (ok) _disp.setRotation(CROWPANEL_TFT_ROTATION);
    return ok;
  }
};
