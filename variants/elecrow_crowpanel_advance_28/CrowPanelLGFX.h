#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// CrowPanel Advance 2.4" / 2.8" LVGL display (shared SPI-tier config).
//  Display: ST7789, 320x240 IPS, on a DEDICATED SPI2 bus (like the 3.5's ILI9488).
//  Touch:   capacitive on I2C0 (SDA15/SCL16). The board platform resets it in
//           CrowPanelBoard::begin() (GT911 INT/RST timing, addr 0x14).
//  Backlight on GPIO38, PWM (LEDC) via CrowPanelBoard.
//
// BLIND BUILD -- VERIFY ON DEVICE: Elecrow's spec lists the 2.4/2.8 touch as
// FT6336U, but Meshtastic treats 2.4/2.8/3.5 identically (same touch INT 47) and
// our 3.5 is GT911, so we configure GT911 here for platform consistency. If touch
// is dead, switch `lgfx::Touch_GT911` -> `lgfx::Touch_FT5x06` (FT6336U) and drop the
// gt911_reset() in CrowPanelBoard. Also verify ST7789 invert/rgb_order/offsets and
// the panel pin map (cloned from the 3.5: SCLK42/MOSI39/DC41/CS40, dedicated SPI2).

#ifndef CROWPANEL_TFT_ROTATION
  #define CROWPANEL_TFT_ROTATION 1   // landscape 320x240; flip to 3 if upside-down
#endif

class CrowPanelLGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Touch_GT911  _touch;

public:
  static constexpr uint16_t native_w = 240;   // ST7789 native (portrait); landscape via rotation
  static constexpr uint16_t native_h = 320;

  CrowPanelLGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;   // dedicated display bus (not shared with LoRa/SD)
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 42;
      cfg.pin_mosi    = 39;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = 41;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 40;
      cfg.pin_rst          = -1;   // VERIFY: 3.5 ties panel RST to SX1262 RST; assume the same here
      cfg.pin_busy         = -1;
      cfg.panel_width      = native_w;
      cfg.panel_height     = native_h;
      cfg.offset_x         = 0;    // VERIFY (ST7789 240x320 window offsets)
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;    // rotation set in CrowPanelDisplay::begin()
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;   // ST7789 IPS typically inverted; VERIFY
      cfg.rgb_order        = false;  // VERIFY (red/blue swap -> flip this)
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;  // dedicated SPI2
      _panel.config(cfg);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min       = 0;
      cfg.x_max       = native_w - 1;
      cfg.y_min       = 0;
      cfg.y_max       = native_h - 1;
      cfg.pin_int     = 47;     // matches Meshtastic SCREEN_TOUCH_INT for the 2.4/2.8/3.5 tier
      cfg.pin_rst     = -1;     // owned by CrowPanelBoard::begin() (deterministic GT911 reset)
      cfg.bus_shared  = false;
      cfg.offset_rotation = 0;
      cfg.i2c_port    = 0;
      cfg.i2c_addr    = 0x14;
      cfg.pin_sda     = 15;
      cfg.pin_scl     = 16;
      cfg.freq        = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

class CrowPanelDisplay : public LGFXDisplay {
  CrowPanelLGFX _disp;
public:
  CrowPanelDisplay() : LGFXDisplay(CrowPanelLGFX::native_h, CrowPanelLGFX::native_w, _disp) {}
  bool begin() {
    bool ok = LGFXDisplay::begin();
    if (ok) _disp.setRotation(CROWPANEL_TFT_ROTATION);
    return ok;
  }
};
