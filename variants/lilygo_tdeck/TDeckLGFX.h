#pragma once

#include <helpers/ui/LGFXDisplay.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// LilyGo T-Deck Plus LVGL display.
//  Display: ST7789 320x240 IPS, on the SHARED SPI bus (also LoRa SX1262 + SD).
//  Touch:   GT911 capacitive (T-Deck *Plus* only), on the main I2C bus (SDA 18 / SCL 8).
//  Backlight on GPIO42 (driven by TDeckBoard / board_set_backlight).
//
// SHARED BUS: unlike CrowPanel (dedicated SPI2), the display shares one SPI host with the radio
// and SD. cfg.bus_shared = true tells LovyanGFX to re-acquire the bus each transaction; cross-core
// serialization with the radio HAL + SD is done by the bus mutex (target.cpp, MESH_PROXY) and the
// shared-bus flush path in UITask::disp_flush_cb (board_display_bus_shared()).
//
// VERIFY ON DEVICE: GT911 i2c addr (0x5D vs 0x14) + INT pin (16); ST7789 offset_x/y, invert,
// rgb_order, rotation; display SPI freq (40 MHz may be too high on the shared bus). The spi_host
// MUST match the radio/SD Arduino SPIClass host (default SPIClass = HSPI = SPI3_HOST on the S3).

#ifndef TDECK_TFT_ROTATION
  #define TDECK_TFT_ROTATION 1   // landscape (320 wide x 240 tall); flip to 3 if upside-down
#endif

class TDeckLGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Touch_GT911  _touch;

public:
  static constexpr uint16_t native_w = 240;   // ST7789 controller native (portrait); landscape via rotation
  static constexpr uint16_t native_h = 320;

  TDeckLGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI2_HOST;   // MUST match the radio/SD SPIClass(FSPI) in target.cpp. VERIFY.
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;    // VERIFY: drop (e.g. 20-27 MHz) if the shared bus glitches
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 40;
      cfg.pin_mosi    = 41;
      cfg.pin_miso    = 38;
      cfg.pin_dc      = 11;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 12;
      cfg.pin_rst          = -1;   // T-Deck ties the panel reset to the board reset
      cfg.pin_busy         = -1;
      cfg.panel_width      = native_w;
      cfg.panel_height     = native_h;
      cfg.offset_x         = 0;    // VERIFY (ST7789 240x320 window offsets)
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;    // rotation set in TDeckDisplay::begin()
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;
      cfg.invert           = true;   // ST7789 IPS typically inverted; VERIFY
      cfg.rgb_order        = false;  // VERIFY (red/blue swap -> flip this)
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;   // shared with LoRa + SD
      _panel.config(cfg);
    }

    {
      auto cfg = _touch.config();
      cfg.x_min       = 0;
      cfg.x_max       = native_w - 1;
      cfg.y_min       = 0;
      cfg.y_max       = native_h - 1;
      cfg.pin_int     = 16;     // VERIFY (T-Deck Plus GT911 INT)
      cfg.pin_rst     = -1;
      cfg.bus_shared  = false;  // touch is on I2C, not the SPI bus
      cfg.offset_rotation = 0;
      cfg.i2c_port    = 0;      // Wire (SDA 18 / SCL 8)
      cfg.i2c_addr    = 0x5D;   // VERIFY (0x5D or 0x14 depending on INT-at-reset)
      cfg.pin_sda     = 18;
      cfg.pin_scl     = 8;
      cfg.freq        = 400000;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }

    setPanel(&_panel);
  }
};

// LGFXDisplay wrapper. Mirrors CrowPanelDisplay: pass (native_h, native_w) so the UI canvas is
// 320 wide x 240 tall after the landscape rotation.
class TDeckDisplay : public LGFXDisplay {
  TDeckLGFX _disp;
public:
  TDeckDisplay() : LGFXDisplay(TDeckLGFX::native_h, TDeckLGFX::native_w, _disp) {}

  bool begin() {
    bool ok = LGFXDisplay::begin();
    if (ok) _disp.setRotation(TDECK_TFT_ROTATION);
    return ok;
  }
};
