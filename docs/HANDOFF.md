# WatchaGotchi — agent handoff

Written 2026-08-16 so any agent (or human) can resume this project cold.

## What this project is
The SenseCAP Watcher (ESP32-S3 + Himax, round 412×412 touch LCD, knob, speaker,
WS2812 LED, PCF8563 RTC) is being turned into a **complete original-Tamagotchi**
(P1 1996 rules) called WatchaGotchi. **The full approved plan is committed in
this repo: `docs/PLAN.md`** (design, P1 rule table, hardware mapping,
architecture, phases, verification). Read it first; this file adds the current
status and the hard-won operational gotchas.

## Decisions (locked with Gabriel)
1. Native pet (NOT TamaLIB ROM emulation), full P1 rule set, real death.
2. Tamagotchi-only firmware — the old WatcherOS apps (Radar/Timer/WiFi/selfie
   spec) are deleted from the tree; history: `git log --before=2026-08-16`.
   The selfie-app spec was superseded before implementation.
3. Retro pixel-art, original characters (no Bandai sprite likeness).
4. V1 extras: LED moods + sound personality only. No camera/mic/touch-petting.
   Knob-only navigation; touch just wakes the screen.
5. UI quality bar: commercially viable — no clipped text, no overlaps, nothing
   irrelevant on screen (Gabriel asked for this explicitly).

## Phase status (updated 2026-08-16, end of session — READ THIS FIRST)
- **Phase 0 DONE** (commit `d0e1e66` + follow-ups): skeleton firmware flashed
  and verified on-device. Full chip erase was performed first (old NVS gone).
