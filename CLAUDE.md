# CLAUDE.md

Guidance for working on this fork, whose focus is the **Elecrow CrowPanel Advance
3.5 LVGL companion** firmware. Most notes below are specific to that target; the
rest of the MeshCore tree is upstream and largely untouched.

## Golden rule: don't fork standard MeshCore
Keep all changes at the **example/variant boundary**. Do **not** edit shared
`src/` or the RadioLib library — that's standard MeshCore and we track upstream.
Work lives in:
- `examples/companion_radio_lvgl/ui-lvgl/` — the LVGL UI + `MeshProxy`
- `examples/companion_radio/` — shared companion app (`main.cpp`, `MyMesh`,
  `DataStore`, `NodePrefs`); changes here must stay safe for **all** companion
  builds (guard target-specific code with the right `#ifdef`)
- `variants/elecrow_crowpanel_advance_35/` — board/variant code

## Build & flash
PlatformIO lives in a repo-local venv:
```
.devtmp/venv/bin/pio run -e ElecrowCrowPanelAdvance35_companion_radio_lvgl            # build
.devtmp/venv/bin/pio run -e ElecrowCrowPanelAdvance35_companion_radio_lvgl -t upload  # flash
```
After touching shared `examples/companion_radio/*` or `NodePrefs`/`DataStore`,
also build a non-LVGL env (e.g. `LilyGo_TDeck_companion_radio_usb`) to confirm
you didn't break other companions.

The ESP32-S3 USB-CDC port (`/dev/ttyACM0`) intermittently disappears and fails a
flash mid-upload. Recovery: replug, or enter download mode (hold BOOT, tap
RESET) and re-flash.

## Architecture: dual-core, shared-nothing UI ↔ mesh
The mesh/radio runs on its **own FreeRTOS task pinned to core 0** (`meshTask` in
`main.cpp`); LVGL runs on the Arduino `loop()` (**core 1**). This keeps packet
crypto from blocking the UI. Gated by the `MESH_PROXY` build flag (set for the
CrowPanel LVGL envs); other builds keep `the_mesh.loop()` on the Arduino loop.

**The UI never calls `the_mesh` directly.** It talks to the backend only through
`MeshProxy` (`ui-lvgl/MeshProxy.{h,cpp}`, namespace `mproxy`):
- **Snapshot** (backend → UI read-model): triple-buffered in PSRAM; the UI pins
  the published buffer per pass (`beginUiRead`/`endUiRead`) and reads lock-free
  (`getNumContacts`, `getContactByIdx`, `lookupContactByPubKey`, `getChannel`,
  `prefsSnap`, …). The backend rebuilds into a free buffer and swaps under a tiny
  index mutex. Republished (500 ms-throttled signature check) only on real change.
- **Command queue** (UI → backend): every write is a posted `MeshCmd`
  (`CMD_Send`, `SaveGps`, `ToggleFav`, `SetNameOvr`, `ResetPath`, `SetPath`,
  `AddContact`, `ReqTelem`, `ShareZhop`, `Advert`, `UpdatePrefs`, `ApplyRadio`).
- **Event queue** (backend → UI): the five mesh callbacks (`newMsg`, `sentMsg`,
  `msgDelivered`, `telemetryResponse`, `msgRead`) run on the backend thread, cook
  their data, and enqueue; `UITask::drainEvents()` applies them on the UI core.

Rule of thumb: **locks for shared hardware, queues between software domains.** If
you add a UI feature that needs mesh data, read the snapshot or add a command/
event — never reach into `the_mesh` from UITask. Sending is async: an optimistic
bubble (client token) is shown, the backend replies with the real ack.

## HSPI bus is shared (LoRa + SD)
`variants/.../target.cpp`: one `SPIClass lora_spi(HSPI)` drives both the radio
and the SD card (re-pinned per access). A shared **HSPI mutex** serializes them
across cores: a custom `RadioLibHal` brackets each radio SPI transaction;
`SdSvc::Lock` and the `lv_fs` driver take the same mutex across their bus span.
The display is on **SPI2 (dedicated)**, not this bus. Touch (GT911) is on I2C.

## Persisting a new setting
`NodePrefs` (companion_radio/NodePrefs.h) is written/read field-by-field by
`DataStore` (`save/loadPrefsInt`). Add new UI settings as **appended fields at
the tail**: a struct member at the end, plus a matching `file.read`/`file.write`
in the same order. Old (shorter) prefs files leave the new field untouched, so
default it (0 / 0xFF = unset) before the read. See `display_brightness`,
`screen_timeout_s`, etc.

## UITask conventions
- **Reuse shared UI components — don't fork per screen.** A design goal: every
  list/card/field/modal should come from one shared primitive so the UX stays
  consistent and tweaks propagate everywhere. Reuse (and parameterize behavior on)
  the virtualized contact-list recycler (Contacts tab *and* the share/insert
  picker), `makeHeroCard` (contact info + owner profile), `makeModalCard`,
  `makeHeaderBar`/`makeBackButton`, `makeField`/`makeNumberField`/
  `makeDropdownField`, `makePassive`, the type-ramp fonts, and `ui_theme.h` tokens.
  Differentiate modes via config + the page header, not by cloning widget code.
- Variant-agnostic: it derives resolution/rotation from the LGFX device and only
  knows the weak `board_set_backlight()` hook — keep variant specifics out of it.
- Lists (`rebuildContactsList`/`rebuildChannelsList`) are heavy; rebuild only when
  the relevant tab is on screen, on snapshot-version change, lightly throttled.
- "Last heard" uses `ContactInfo.lastmod` (our clock), never `last_advert_timestamp`
  (the remote's clock, which bad RTCs report wildly).
