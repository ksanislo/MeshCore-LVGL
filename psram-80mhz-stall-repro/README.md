# CrowPanel Advance 3.5 — octal-PSRAM-@80MHz read-stall boot loop

A reproduction archive for a deterministic boot loop on the **Elecrow CrowPanel
Advance 3.5** (ESP32-S3, octal/OPI PSRAM @ 80 MHz) LVGL companion firmware.
Captured so the bug stays reproducible after the main codebase moves past it.

> **One-line summary (proven):** with a *specific* set of chat logs on the SD card,
> the boot-time history preload (`SdMessageStore::preloadRecent`) leaves the system in a
> state where a subsequent **read of a valid, in-pool PSRAM address stalls the CPU**; the
> interrupt watchdog then resets the board → endless splash loop. It is a **memory-bus
> read STALL, not data corruption**, and it is **knife-edge sensitive to the exact data**
> (adding ~2 unrelated log lines makes it vanish) and to **binary layout**.

Archive built against branch `crowpanel-lvgl`, commit `c6dd0ceed73b412ee6c8083c870dfa78a2a7720b`,
env `ElecrowCrowPanelAdvance35_companion_radio_lvgl_ble`. Date: 2026-06-02.

---

## 1. Hardware / environment (exact)

- Board: Elecrow CrowPanel Advance 3.5, ESP32-S3, **octal (OPI) PSRAM @ 80 MHz**
  (`CONFIG_SPIRAM_SPEED_80M`, `CONFIG_SPIRAM_MODE_OCT`).
- `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`
  (heap allocations > 4 KB are eligible for PSRAM).
- LVGL pool: `ps_malloc(256 KB)` in PSRAM, base ≈ `0x3d880800` (`LV_MEM_CUSTOM=0`).
- Mesh/radio on its own FreeRTOS task pinned to **core 0** (`meshTask`), gated behind
  `s_ui_ready` until UI init done; LVGL/`setup()`/`loop()` on **core 1** (`loopTask`).
- LoRa + SD share one HSPI bus (`lora_spi`), re-pinned per access, serialized by a
  FreeRTOS mutex (`hspi_mutex`); display on dedicated SPI2; touch GT911 on I2C.
- SD pins GPIO 4–7; LoRa pins GPIO 0,1,2,3,9,10,46. **None overlap the OPI PSRAM
  pins (26–37).**

## 2. Symptom

