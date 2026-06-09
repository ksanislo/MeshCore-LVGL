# Scoping: porting the LVGL companion to LilyGo T-Display P4 (ESP32-P4)

Status: **feasibility only — no code, no hardware in hand.** Written 2026-06.
Verdict up front: **feasible but it's a platform port, not a variant add.** Two of the three
pillars our UI rests on (LovyanGFX display, native BLE/WiFi radio) do not exist on ESP32-P4 and
must be re-backended. This is weeks of work with a hard hardware dependency, versus the few-day
"new variant" the CrowPanel 2.4–7.0 siblings were. Park unless there's real demand + hardware.

---

## Why the CrowPanel siblings were cheap and this isn't

The 2.4/2.8/4.3/5.0/7.0 CrowPanels were a few-day, blind-buildable add because they share the
**exact stack** of our reference board: ESP32-S3, stock `espressif32` Arduino, LovyanGFX display,
native BLE + WiFi. A variant is then just pins + an LGFX header + a `platformio.ini` env
(see CLAUDE.md "variant boundary"; ~9 files, zero changes to shared code).

The P4 shares the *application* (mesh, UITask logic, MeshProxy) but **none of the three platform
pillars**. Each pillar that differs forces work *below* the variant boundary, which is exactly
what the golden rule tells us to avoid — so this port stresses the architecture, it doesn't slot
into it.

---

## The three pillars, by severity

### 1. Display — the real wall (LovyanGFX has no P4 path *at all*)
- Our entire UI — `UITask`, `MapView` (incl. the new prefetch engine), `LGFXDisplay`,
  `MapThumb` — renders through **LovyanGFX**. The pinned LovyanGFX (`lovyan03/LovyanGFX@^1.2.7`)
  ships **no `esp32p4` platform directory at all** — verified in the installed package: no
  `esp32p4` dir, no DSI panel, *and no `esp32p4/Bus_RGB.hpp`* either.
- **This is the key correction to the naive read:** the wall is *not* "DSI vs RGB." LovyanGFX
  drivers are **per-SoC** (`esp32`, `esp32s3`, `esp32c3`, …). Our RGB big-CrowPanels work only
  because they bolt to **`lgfx/v1/platforms/esp32s3/Bus_RGB.hpp`** — there is no `esp32p4`
  equivalent. So **any** P4 board needs a non-LovyanGFX display backend regardless of panel type:
  feed LVGL from Espressif's **`esp_lcd`** driver directly (an LVGL `flush_cb` over an
  `esp_lcd_panel_handle_t`), plus a new touch path without LovyanGFX's `Touch_*` helpers.
- The interface only decides *which* `esp_lcd` backend you write — see the two-path split below.
- Knock-on: anywhere we touch LovyanGFX outside the flush path (rotation, any `lgfx::` type)
  needs a P4 shim. `board_set_backlight` is already a weak hook, so backlight is clean; the
  panel/touch bring-up is the cost.

#### Two distinct P4 display paths (different boards, different risk)
The "P4 with RGB" boards and the T-Display P4 are **different efforts** — picking the right entry
point matters if a P4 port is ever greenlit.

- **Path A — RGB CrowPanel Advance P4 (LOWER risk, the sane entry point).** Meshtastic's
  `crowpanel-p4` branch targets exactly this: envs `crowpanel-advanced-p4-50` and
  `crowpanel-advanced-p4-70-90-101` = CrowPanel Advance **5.0 / 7.0 / 9.0 / 10.1**, RGB-parallel,
  but on **ESP32-P4** silicon. This is the **same physical product family we already build on S3**
  (`elecrow_crowpanel_advance_50/_70`, `board = esp32-s3-devkitc-1`, RGB 800×480 via
  `esp32s3/Bus_RGB`). The split is **chip, not panel** — same Elecrow timings, same `CROWPANEL_RGB`
  board-platform branch, same UITask. The 9/10.1 sizes are effectively **P4-only** (S3 can't push
  that pixel bandwidth). The display work is a **single `esp_lcd` RGB → LVGL flush** (well-trodden
  on P4) reusing our known panel timings — much smaller than DSI. **If we ever do P4, start here.**

