#pragma once

// Fuel-gauge state shared across cores: written by the UI estimator (UITask, core 1, which runs even
// while the backlight is off so tracking never pauses) and read by the backend telemetry builder
// (MyMesh, core 0). Plain volatile ints -- 32-bit aligned, no tearing on either core, no lock needed.
extern volatile int g_batt_pct;   // 0..100 estimated state-of-charge, or -1 = no monitor / no reading
extern volatile int g_batt_mv;    // pack voltage (mV)
extern volatile int g_batt_ma;    // signed current (mA): + charging, - discharging
