# WatchaGotchi — a complete Tamagotchi for the SenseCAP Watcher

## Context
Gabriel wants to erase what's on the SenseCAP Watcher (WatcherOS's Radar/Timer/WiFi apps) and replace it with a **dedicated Tamagotchi firmware** — the device *becomes* the pet. Requirements gathered and locked with the user:

1. **Native pet, full hardware** — our own implementation of the complete original-Tamagotchi (P1, 1996) rule set on the round color touchscreen. NOT TamaLIB ROM emulation (needs a copyrighted Bandai ROM, renders 32×16 mono, wastes the hardware).
2. **Tamagotchi-only firmware** — old apps deleted; the proven plumbing (LVGL internal-DMA init, input gating, fx audio/LED task) is reused as scaffolding.
3. **Real stakes** — faithful care mistakes, sickness, death.
4. **Art: retro pixel-art, modernized** — ~11 original characters (no Bandai sprite resemblance), chunky pixels with color accents.
5. **V1 extras: LED + sound personality** only. Camera/mic/touch-petting are out of v1 scope. Navigation is **knob-only** in v1 (touch just wakes the dimmed screen); touch-tap icons can come post-v1.
6. Also in scope: **Phase 0 battery diagnostic** (user suspects the battery is dead — see Battery section).

Note: the earlier selfie-app spec (`docs/superpowers/specs/2026-07-16-selfie-camera-app-design.md`) is **superseded by this pivot** — keep the file, build none of it.

## Prior art (researched)
- **ArduinoGotchi / mcugotchi (TamaLIB)** — authentic P1 emulation on MCUs; proves the ruleset is MCU-friendly; rejected (ROM legality, ignores hardware). Used as mechanics reference.
- **ESP32-TamaPetchi, TamaFi** — native ESP32 virtual pets; validate the native approach and device-unique charm.
- **P1 mechanics sources**: thaao.net P1 care guide, Tamagotchi Wiki (1996 pet), TamaVault Gen1 chart.

## The original-P1 rule set (authenticity spec)
| Mechanic | Behavior (structure locked; timing numbers are tunable defaults) |
|---|---|
| Stats | Hunger ×4 hearts, Happy ×4 hearts, Discipline 0–100%, Weight, Age (1 day = 1 year at wake rollover) |
| Feeding | Meal +1 hunger heart +1 weight, refused when full (head-shake); Snack +1 happy +2 weight, always accepted (the discipline trap) |
| Game | Higher/lower guessing, 5 rounds; win: +1 happy, −1 weight |
| Poop | Random 10–90 min after meals, max 4 on screen; toilet cleans; old/4th poop → sickness |
| Sickness | Skull overlay; medicine 1–2 doses; once per stage + poop-induced; untreated ~6 h → death |
| Sleep | Stage-specific bed/wake hours; lights must go off ≤15 min after sleep else care mistake |
| Discipline | Random misbehavior calls (~2–4×/day): scold ≤15 min → +25%; ignore/feed instead → mistake |
| Care mistakes | Empty heart or misbehavior unaddressed 15 min; drives evolution branching |
| Evolution | Egg (5 min) → Baby (65 min) → Child (~1 day) → Teen (~2–3 days) → Adult (~day 10) → Secret (perfect discipline path) |
| Death | Adults: 5 care mistakes; untreated sickness; old age (~12-day avg lifespan) → death screen → new egg |
| Clock | Settable wall clock (the P1 had one); drives sleep schedule |

