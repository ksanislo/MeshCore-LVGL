#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Display: ILI9488, 480x320, SPI2 host @ 40 MHz.
// Touch:   GT911 capacitive, I2C0 @ 400 kHz, addr 0x14.
// Backlight on GPIO38, driven by CrowPanelBoard at boot.
// GPIO2 is shared with SX1262 RST; we leave panel reset to LoRa's pulse and
// init LGFX lazily from UITask::begin() after radio_init() has fired.

// 0 = native portrait (320x480, USB at bottom)
// 1 = 90 deg CW   landscape (480x320, USB on left)
// 2 = 180 portrait        (320x480, USB at top)
// 3 = 90 deg CCW landscape (480x320, USB on right)
// Override per-env in platformio.ini with -D CROWPANEL_TFT_ROTATION=N.
#ifndef CROWPANEL_TFT_ROTATION
  #define CROWPANEL_TFT_ROTATION 0
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
      cfg.freq_write  = 40000000;   // SPI pins route via GPIO matrix (not IOMUX), so ~40 MHz ceiling
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
      cfg.offset_rotation  = 0;  // rotation set explicitly in CrowPanelDisplay::begin()
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
// Note on GPIO2: TFT RST is tied to SX1262 RST. When radio_init() runs later
// in setup() it pulses GPIO2 low, which also resets the panel and wipes any
// init state we pushed here. The "Loading..." splash drawn between display
// init and radio init is therefore visible only briefly. UITask::begin() must
// re-init the panel (lgfx.init() again) after the radio is up before
// rendering LVGL content. pin_rst is set to -1 in the panel config so this
// class never pulses GPIO2 itself.
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
