# Selfie Camera App — Design

**Date:** 2026-07-16
**Target:** SenseCAP Watcher (ESP32-S3, round 412×412 touch LCD, Himax AI camera), WatcherOS app framework.

## Goal
Add a "Selfie" app to WatcherOS: a live camera preview with a self-timer, that
captures a photo and uploads it over WiFi to a small local server, which saves it
and serves a browsable gallery. Deliver both the on-device app and a runnable
receiving server so the flow can be tried end-to-end.

## Decisions (locked)
- **Photo destination:** upload over WiFi to a local server (also built here).
- **Capture flow:** knob selects self-timer delay `0 / 3 / 5 / 10 s` (0 = instant);
  button starts. Delay > 0 shows a per-second countdown with a beep + LED flash.
- **Preview:** mirrored horizontally (natural to pose with, like a phone selfie cam).
- **Saved file:** raw camera JPEG bytes, **not** mirrored (keeps the server
  dependency-free). The gallery shows photos as saved. (This supersedes an earlier
  "mirror both" idea; the raw-save choice was made last and wins.)
- **Server:** Python 3 **standard library only** (zero dependencies), so it is
  trivial to spin up.

## Architecture

### Why reuse the existing camera pipeline
The WiFi QR feature already solves the hard parts of driving the Himax camera:
lazy `sscma` client init on its own task (the blocking init must never run on the
LVGL task), frames delivered as base64 JPEG on the sscma callback thread, and a
JPEG→RGB565 decode feeding a live LVGL preview. The selfie app branches this same
pipeline: instead of grayscale→quirc, it mirrors the RGB565 for preview and keeps
the raw JPEG bytes for upload.

**Single camera owner.** Only one `sscma` client may drive the Himax at a time.
QR and Selfie must not each call `bsp_sscma_client_init()`. The camera lifecycle
is factored into a shared module; the currently-active tile is the only one
streaming.

### Component 1 — Shared camera module (refactor of existing QR code in `main/watcher_os.c`)
Extract from the QR code:
- `cam_init()` — one-time `bsp_sscma_client_init()` + `sscma_client_init` +
  model/sensor config, then `sscma_client_break()` (idle). Runs once, on a task.
- `cam_start()` / `cam_stop()` — `sscma_client_invoke(-1,false,true)` / `sscma_client_break()`.
- A single `on_event` callback that base64-decodes the frame, exposing to consumers:
  - the **raw JPEG** (`uint8_t* + len`) — for upload,
  - the **decoded RGB565** buffer (416×416) — for preview,
  - a per-consumer hook so QR runs `qr_scan()` and Selfie latches a frame.

Minimal-change strategy: keep QR working exactly as today. Introduce a small
"active camera consumer" indirection (an enum or a function pointer set on
`on_show`) so the shared `on_event` dispatches to QR-scan vs selfie-latch. Buffers
(RGB565 + JPEG scratch, PSRAM) are shared/allocated once.

### Component 2 — Selfie app (`app_t` registered in `APPS[]`)
State machine: `IDLE → COUNTDOWN → CAPTURING → UPLOADING → (DONE|FAILED) → IDLE`.

- **build(tile):** full-screen `lv_img` preview; a small self-timer badge (top);
  a large centered countdown label (hidden unless counting); a status label
  (bottom) for `Uploading… / Saved ✓ / Failed`.
- **on_show:** `cam_start()`, set the active consumer to selfie, reset to IDLE.
- **on tile leave / long-press home:** `cam_stop()`.
- **preview (per frame, in `on_event`→flag→`tick`):** mirror the latest RGB565
  buffer horizontally into the preview image, `lv_img_set_src` + invalidate.
  Mirroring is a per-row reverse of 16-bit pixels; cheap enough at frame rate.
- **on_knob(dir):** cycle delay index over `{0,3,5,10}`; update the badge.
- **on_button:**
  - delay == 0 → go straight to CAPTURING.
  - delay > 0 → COUNTDOWN: `tick` decrements a 1 Hz counter, shows the number,
    queues `SND_TICK`/LED flash to `fx_task` each second; at 0 → CAPTURING.
- **CAPTURING:** latch the **next** raw JPEG frame (copy bytes + len out of the
  shared buffer under the frame flag), then → UPLOADING. Queue a shutter beep.