- **Phase 1 DONE**: `tools/spritegen.py` + `sim/` pipeline proven end-to-end.
  Full roster authored (28 sprites: 9 characters, 8 ring icons, 11 props),
  validated, style-judged 28/28 — verdict + watch-list in `docs/ART-REVIEW.md`
  (per Gabriel: watch-list intentionally NOT acted on; next session's call).
  Contact sheet: `docs/sprite_sheet.png`. After any art change:
  `python3 tools/spritegen.py`, rebuild sim AND firmware.
- **Phase 2 PARTIALLY DONE — resume exactly here:**
  - `main/tama_logic.h` — the binding API contract (written, reviewed).
  - `main/tama_logic.c` — full implementation, compiles clean
    (`cc -c -std=c99 -Wall -Wextra main/tama_logic.c -I main`).
  - `sim/sim_life.c` + `sim/build_life.sh` — care-bot test harness
    (perfect/neglect/snack-spam/no-discipline bots + invariant assertions),
    syntax-checks against the header.
  - **NEVER RUN: the integrate step.** A workflow (impl + tests in parallel,
    then a fix-until-green integrator) was stopped by Gabriel right before the
    integrator started — impl and tests were written by SEPARATE agents and
    have never been linked or executed together. **Next agent's first command:**
    `cd sim && bash build_life.sh && ./sim_life` — then fix link errors and
    test failures until all PASS (judge impl-vs-test disagreements against the
    rules in docs/PLAN.md; prefer fixing whichever violates the plan; tune
    stage_params toward P1 feel rather than weakening tests).
  - Also still missing from Phase 2: clock module + NVS save/load in
    `tama_main.c` (tama_port_save_request is a stub), wiring tama_logic into
    tama_ui (the UI still runs a Phase 1 demo: button toggles egg/baby),
    feed/status/clock-set/toilet screens.
- **Phase 3**: sleep/lights UI, attention/discipline, sickness, higher-lower
  game, evolution/death cinematics wiring, death→new egg. `TAMA_TIME_SCALE`
  debug flag for live demos.
- **Phase 4**: jingle set, LED moods wired to game events, secret character,
  battery status page, final fresh-user walkthrough.

## Non-obvious technical facts (violate these and the device hangs/bricks UX)
- LVGL draw buffer MUST be internal DMA RAM (`.buff_spiram=false`); PSRAM draw
  buffer = QSPI flush stall ≈ 28 s in. PSRAM is fine for sprite/canvas sources.
- All UI on the LVGL task via the 80 ms `lv_timer`; input callbacks only set
  volatile flags; knob burst-gate (3 events / 450 ms) rejects phantom detents.
- Audio: `bsp_codec_dev_stop()` when idle, `resume()` only around playback.
  The stop also closes the never-enabled mic channel → i2s_common error spam,
  silenced via `esp_log_level_set("i2s_common", ESP_LOG_NONE)`.
- `bsp_system_is_charging()` has inverted polarity — use
  `bsp_exp_io_get_level(BSP_PWR_VBUS_IN_DET) == 0` (factory-firmware pattern).
- `bsp_battery_get_percent()` = 10 ADC reads + log lines; poll ≤ 1/30 s.
- Colors in portable code go through `lv_color_hex()`/`lv_color_make()` ONLY —
  device is `LV_COLOR_16_SWAP=1`, sim is 0.
- Chroma key (icon transparency) is pure green 0x00ff00 on both targets.
- Time model: pet-epoch seconds injected by the caller; all scheduled events
  are absolute epochs; power-off = hibernation (no catch-up). Clock changes go
  through `tama_clock_rebase()` (shifts every stored epoch by the delta).
- PCF8563 RTC works (`bsp_rtc_get_time`) and has its own backup cell — a
  post-v1 option for real away-time; v1 clock = NVS epoch + esp_timer.

## Build/flash environment (this Mac)
- ESP-IDF v5.2.1 at `~/esp/esp-idf`; BSP at `~/esp/SenseCAP-Watcher-Firmware`
  (referenced by `main/idf_component.yml` override_path).
- `export CMAKE_POLICY_VERSION_MINIMUM=3.5` before `idf.py build` (cmake 4.x
  vs the BSP repo's old rlottie CMakeLists).
- Flash port `/dev/cu.wchusbserial56D50556323` @ 460800 — REQUIRES the WCH
  CH34x VCP driver; Apple's built-in driver corrupts every bulk write
  (checksum/timeout errors). The `…321` port is the Himax, not the ESP32.
- From Phase 2 on: flash WITHOUT `erase-flash` (pet save lives in NVS,
  namespace `tama`). Factory reset gesture (to implement): hold knob at boot.

## Hardware quirks / battery
- Battery diagnostic (Phase 0 screen + serial `TAMA: DIAG` lines every 30 s)
  showed `present=0`, rail at 4182 mV = charger float ⇒ **no battery detected**
  (dead cell, latched protection, or unseated connector). Replacement part:
  3.7 V 400 mAh LiPo **403035**, 3-wire with 10 kΩ NTC + protection PCB,
  **JST ZH 1.5 mm 3-pin** (schematic J9 `ZH-3A-WT`). Verify wire polarity
  against the old pack. Sources: OSHW schematic PDF + FCC photos (Z4T-WATCHER).
- No fuel gauge on this board — cycle counts are unrecoverable by design.
- No accelerometer / ambient-light sensor. BLE stack not compiled in.

## Exact state at session end (2026-08-16, verified vs assumed)
- **VERIFIED: firmware builds clean** after regenerating sprites
  (`watcher_os.bin` 0xa2080 bytes; the generated-header comment bug — a `/*`
  inside the header comment tripping `-Werror=comment` — was fixed in
  `tools/spritegen.py` and `main/tama_sprites.[ch]` were regenerated).
- **VERIFIED: `tama_logic.c` compiles** (`cc -c -std=c99 -Wall -Wextra`) and
  `sim_life.c` syntax-checks. **NOT verified: they have never been linked or
  run together** (the integrate step was cancelled — see Phase status above).
- **NOT current: `docs/tama_egg.png` / `docs/tama_baby.png`** — rendered
  before the ring switched from text tiles to sprite icons. The final render
  was aborted on purpose. To refresh: `cd sim && ./build.sh && ./sim_screens
  && /usr/bin/python3 render.py`.
- **Gotcha:** after sourcing ESP-IDF's `export.sh`, `python3` is the IDF venv
  (no PIL) — run `render.py`/`spritegen.py` with `/usr/bin/python3` or in a
  shell without the IDF env.
- **NOT verified on-device:** the sprite-icon ring build has not been flashed;
  the device still runs the Phase 0 text-tile build. Flash the current build
  first thing (`idf.py -p /dev/cu.wchusbserial56D50556323 -b 460800 flash`).
- Two orchestration workflows ran this session: sprite roster (completed;
  verdicts in `docs/ART-REVIEW.md`) and logic TDD (impl + tests completed,
  integrator cancelled before starting — Gabriel chose to stop rather than
  wait; nothing was actually hung, deliverables landed on disk).

## Memory files (Claude-specific)
`~/.claude/projects/-Users-gabrielbeaudoin-Development-watcheros/memory/` holds
flash-setup, project-status, and "never log this repo to awork" notes.
