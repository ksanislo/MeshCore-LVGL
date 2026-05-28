#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Display: ILI9488, 480x320, SPI2 host @ 40 MHz.
// Touch:   GT911 capacitive, I2C0 @ 400 kHz, addr 0x14.
// Backlight on GPIO38, driven by CrowPanelBoard at boot.
// GPIO2 is shared with SX1262 RST; we leave panel reset to LoRa's pulse and
// init LGFX lazily from UITask::begin() after radio_init() has fired.

#ifndef CROWPANEL_TFT_ROTATION
  #define CROWPANEL_TFT_ROTATION 3  // 90 deg CCW (landscape, USB on the right)
#endif

class CrowPanelLGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Touch_GT911   _touch;

public:
  static constexpr uint16_t native_w = 320;
  static constexpr uint16_t native_h = 480;

  CrowPanelLGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;
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
      cfg.pin_rst          = -1;   // tied to SX1262 RST; LoRa driver owns it
      cfg.pin_busy         = -1;
      cfg.panel_width      = native_w;
      cfg.panel_height     = native_h;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = CROWPANEL_TFT_ROTATION;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min       = 0;
      cfg.x_max       = native_w - 1;
      cfg.y_min       = 0;
      cfg.y_max       = native_h - 1;
      cfg.pin_int     = 47;
      cfg.pin_rst     = 48;
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

// LGFXDisplay wrapper.
//
// We defer the real LGFX init to UITask::begin() because GPIO2 is tied to the
// SX1262 RST, and radio_init() (which pulses it low) runs after the early
// "Loading..." path in companion_radio/main.cpp. Doing display init now would
// leave the panel in an undefined state after the radio's reset pulse.
//
// begin() therefore just returns false so main.cpp's early splash is skipped.
// UITask::begin() owns the real init via getLgfx().init() and the LVGL setup.
class CrowPanelDisplay : public LGFXDisplay {
  CrowPanelLGFX _disp;
public:
  CrowPanelDisplay() : LGFXDisplay(CrowPanelLGFX::native_h, CrowPanelLGFX::native_w, _disp) {}

  bool begin() { return false; }  // see comment above

  CrowPanelLGFX& getLgfx() { return _disp; }
};