- **UPLOADING:** signal a dedicated upload task (see below). UI shows status.
- Guard against re-entrancy: ignore button while not IDLE.

All input callbacks only set flags (existing WatcherOS convention); the `tick`
(in the LVGL task) applies state changes and touches the UI.

### Component 3 — Upload task (own FreeRTOS task, never on the LVGL path)
- Waits on a signal/queue carrying the latched JPEG pointer + length.
- **New binary-POST helper** (the existing `http_req()` cannot be reused: it does
  `strlen(post_body)` so null bytes in a JPEG truncate it, hardcodes
  `Content-Type: application/x-www-form-urlencoded`, and has only a 2560-byte TX
  buffer). Add `http_post_binary(url, const uint8_t *data, int len)` that:
  takes an explicit length, sets `Content-Type: image/jpeg`, opens with
  `esp_http_client_open(h, len)`, and streams the body in chunks via
  `esp_http_client_write` (JPEG is ≤ 48 KB — the QR JPEG budget). No TLS needed
  for a plain-HTTP LAN server (skip `crt_bundle_attach` when the URL is `http://`).
- Sets a result flag (`OK`/`FAIL` + HTTP status) the `tick` reads to update status.
- Runs off the LVGL task so a slow/absent server never stalls rendering.

### Component 4 — Config
Add to gitignored `main/secrets.h`:
```c
#define SELFIE_UPLOAD_URL "http://<your-mac-lan-ip>:8080/upload"
```
Document the placeholder in the README / server README; the user fills in their
Mac's LAN IP.

### Component 5 — Receiving server (`server/`, Python 3 stdlib)
- `server/selfie_server.py` using `http.server`:
  - `POST /upload` — read `Content-Length` bytes, write them verbatim to
    `server/uploads/selfie-YYYYMMDD-HHMMSS.jpg`, respond `200 OK`. (Timestamp from
    the server clock; the device sends no metadata.)
  - `GET /` — HTML gallery: all `uploads/*.jpg` newest-first, `<meta refresh>` a
    few seconds so new selfies appear automatically.
  - `GET /uploads/<file>` — serve the raw JPEG.
- Binds `0.0.0.0:8080` so the device on the LAN can reach it.
- `server/README.md` — run instructions (`python3 selfie_server.py`), how to find
  the Mac's LAN IP, and the `SELFIE_UPLOAD_URL` value to set on the device.
- No dependencies; no venv needed.

## Data flow
```
Himax → sscma (base64 JPEG) → cam on_event
  ├─ decode → RGB565 → [mirror] → LVGL preview
  └─ on capture: latch raw JPEG → upload task → POST → server saves → gallery
```

## Error handling
- Camera init fails → status label "Camera error"; app stays usable (no crash).
- No WiFi / server unreachable / non-200 → status "Upload failed"; return to IDLE
  so the user can retry. Latched JPEG buffer is freed/reused.
- Server: malformed/oversized POST → `400`; missing `uploads/` dir → created on start.
- Never hold the LVGL mutex across network or camera-blocking calls (WatcherOS's
  core stability rule).

## Testing / verification
- **Server unit-ish check:** `curl --data-binary @sample.jpg -H 'Content-Type: image/jpeg'
  http://localhost:8080/upload` → file appears in `uploads/`, shows in gallery.
- **Simulator:** the selfie screen's static layout (preview placeholder, badge,
  countdown, status) can be added to `sim/sim_main.c` for a layout screenshot,
  matching the existing README-shot workflow. (Camera frames are device-only.)
- **On-device:** flash, open Selfie tile, verify live mirrored preview, knob cycles
  delay, countdown beeps/flashes, capture uploads, photo lands in the gallery.

## Out of scope (YAGNI)
- On-device gallery/review of past shots.
- On-device or server-side mirroring of the saved file.
- Authentication / TLS to the local server.
- SD-card or SPIFFS persistence on the device.
- Multiple resolutions / filters / face detection.

## Files touched
- `main/watcher_os.c` — refactor camera lifecycle into a shared consumer model;
  add the Selfie `app_t` + upload task; register in `APPS[]`.
- `main/secrets.h` (gitignored) — `SELFIE_UPLOAD_URL`.
- `server/selfie_server.py`, `server/README.md`, `server/uploads/.gitkeep` — new.
- `README.md` — add Selfie to the Apps list (+ optional simulator shot).
