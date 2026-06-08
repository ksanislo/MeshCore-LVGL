# CLAUDE.md

Guidance for working on this fork. Its focus is the **LVGL touchscreen companion**
firmware, now supporting **two** boards from one shared UI:
- **Elecrow CrowPanel Advance 3.5** — `elecrow_crowpanel_advance_35_companion_radio_lvgl`
- **LilyGo T-Deck Plus** — `LilyGo_TDeck_companion_radio_lvgl`

The rest of the MeshCore tree is upstream and largely untouched. Active development
branch is **`dev`** (the old `crowpanel-lvgl` branch is retired). Releases are tagged
on `dev`; `main` is fast-forwarded to the latest stable.

## Golden rule: don't fork standard MeshCore
Keep all changes at the **example/variant boundary**. Do **not** edit shared `src/`
or RadioLib — that's standard MeshCore and we track upstream. Work lives in:
- `examples/companion_radio_lvgl/ui-lvgl/` — the LVGL UI (`UITask`) + `MeshProxy`,
  variant-agnostic (shared by **both** boards)
- `examples/companion_radio/` — shared companion app (`main.cpp`, `MyMesh`,
  `DataStore`, `NodePrefs`); changes here must stay safe for **all** companion
  builds, LVGL and not (guard target-specific code with the right `#ifdef`)
- `variants/elecrow_crowpanel_advance_35/`, `variants/lilygo_tdeck/` — board/variant code

## Build & flash
PlatformIO lives in a repo-local venv. Build both LVGL targets when you touch shared UI:
```
.devtmp/venv/bin/pio run -e elecrow_crowpanel_advance_35_companion_radio_lvgl
.devtmp/venv/bin/pio run -e LilyGo_TDeck_companion_radio_lvgl
# add -t upload to flash
```
After touching shared `examples/companion_radio/*` or `NodePrefs`/`DataStore`, **also**
build a non-LVGL env (e.g. `LilyGo_TDeck_companion_radio_usb`) to confirm you didn't
break other companions. CI also checks non-ESP32 companions (e.g. RAK4631) — keep
`#include`s and FS calls portable (see `PRESET_FS` macro in `RadioPresetStore.cpp`).

Each LVGL build's post-script stages `firmware.bin` to `.devtmp/ota/<env>.bin`
(`tools/dev_ota_stage.py`) so a local HTTP server (`python3 -m http.server 8000
--directory .devtmp/ota`, host `mortar.local:8000`) can host both boards' firmware
under distinct names for the WiFi-OTA dev loop (point each device's OTA Custom URL
at its own `<env>.bin`).

The ESP32-S3 USB-CDC port (`/dev/ttyACM0`) intermittently disappears and fails a flash
mid-upload. Recovery: replug, or enter download mode (hold BOOT, tap RESET) and re-flash.

## Architecture: dual-core, shared-nothing UI ↔ mesh
The mesh/radio runs on its **own FreeRTOS task pinned to core 0** (`meshTask` in
`main.cpp`); LVGL runs on the Arduino `loop()` (**core 1**). This keeps packet crypto
from blocking the UI. Gated by `MESH_PROXY` (set for **both** LVGL envs); other builds
keep `the_mesh.loop()` on the Arduino loop.

**The UI never calls `the_mesh` directly.** It talks to the backend only through
`MeshProxy` (`ui-lvgl/MeshProxy.{h,cpp}`, namespace `mproxy`):
- **Snapshot** (backend → UI read-model): triple-buffered in PSRAM; the UI pins the
  published buffer per pass (`beginUiRead`/`endUiRead`) and reads lock-free
  (`getNumContacts`, `getContactByIdx`, `lookupContactByPubKey`, `getChannel`,
  `prefsSnap`, …). The backend rebuilds into a free buffer and swaps under a tiny index
  mutex. Republished (500 ms-throttled signature check, which covers `sizeof(NodePrefs)`)
  only on real change.