Board shows the splash, resets, shows the splash again — forever. Reset reason on
each loop is **`rst:0x8 (TG1WDT_SYS_RST)`** (Timer-Group-1 / interrupt watchdog), with
**no Guru Meditation / no backtrace printed** (the CPU is wedged on a memory read and
can't even run the panic handler before the WDT resets it). See
`captures/01-pristine-head-bootloop.log`.

## 3. Reproduction recipe (exact, proven)

### 3a. Arm the SD card
SdFat enumerates the directory in **creation order**, and the order matters (see §6),
so rebuild `/chat/` exactly:

```bash
M=/path/to/sdcard/chat
rm -f "$M"/*            # clear it (NOTHING else may live in /chat)
for f in 7BEAC6073EC6 ch_000000000000 ch_4DD75E2D5BAE ch_8B3387E9C5CD \
         ch_9CD8FCF22A47 ch_CDE5E82CF515 ch_FD2931E0DD57 DD7AE62BF96A E59DF8DC4BE5; do
  cp logs/$f.log "$M"/ ; sync
done
ls -la "$M"            # verify 7BEAC6073EC6.log is FIRST and there is no stray file
```
Byte-exactness matters — verify against `logs/MD5SUMS.txt`. The full set is in `logs/`.

### 3b. Flash a reproducer and observe
Flash to offset `0x10000`. Two binaries (`firmware/`):

- **`pristine-head.bin`** — the unmodified committed firmware (commit above). Expected:
  immediate **TG1WDT boot loop** (multiple `ESP-ROM` banners, `rst:0x8`). This is the
  real, end-to-end symptom. (VERIFIED on the live card — `captures/VERIFY-pristine-head-loops.log`.)
- **`diagnostic-probe.bin`** — same code + a built-in pass/fail probe and **all watchdogs
  disabled** (`killAllWatchdogs()`), so the wedge **hangs** (JTAG-inspectable) instead of
  resetting. After preload it prints `[PROBE] post-preload: stall core0 + read 256KB pool...`
  then:
  - **poisoned** → it hangs there; `pool read COMPLETED` is **never** printed.
  - **clean**    → it prints `[PROBE] pool read COMPLETED ... NOT poisoned this boot` and boots on.
  (VERIFIED: on the armed card it hangs, no COMPLETED — `captures/VERIFY-diagnostic-probe-wedges.log`.)

Flashing over JTAG (OpenOCD running, user-launched `sudo openocd -f board/esp32s3-builtin.cfg`):
```
# via telnet :4444
reset halt
program_esp firmware/pristine-head.bin 0x10000 verify
reset halt
# start a serial reader on /dev/ttyACM0 BEFORE resuming, then: resume
```

## 4. What is PROVEN by test (the experiment matrix)

The probe in `diagnostic-probe.bin` (`esp_cpu_stall(0)` then read the whole 256 KB pool
on core 1) is the pass/fail oracle. Each row was run on the armed card; results were
reliable across the noted repeats.

| # | SD reads | parse | data parsed | result | capture |
|---|----------|-------|-------------|--------|---------|
| pristine HEAD | real | real | real SD bytes | **LOOP** (6/6) | 01-pristine-head-bootloop.log |
| B1 repin-only | none (300× `lora_spi.end/begin`, no I/O) | none | — | clean | B1-repin-only-CLEAN.log |
| B2 reads-only | real | **skipped** | — | clean (3/3) | B2-reads-no-parse-CLEAN.log |
| parser-only ×200 / ×20000 | none | real code | **synthetic** | clean (2/2 each) | SYN-parser-only-20k-CLEAN.log |
| scattered-ts + full insertion sort | none | real code | synthetic, shuffled ts | clean (3/3) | SHIFT-scattered-ts-sort-CLEAN.log |
| COMBO | real (discarded) | real code | **synthetic** | clean (2/2) | COMBO-realreads+synthparse-CLEAN.log |
| real preload / decoupled read-then-parse | real | real | **real SD bytes** | **WEDGE** | decouple-fix-STILL-LOOPS.log |

**Conclusions proven by the above:**
- It is **not** the SD reads alone (B2 clean), **not** the bus repinning (B1 clean),
  **not** the parser code alone (synthetic ×20000 clean), **not** the insertion-sort
  240-byte struct memmoves (scattered-ts clean), and **not** "SD I/O then any work"
  (COMBO clean). The poison requires the parser to run over **the real SD byte content**.
- **It is a read stall, not corruption.** A per-chunk hash of the 256 KB pool taken
  *before* vs *after* preload shows **0 changed bytes** — the pool data is intact; a
  later *read* of it stalls.
- **It is core-1-local.** With core 0 **hardware-stopped** via `esp_cpu_stall(0)`,
  core 1 still wedges reading the pool (`captures/03-core0-hwstalled-still-wedges.log`).
  Not a cross-core HSPI race. (NB: there *was* a separate, already-fixed core0/core1
  startup HSPI race — see the `meshTask` gate comment in `main.cpp` — this is not that.)

### Faulting state (JTAG) — see `gdb/panic-frame-analysis.txt`
Core 1 is hung in the pool-read loop; ~300 ms later `xt_highint4` (interrupt WDT) fires
`panicHandler` with **`pseudo_excause=true`, `exccause=7`, `excvaddr=0`** — a *timeout*,
not a load/store fault. The load pointer (`a13`) is **`0x3d887660`** — a fixed, valid,
linearly-computed, in-pool address that the *pre*-preload scan read fine. So a
normally-readable PSRAM word becomes un-readable (its cache-line fill never returns).

## 5. Mitigations that were tried and PROVEN NOT to fix it
All still wedge reliably on the armed card:
- Forcing the preload scratch buffer (`recent`, 11.5 KB) to **internal RAM** instead of
  PSRAM (`heap_caps_malloc(MALLOC_CAP_INTERNAL)`). `captures/recent-internal-STILL-WEDGES.log`
- **Decoupling** parse from the bus: read each file's tail under the lock, release the
  HSPI bus, then parse with the bus idle. `captures/decouple-fix-STILL-LOOPS.log`
- **Yielding** (`vTaskDelay(1)`) every 8 records during the parse.
- A 150 ms **settle delay** after preload.
- Full data-cache **writeback + invalidate** (`Cache_WriteBack_All` + `Cache_Invalidate_DCache_All`)
  after preload.

## 6. The crux: extreme sensitivity (PROVEN observations)
- **Data:** the bug stopped reproducing after ~2 unrelated channel messages were appended
  to the live logs (`ch_9CD8FCF22A47.log` 464→496 B, `ch_CDE5E82CF515.log` 3720→3794 B);
  re-arming required restoring the **byte-identical** original logs (md5-verified) **and**
  the original directory order. A few bytes in an unrelated file toggle it.
- **Read order:** `7BEAC6073EC6.log` (73 records) must be enumerated **first** (read into
  an empty buffer). When file order changed so it was read last, the workload changed.
- **Binary layout:** many source edits (diagnostic scaffolding) toggled reproduction by
  themselves — the classic signature of code-placement moving the workload on/off a margin.

This is why no synthetic input reproduces it and why "find the one bad record" is a dead
end: there is **no logical property** to reproduce. The exact aggregate of addresses,
values, and bus operations this *specific* dataset produces lands on the wrong side of a
**marginal PSRAM timing window**; perturb it by a hair and it lands on the safe side.

## 7. What is NOT known / NOT proven
- The exact micro-mechanism (why parsing *these* bytes makes a later pool read stall).
- **Whether dropping PSRAM to 40 MHz fixes it — UNTESTED.** It very likely boots clean,
  but by the same marginal-nature argument, a clean 40 MHz boot would *not prove*
  robustness — it would just be another setup sitting on the safe side for this data.
  A real fix has to be argued from "moves the timing margin so no normal workload lands
  on it," not from "this dataset now boots."
- Whether it is temperature / PSRAM-warm-up dependent (deferring the load until later in
  boot was **not** tested).
- Whether specific byte values (e.g. UTF-8/emoji high bytes present in the real logs) vs
  aggregate workload are responsible — not isolated.
- Whether the 256 KB-read probe is a perfect proxy for the real `buildHomeScreen` failure
  (it correlates on every test, and the real symptom also loops, so it is representative —
  but it is a proxy).

## 8. Files in this archive
```
README.md                              this document
logs/                                  the exact trigger data (9 files)
  MD5SUMS.txt, SIZES.txt, ORDER.txt    byte hashes, sizes, required dir order
firmware/
  pristine-head.bin                    unmodified committed reproducer (real loop)
  diagnostic-probe.bin                 + esp_cpu_stall probe + WDTs off (hang, COMPLETED signal)
  COMMIT.txt                           commit hash / branch / env
patches/
  diagnostic-build.patch               git diff (HEAD -> the diagnostic-probe source)
captures/                              serial logs + JTAG for every row in §4/§5
gdb/
  panic-frame-analysis.txt             the faulting-frame decode (addr 0x3d887660)
```

## 9. Tooling notes (how to re-inspect)
- OpenOCD is user-launched (`sudo openocd -f board/esp32s3-builtin.cfg`); drive via
  telnet `:4444` and gdb `:3333`.
- esp-gdb: `.devtmp/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb` (pio's gdb lacks libpython2.7).
- **JTAG reads of cached PSRAM (`0x3d…`) are unreliable** — trust on-device CPU reads /
  register values, not `x/` dumps. HW data watchpoints fire on internal RAM but NOT on
  cached PSRAM.
- To rebuild `diagnostic-probe.bin`: `git checkout <commit>`, `git apply patches/diagnostic-build.patch`,
  `pio run -e ElecrowCrowPanelAdvance35_companion_radio_lvgl_ble`.
- The probe lives in `UITask::begin()` right after `preloadRecent`; the WDT-kill is
  `killAllWatchdogs()` in `main.cpp`. Both are diagnostic-only and must never ship.