- **Path B — LilyGo T-Display P4 (HIGHER risk, the original question).** A small board, almost
  certainly **MIPI-DSI**. DSI-via-`esp_lcd` is real but less trodden, the panel/touch controllers
  are unknown until we have the schematic, and there's **no ecosystem reference for it** — even
  Meshtastic's P4 config enables only `CONFIG_SOC_LCD_I80_SUPPORTED` + `CONFIG_SOC_LCD_RGB_SUPPORTED`,
  **not** DSI. This stacks the DSI unknown on top of everything in Path A.

Both paths carry the **no-native-radio / C6 co-processor** connectivity cost (pillar 2) equally —
that's chip-level, independent of the display. So Path A isolates the connectivity port without
also gambling on DSI; Path B does not. Neither reuses our LovyanGFX RGB code.

### 2. Connectivity — the P4 has no native radio
- ESP32-P4 has **no native WiFi and no native BLE**. Our firmware leans on both:
  - **BLE companion** (the phone app — `BLE_PIN_CODE`, the whole NimBLE companion path).
  - **Native WiFi** — `WITH_WIFI`, `WITH_MQTT_BRIDGE`, NTP, and **the manifest-OTA system**
    (`MyMesh::updateReleaseList()` + the hourly refresh) just reworked.
- Meshtastic's answer (from `variants/esp32p4/esp32p4.ini`): **ESP-Hosted + an ESP32-C6
  co-processor over 4-bit SDIO**, with BLE tunneled as **NimBLE-over-VHCI**. Key sdkconfig:
  `CONFIG_BT_CONTROLLER_DISABLED=y`, `CONFIG_ESP_WIFI_REMOTE_ENABLED=y`,
  `CONFIG_ESP_HOSTED_ENABLED=y`, `CONFIG_ESP_HOSTED_TRANSPORT_SDIO=y`,
  `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c6"`, `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y`,
  `CONFIG_ESP_HOSTED_NIMBLE_HCI_VHCI=y`, `CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS=1500`.
- **Hard prerequisite:** the T-Display P4 must actually *carry a C6/H2 co-processor* wired for
  SDIO. If it doesn't, there is **no WiFi/BLE at all** and the board is USB-serial-companion only
  (acceptable for a mesh node, but it kills the phone app + WiFi-OTA dev loop we depend on).
- Re-plumbing scope: the BLE companion transport and the WiFi/MQTT/NTP/OTA stack would have to
  run against `esp_wifi_remote` + hosted-NimBLE instead of the native stacks. This is below the
  variant boundary (touches `examples/companion_radio` connectivity), and must stay `#ifdef`-gated
  so it can't regress the S3 boards.

### 3. Framework/toolchain — the easy pillar (already half-done)
- The **pioarduino fork** that adds P4 (Arduino 3.x / IDF 5.3) is **already in our tree**:
  `esp32c6_base` uses
  `https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13-1/platform-espressif32.zip`.
  An `esp32p4_base` extends the same platform — `board_build.mcu = esp32p4`, a P4 partition
  table, PSRAM config. Mechanically straightforward; the risk is library-compat churn on the
  newer IDF (our lib_deps were validated against `espressif32@6.11.0`).
- Note `merge_factory.py` hardcodes `--chip esp32s3`; a P4 variant needs `esp32p4` (and possibly
  different flash offsets). Trivial, but don't forget it.

---

## What LoRa/SD look like (the parts that *do* port)
- **LoRa/RadioLib**: ports cleanly. SX1262 over SPI works on P4; our `HspiLockHal` bus-mutex
  pattern is chip-agnostic. Just need the T-Display's real SX126x pin map.