- **Command queue** (UI → backend): every write is a posted `MeshCmd` (`CMD_Send`,
  `SaveGps`, `ToggleFav`, `SetNameOvr`, `ResetPath`, `SetPath`, `AddContact`, `ReqTelem`,
  `ShareZhop`, `Advert`, `UpdatePrefs`, `ApplyRadio`, …).
- **Event queue** (backend → UI): the mesh callbacks (`newMsg`, `sentMsg`,
  `msgDelivered`, `telemetryResponse`, `msgRead`) run on the backend thread, cook their
  data, and enqueue; `UITask::drainEvents()` applies them on the UI core.

Rule of thumb: **locks for shared hardware, queues between software domains.** If you add
a UI feature that needs mesh data, read the snapshot or add a command/event — never reach
into `the_mesh` from UITask. Sending is async: an optimistic bubble (client token) is
shown, the backend replies with the real ack.

Caveat: `prefsSnap` is the backend's copy; the UI keeps a working `_node_prefs` and pushes
edits via `CMD_UpdatePrefs`. The UI periodically re-copies `prefsSnap` into `_node_prefs`,
so a UI-owned setting must round-trip through the backend (which republishes the snapshot)
to "stick" — don't assume a raw `_node_prefs` write survives.

## Shared buses differ per board — both serialized by mutexes
- **CrowPanel**: display on a **dedicated SPI2**; LoRa + SD share one HSPI
  (`SPIClass lora_spi(HSPI)`, re-pinned per access) serialized by an **HSPI mutex**
  (radio HAL brackets each transaction; `SdSvc::Lock` + the `lv_fs` driver take the same
  mutex). Touch (GT911) on I2C.
- **T-Deck Plus**: display (ST7789) + LoRa (SX1262) + SD **all on one SPI3** bus,
  serialized by `hspi_*` mutex — and the LVGL flush also takes it (`board_bus_lock`,
  the closed-per-flush path in `disp_flush_cb` gated by `board_display_bus_shared()`).
  SD shares LoRa's exact pins (only CS differs → no re-pin). A second mutex (`board_i2c_lock`)
  serializes the **shared I2C bus**: GT911 touch + BlackBerry keyboard (0x55) + RTC, all
  on `Wire` (SDA18/SCL8). Trackball is 4 GPIO pulse-counters (IRAM ISRs) + click on GPIO0.

