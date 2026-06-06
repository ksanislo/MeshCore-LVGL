#pragma once
#include <Arduino.h>

// Runtime gate for the WiFi firmware-update diagnostics ([REL] release fetch + [OTA] download).
//
// One build, BLE included (we want Bluetooth). But BLE and WiFi can't coexist on this chip, so when WiFi
// mode is turned on to update, the companion frame protocol is driven over USB Serial instead of BLE
// (see main.cpp). The update path only runs in WiFi mode, so its prints would land on -- and corrupt --
// that companion stream. In normal (BLE) mode the USB-serial port is free and these prints are fine.
//
// So this is a runtime switch, not a build option: main.cpp clears the flag when WiFi mode binds the
// companion to USB Serial. The check sits BEFORE the printf, so when it's off the format string and all
// its arguments are never evaluated -- the dropped logs cost nothing, no string built just to bit-bucket.
extern bool g_dbg_serial;     // true = diagnostics -> Serial (BLE mode); false = dropped (WiFi mode)
#define DBG_LOGF(...) do { if (g_dbg_serial) Serial.printf(__VA_ARGS__); } while (0)
