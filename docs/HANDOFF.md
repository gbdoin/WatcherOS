# WatchaGotchi — agent handoff

Written 2026-08-16, updated 2026-08-16 (second session, remote container) so
any agent (or human) can resume this project cold.

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

## Phase status (updated 2026-08-16, session 2 — READ THIS FIRST)
- **Phase 0 DONE** (commit `d0e1e66` + follow-ups): skeleton firmware flashed
  and verified on-device. Full chip erase was performed first (old NVS gone).
- **Phase 1 DONE**: `tools/spritegen.py` + `sim/` pipeline proven end-to-end.
  Full roster authored (28 sprites), style-judged 28/28 — verdict + watch-list
  in `docs/ART-REVIEW.md`. Session 2 reviewed the watch-list and left it as-is
  (all three items were judged acceptable and look fine in the new renders).
  **Frame semantics matter:** 4-frame pets are idle/idle/EAT/SLEEP — the UI
  alternates 0/1 for idle, shows 2 while eating, 3 while asleep.
  After any art change: `python3 tools/spritegen.py`, rebuild sim AND firmware.
- **Phase 2 DONE (code + sim-verified)**: the stalled integrate step was run
  first — `sim_life` linked on the first try, 5 failures judged against
  docs/PLAN.md and fixed (see commit `4e9ac2b`: care-scaled old age, neglect
  vs discipline mistake split, adult-only random sickness, rebase anchor
  guard). Now 12/12 PASS. Clock module + NVS save/load + factory reset live
  in `tama_main.c`; the full game UI is wired in `tama_ui.c`.
- **Phase 3 DONE (code + sim-verified)**: sleep/lights, attention/discipline,
  sickness/medicine, higher-lower game, evolution branches + cinematic,
  death → clock-set → new egg (P1-style), `TAMA_TIME_SCALE` demo flag
  (`TAMA_TIME_SCALE=60 idf.py build` via root CMakeLists env hook).
- **Phase 4 CODE-COMPLETE**: 14-cue jingle table in `tama_sfx.c`, LED moods
  driven from state each tick, secret character path, battery status page.
  **Remaining Phase 4 work is on-device only** — see the checklist below.
- **Session 2 could not touch the device** (remote container; no ESP-IDF —
  the toolchain host dl.espressif.com is blocked by network policy). All
  portable code is verified through the sim; `tama_main.c`/`tama_sfx.c` were
  syntax-checked against stub headers mirroring the real BSP signatures
  (real header cross-checked from a clone of Seeed-Studio/
  sensecap-watcher-firmware) but have NEVER been compiled by ESP-IDF.

## Device checklist for the next Mac session (in order)
1. `idf.py build` — first real compile of the new `tama_main.c` / `tama_ui.c`
   / `tama_sfx.c`. Expect at most small fixes (stub-checked, not IDF-checked).
2. Flash WITHOUT erase (`idf.py -p /dev/cu.wchusbserial56D50556323 -b 460800
   flash`). First boot should show SET CLOCK; confirm → egg; 5 min → hatch.
3. Walk every flow: feed (meal refuse at 4 hearts), game, status pages,
   clock re-set (±hours; pet must not sleep/wake spuriously), scold during a
   misbehave call, WC, meds while sick.
4. Reboot mid-life → pet resumes; power-pull → ≤1 pet-minute lost.
5. Factory reset gesture: hold knob while plugging in (watch for the
   `factory reset` log banner). Verify bsp_knob_btn_init that early in boot
   actually reads the pressed level — this is the least-certain device code.
6. Idle dim: screen off after 60 s, touch wakes it back to the UI-chosen
   brightness (5% while asleep+lights-off, not 100%).
7. Audio pass: TSFX_CALL (2093 Hz @ amp 9000) may be harsh on the small
   speaker — tune amplitudes by ear.
8. Demo build `TAMA_TIME_SCALE=60 idf.py build` for a fast lifecycle demo,
   then a real-time overnight soak (watchdog, heap, save cadence).
9. `docs/PLAN.md` pixel-parity check: sim PNGs vs device (LV_COLOR_16_SWAP).

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

## Exact state at end of session 2 (2026-08-16, verified vs assumed)
- **VERIFIED (host): `sim_life` 12/12 PASS** — full P1 invariant suite:
  perfect care → HERO → SECRET at day 14, neglect dies day 0, snack-spam
  → fat FERAL adult, no-discipline survives, refuse-when-full, care window,
  serialize round-trip, ±6h clock rebase, 24h large-jump determinism.
- **VERIFIED (host): `sim_screens` SCENES: PASS** — drives the real
  `tama_ui.c` through one continuous life (first-boot clock-set → egg →
  hatch → feed → game → sickness → sleep → evolutions → death) and dumps
  15 scenes → `docs/shots/*.png` (also embedded in the README). Every PNG
  visually reviewed against the commercial-quality bar.
- **VERIFIED (host, stub-only): `tama_main.c` + `tama_sfx.c` compile** under
  gcc with stub headers mirroring the real BSP/IDF signatures. **NOT
  verified: a real `idf.py build`** — this container cannot install ESP-IDF
  (dl.espressif.com blocked). The device also still runs the Phase 0 build;
  nothing from sessions 1-2 beyond Phase 0 has ever been flashed.
- **Save format note:** NVS namespace `tama`, keys `blob` (76-byte
  tama_state_t, versioned/validated by tama_deserialize) and `epoch` (u32
  pet-epoch snapshot, written with every blob save + every 60 s). First-boot
  epoch base = 30 days (pad so backwards clock-sets can't underflow).
- **Sim environment (cloud containers):** LVGL 8.4.0 auto-found at
  /workspace/lvgl/lvgl (sim/build.sh falls back: $LVGL env → Mac BSP path →
  /workspace clone). Pillow needed for render.py (`pip3 install pillow`).
- **Gotcha (Mac):** after sourcing ESP-IDF's `export.sh`, `python3` is the
  IDF venv (no PIL) — run `render.py`/`spritegen.py` with `/usr/bin/python3`.
- Three orchestration workflows ran in session 2: implementation fan-out
  (UI rewrite / device main / sfx jingles / sim scenes — 4 agents, all
  landed), and an adversarial verification pass (4 review dimensions, each
  finding independently refute-tested); confirmed findings were fixed before
  the final push. Session 1's workflows: sprite roster + logic TDD.

## Memory files (Claude-specific)
`~/.claude/projects/-Users-gabrielbeaudoin-Development-watcheros/memory/` holds
flash-setup, project-status, and "never log this repo to awork" notes.