## Variant-agnostic UITask via weak `board_*` hooks
`UITask` derives resolution/rotation from the LGFX device and knows the hardware **only**
through weak `board_*` hooks (default no-op/false in UITask.cpp; strong overrides in each
variant's `target.cpp`). Keep variant specifics behind a hook, never in UITask. Current hooks:
`board_set_backlight`, `board_touch_int_pin`, `board_display_bus_shared`/`board_bus_lock`/
`board_bus_unlock`, `board_i2c_lock`/`board_i2c_unlock`, `board_has_physical_kbd`/
`board_kbd_read`, `board_has_trackball`/`board_trackball_read`, `board_has_builtin_battery`/
`board_batt_millivolts`/`board_batt_current_ma`/`board_set_power_monitor`. Settings UI that's
only meaningful on some hardware is gated on these (e.g. the trackball Scroll-speed slider on
`board_has_trackball()`; the battery sensor selector hidden when `board_has_builtin_battery()`).

## Persisting a new setting
`NodePrefs` (companion_radio/NodePrefs.h) is serialized field-by-field by `DataStore`. To add one:
1. Append a struct member at the **tail** of `NodePrefs`.
2. Add a matching `file.read`/`file.write` at the tail of `loadPrefsInt`/`savePrefs`, in the
   **same order** (the format is a fixed byte layout with `// <offset>` comments + `pad`
   writes; read and write must stay byte-identical — an old/short file leaves new fields at
   their default, since `file.read` no-ops at EOF).
3. Set its default in **`DataStore::applyAppendedPrefsDefaults()`** (NOT inline in the read
   path). That helper is the single source of truth for appended-field defaults and runs on
   **both** the file-read path *and* the fresh-device no-file path (`loadPrefs` else branch).
   Putting a default only in the read path means a fresh device (no prefs file) never gets it
   — that was the `persist_history` default-off bug. Use `0`/`0xFF`/`0xFFFF` as the "unset"
   sentinel where the UI distinguishes unset from a real value.

UI-side: edits set `_node_prefs->field` then `pushPrefs()` (CMD_UpdatePrefs → backend
`savePrefs`). **Slider** settings: persist on `LV_EVENT_VALUE_CHANGED` (debounced via
`markPrefsDirty()` → flushed in `loop()`), NOT on `LV_EVENT_RELEASED` — the release can be
swallowed by the settings pane's scroll on small screens, and a non-VALUE_CHANGED event can
read the slider as 0.

## Selectable font size + tier-scaled metrics
`NodePrefs.font_scale` (0=auto, 1/2/3 = Small/Med/Large) picks a tier once at boot
(`s_font_tier`; auto = by screen width). The type ramp (`fontHero/Title/Heading/Body/Caption`)
indexes `FONT_RAMP[tier]`, and structural sizes (row/avatar/header/tab/compose/search) index
`METRICS_RAMP[tier]` exposed as `g_ui_metrics` via macros (`UI_CONTACT_ROW_H`, `HEADER_H`, …).
Large == the original constants (CrowPanel unchanged). Restart-to-apply. New ramp/montserrat
sizes must be enabled in `lv_conf.h`.

## Emoji packs must match the font ramp
Color emoji render from per-size SD bundles `/emoji/<size>.bin`, downloaded from the
`emoji-emj1` GitHub release (`EMOJI_PACK_FMT`). `EMOJI_SIZES` (SdCard.h) **must be the union
of all `FONT_RAMP` sizes** or emoji fall back to the wrong-size mono set. To add a size:
generate the bundle (`tools/emoji_pack.py`, Twemoji PNGs at `.devtmp/twemoji/assets/72x72`,
needs Pillow in the venv), gzip it, and **publish `emoji-emj1-<size>.bin.gz` to the
`emoji-emj1` release BEFORE shipping firmware** — the downloader aborts the whole pack on the
first missing size. Keep `EMOJI_SIZES` + `tools/make-emoji-pack.sh` + `emoji_pack.py` in sync.

## UITask conventions
- **Reuse shared UI components — don't fork per screen.** Every list/card/field/modal should
  come from one shared primitive so the UX stays consistent and tweaks propagate. Reuse (and
  parameterize) the virtualized contact-list recycler (Contacts tab *and* the share/insert
  picker), `makeHeroCard` (contact info + owner profile), `makeModalCard`, `makeHeaderBar`/
  `makeBackButton`, `makeField`/`makeNumberField`/`makeDropdownField`, `makePassive`, the
  type-ramp fonts, and `ui_theme.h` tokens. Differentiate modes via config + the page header,
  not by cloning widget code.
- Lists (`rebuildContactsList`/`rebuildChannelsList`) are heavy; rebuild only when the relevant
  tab is on screen, on snapshot-version change, lightly throttled.
- "Last heard" uses `ContactInfo.lastmod` (our clock), never `last_advert_timestamp` (the
  remote's clock, which bad RTCs report wildly).
- Don't hold `SdSvc::Lock` for multi-second spans — it starves core 0 → task WDT reset. Batch
  reads and release.

## Releasing
Single version for all LVGL variants: `[meshcore_lvgl] gui_version` in the elecrow
`platformio.ini` (each variant references `${meshcore_lvgl.gui_version}`). Per release: bump it,
build both LVGL envs (+ a non-LVGL sanity build), commit on `dev`, build clean (so `FW_GIT_REV`
isn't `-dirty`), stage versioned assets `<OTA_ASSET_PREFIX>-<ver>-<sha7>.{bin,bin.md5,-merged.bin}`
for each variant, push `dev`, then `gh release create <ver> --target dev` (`--prerelease` for rc,
`--latest` for final). Keep release notes SHORT (they count toward the 128 KB release-list JSON the
OTA picker parses). Emoji packs are a separate `emoji-emj1` release. Never push without explicit
user approval.