- **SD**: P4 boards typically use **SDMMC** (4-bit), not SPI SD. Meshtastic sets
  `CONFIG_ARDUINO_SELECTIVE_SD_MMC=y`. Our `SdFat`/`SdSvc` + the `lv_fs` driver + the bus-share
  mutex assume **SPI SD**. Either wire the T-Display's SD as SPI (if the board exposes it) or add
  an SDMMC path to `SdSvc` — the latter is real work and touches the map/emoji/chat-store SD code.
- **LDO/GPIO caveat** (from Meshtastic's P4 `pins_arduino.h`): high-numbered GPIOs hit LDO power
  issues; SPI pins can collide with SDMMC slot0 and need a "SDMMC POWER" bus-type pre-tag in
  `variant.cpp`. Budget time for board-bring-up gotchas like this.

---

## What Meshtastic actually gives us (and what it doesn't)
**Gives:**
- `variants/esp32p4/esp32p4.ini` (main) — generic P4 SDK config: pioarduino platform, the full
  C6/SDIO ESP-Hosted + NimBLE-over-VHCI setup, SDMMC, component-prune list, linker response-file
  workaround. The blueprint for pillar 2 & 3.
- `origin/crowpanel-p4` branch — a *real working* P4 variant (`variant.h/.cpp`, `pins_arduino.h`,
  `boards/crowpanel-p4.json`, LoRa pins, SDMMC, LDO workarounds). Good structural template.

**Doesn't give:**
- **Anything T-Display P4 specific** — no pin map, no display config, no board JSON.
- **A MIPI-DSI display path** — their P4 boards are RGB parallel; DSI is unsolved ecosystem-wide.
- The `crowpanel-p4` branch is **unmerged** (expect churn / not battle-tested).

---

## Rough effort shape (if we ever do it)
1. **De-risk the toolchain (days):** `esp32p4_base` env (pioarduino) + LoRa-only build, no display,
   no BLE/WiFi. Proves the platform + RadioLib compile and a node joins the mesh over USB.
2. **Display backend (1–2 weeks):** `esp_lcd` MIPI-DSI → LVGL flush + a touch driver, behind the
   existing weak `board_*` hooks where possible; new shim where LovyanGFX types leak. **Highest
   risk; gated entirely on knowing the exact panel + touch controllers.**
3. **Connectivity (1–2 weeks):** ESP-Hosted/C6 bring-up; re-target BLE companion + WiFi/MQTT/NTP/OTA
   onto hosted stacks, `#ifdef`-gated so S3 builds are untouched. **Only if the board has a C6.**
4. **SD + polish (days):** SDMMC path or SPI-SD wiring; LDO/GPIO fixes; OTA asset prefix + merge
   script `--chip esp32p4`; new `gui_version`-tracked release env.

Net: **~3–5 weeks of platform engineering**, most of it below the variant boundary, contingent on
(a) a MIPI-DSI panel we can identify and drive via `esp_lcd`, and (b) an on-board C6 for WiFi/BLE.

---

## Decision gates before any code
1. **Pick the path.** If P4 is ever greenlit, **Path A (RGB CrowPanel Advance P4)** is the entry
   point — it isolates the connectivity port (pillar 2) and the `esp_lcd` RGB backend without the
   DSI gamble, and reuses our existing S3 RGB panel timings + `CROWPANEL_RGB` branch + UITask.
   **Path B (T-Display P4 / DSI)** only after A proves the platform, or if there's specific demand
   for that board.
2. **Get the board schematic / vendor docs** for whichever path: display controller + interface,
   touch controller, **whether a C6/H2 co-processor is present and how it's wired (SDIO pins)**,
   SX126x LoRa pins, SD interface (SDMMC vs SPI), battery ADC. For Path A, Meshtastic's
   `origin/crowpanel-p4` branch already has most of this.
3. **Confirm demand.** This is a from-scratch platform; the S3 line covers current users.
4. If clear, start at effort step 1 (toolchain de-risk) — cheap, validates pioarduino before
   sinking time into display/connectivity.

(See also memory `project-esp32p4-tdisplay-port` for the one-line index entry.)