## Hardware → feature mapping (verified against BSP source + schematic)
| Watcher hardware (verified API) | Tamagotchi use |
|---|---|
| 412×412 round touch LCD (`bsp_lvgl_init_with_cfg`, `bsp_lcd_brightness_set`) | Pet room, animated sprites, **circular 8-icon ring** at r≈178 px (round-native version of P1's icon rows); brightness dim = lights-off |
| Rotary knob (GPIO41/42, burst-gate pattern) | Rotate icon selection / menus / game choice |
| Knob button (only button; IO-expander, `iot_button` custom) | Press = confirm; long-press = back/cancel (free to repurpose — single-app firmware) |
| Touch (SPD2010, polled) | v1: wake dimmed screen only |
| Speaker ES8311 (`play_tone` synth; `bsp_codec_dev_stop/resume` gating) | Chirps + jingles: attention, confirm/deny, eat, win/lose, evolution fanfare, death chime |
| RGB LED WS2812 (`bsp_rgb_set`, in fx_task) | Attention flash, sick pulse, evolution glow, sleep breathing |
| RTC PCF8563 (`bsp_rtc_get/set_time`, own backup micro-battery per schematic v0.4) | Phase 0 probe; post-v1 option for away-time reconciliation. V1 clock = NVS epoch + esp_timer (below) |
| Battery ADC (`bsp_battery_get_voltage/percent`, poll ≤1/30 s; VBUS via `bsp_exp_io_get_level(BSP_PWR_VBUS_IN_DET)==0`, NOT the inverted-polarity `bsp_system_is_charging`) | Battery status page + Phase 0 diagnostic |
| WiFi/BLE/camera/mic/SD/accelerometer | Not used in v1 (BLE not compiled; no accel exists) |
| NVS 200 K | Save blob + clock epoch (wear math checked: <100 erases/yr — fine) |

**Framework invariants (from watcher_os.c, keep verbatim):** LVGL draw buffer in INTERNAL DMA RAM only (PSRAM draw buffer hangs the QSPI flush ~28 s in); all UI in the LVGL task via one `lv_timer` tick; input callbacks only set volatile flags; audio/LED in fx_task via queue, I2S stopped when idle; task watchdog on. With WiFi gone, bump the draw buffer (412×40, try double-buffer) for smoother animation.

## Architecture

### Files (new `main/`)
| File | Role | Host-compilable |
|---|---|---|
| `tama_main.c` | app_main: BSP/LVGL/NVS init, input, fx spawn, clock module, save orchestration, implements `tama_port.h` | no |
| `tama_logic.c/h` | **Pure C game state machine** — no LVGL/FreeRTOS/ESP includes; caller injects pet-epoch seconds; returns event bitmask | **yes** |
| `tama_ui.c/h` | All LVGL rendering (depends only on lvgl + logic + sprites + port) | **yes** |
| `tama_sprites.c/h` | Generated sprite arrays + table (committed codegen output) | yes |
| `tama_sfx.c/h` | fx_task: tone synth (copied), jingles as note tables, LED mood patterns | no |
| `tama_port.h` | Seam: `tama_port_sfx/led_mood/brightness/save_request` — real on device, stubs in sim | header |

Deleted: all WiFi/Radar/QR code, `isp.c/h`, `secrets.h`, tileview. `idf_component.yml`: drop `espressif/quirc` + `esp_jpeg_simd`, keep the `sensecap-watcher` override (sscma stays linked via BSP manifest but never initialized). `partitions.csv` unchanged.

### Game core design (the key decisions)
- **Absolute-epoch scheduling**: every future event (`next_hunger_drop`, `next_poop`, `attention_deadline`, `evolve_epoch`…) is stored as an absolute pet-epoch second in a packed ~96-byte versioned state struct. `tama_tick(state, now)` fires whatever is due. 
- **Hibernation semantics for free**: pet clock = NVS-persisted epoch (saved 1/min) + esp_timer uptime. Power-off freezes pet time, so on boot all pending timers are simply still pending — zero catch-up code, no unfair deaths from a week unplugged.
- **Clock-set rebasing**: user clock change shifts *all* stored epochs by the delta (`tama_clock_rebase`) — no event storms after ±hours adjustments. (Compile flag can disable rebase for P1-authentic time-cheating.)
- **Persisted xorshift32 RNG** → deterministic replays of any saved state in the sim.
- 1 Hz logic tick driven from the 80 ms UI `lv_timer`; `TEV_*` event bitmask drives UI updates, sfx/LED cues, and save-on-change.
- Evolution branch table as data: Child→Teen on stage mistakes ≤2; Teen→one of 5 adults on (teen type × mistakes × discipline); Adult→Secret on age ≥14 + 0 mistakes + discipline 100. First boot / death-restart → clock-set → new egg. Factory reset = hold knob at boot → erase `"tama"` NVS namespace.

### UI approach
- One LVGL screen; each sub-screen a hidden/shown container (no runtime allocation). Diff-style updates in `ui_tick`.
- **Pet rendering: custom 30-line blitter** — 32×32 1–2 bpp sprites integer-scaled 9× into a 288×288 RGB565 canvas in **PSRAM** (source buffers are CPU-copied by LVGL; the internal-DMA constraint applies only to the draw buffer). No `lv_img_set_zoom` (transform limits + sim divergence). Must use `lv_color_make()` everywhere — device is `LV_COLOR_16_SWAP=1`, sim is 0.
- Animation = `lv_timer` frame flips (2 fps idle; 100 ms during evolution flash), frame index from a counter — identical logic headless in the sim.
- Screens: main room (pet + poops + overlays + icon ring), status pages (hearts/discipline/weight+age/battery), feed, game, clock+clock-set, evolution cinematic, death, sleep overlay (brightness ~5%).
- Retro font: enable LVGL's built-in `UNSCII` bitmap font on device and sim.

### Sprite pipeline
ASCII-art `.txt` frames (`size`/`palette`/`frame` sections) → `tools/spritegen.py` (stdlib Python) → committed `tama_sprites.c/h`. ~11 characters × ~9 frames + props/icons/digits ≈ <60 KB flash total. Preview via sim contact sheet → PNGs.

### Simulator strategy (sim/ reused and upgraded)
The sim compiles the **real** `tama_ui.c` + `tama_logic.c` (not copies — the old sim's hand-copied drawing approach is retired). Three host targets:
1. `sim_screens` — every screen in representative states → round PNGs in docs/.
2. `sim_sprites` — sprite contact sheet.
3. `sim_life` — headless: fast-forward 12 pet-days in <1 s with care-bots (perfect/neglect/snack-spam/no-discipline); asserts every evolution branch, death paths, refuse-when-full, 15-min windows, serialize round-trip, clock-rebase invariants. Also the tuning harness for decay tables.
Fixes needed: `sim/build.sh` hardcodes a Linux LVGL path (→ `/Users/gabrielbeaudoin/esp/SenseCAP-Watcher-Firmware/components/lvgl`) and uses bash-4 `mapfile` (macOS ships bash 3.2) — rewrite portably.

## Battery investigation (user-reported: no longer holds charge)
From the OSHW schematic + FCC internal photos (label read from the photo):
- **Replacement part: 3.7 V 400 mAh LiPo, model 403035** (4.0×30×35 mm), **3-wire with 10 kΩ NTC + protection PCB, JST ZH 1.5 mm 3-pin** (J9 "ZH-3A-WT"). Cell date code 240521 → ~2.3 years old; 403035-class life is ~300–500 cycles / 2–3 years, accelerated by permanent-dock float charging. Verify wire polarity against the old pack before plugging (JST order isn't standardized).
- **No fuel gauge exists** → cycle count unrecoverable; only voltage/percent + charger status pins are readable.
- **Phase 0 diagnostic** (temporary): log + on-screen readout of voltage, percent, battery-present, VBUS, Charger_CHRG/STDBY every 30 s. Interpretation: <~3.0 V never rising while plugged = dead cell/charger fault; "not present" = connector issue; reaches ~4.2 V but dies instantly unplugged = no capacity → replace.

## Implementation phases (each flashable & demoable)

**Phase 0 — Erase & skeleton (small).** Full device erase (`idf.py erase-flash`) per user request, then flash: new file layout, placeholder pet + 8-icon ring, knob selection with ticks, press/long-press, idle dim. Temporary probes: battery diagnostic (above), `bsp_lcd_brightness_set` at 5/20/50%, `bsp_rtc_get_time` across a power pull. Verify: no watchdog resets, big internal-heap gain vs WiFi build, inputs feel right.

**Phase 1 — Sprite pipeline + sim revival (medium; art is the long tail).** spritegen.py, first art batch (egg/baby/child/props/icons), fixed build.sh, `sim_screens`+`sim_sprites` rendering the real tama_ui. Verify: docs/ PNGs match the device pixel-for-pixel (16_SWAP check).

**Phase 2 — Core loop (big).** `tama_logic` core + serialize + clock module + rebase; feed flow, status pages, toilet, clock-set; `sim_life` first assertions. Verify on device: hearts decay in real time, poop/clean, reboot → state intact, power-pull → ≤1 min pet-time loss, clock ±hours → no event storm. From here on, flash WITHOUT erase so the pet survives reflashes.

**Phase 3 — Full lifecycle (big).** Sleep/lights, attention/discipline, sickness/medicine, higher-lower game, evolution branches, death→new egg. `sim_life` bots prove every branch row; tune `stage_params[]`. Device demo via `TAMA_TIME_SCALE=60` debug flag (1 min = 1 pet-hour), then overnight real-time soak.

**Phase 4 — Polish (medium).** Full jingle set + LED moods (single mood variable owned by fx_task), evolution/death cinematics, secret character, sleep brightness, battery page. Final fresh-user walkthrough from erase-flash.

## Verification
- **Host**: `sim_life` asserts the entire rule table deterministically (12 days in <1 s); `sim_screens` PNGs review every UI state without hardware.
- **Device**: per-phase checks above; flash via `/dev/cu.wchusbserial56D50556323` @460800 (WCH driver — known-good since the LED test); serial monitor for heap/watchdog; overnight soak in Phase 3; 24 h battery-diagnostic observation answers the dead-battery question.

## Risks / notes
- Tuning "aliveness" (too needy vs boring) — mitigated by sim bots + time-scale flag.
- PSRAM canvas must never become the draw buffer — the `bsp_display_cfg_t` block stays untouched.
- esp_timer drift ~±2 s/day — acceptable; clock-set fixes; RTC reconciliation is the post-v1 upgrade path.
- Deep sleep out of scope v1 (esp_timer freezes; PCF8563 timer-wake is the future battery play — moot until the battery is replaced anyway).
- Estimated sizes: tama_logic ~1.2 k lines, tama_ui ~1.2 k, sim_life ~400, sprites = iteration-heavy but mechanically small; tama_main ~250.
