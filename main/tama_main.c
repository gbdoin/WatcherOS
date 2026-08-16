/*
 * tama_main — WatchaGotchi firmware entry for the SenseCAP Watcher.
 *
 * Framework invariants preserved from WatcherOS (hard-won stability rules):
 *  - LVGL draw buffer lives in INTERNAL DMA RAM, never PSRAM (a PSRAM DMA
 *    buffer feeding the QSPI LCD stalls the flush and hangs the UI).
 *  - The LVGL task owns ALL UI. Periodic work runs in an lv_timer inside it.
 *  - Input callbacks only set flags; the lv_timer applies them. The knob is
 *    burst-gated to reject electrical phantom counts.
 *  - Audio + LED live in the fx task (tama_sfx.c), fed by a queue.
 *  - Task watchdog stays enabled.
 *
 * This file also owns the pet clock (NVS epoch snapshot + esp_timer uptime —
 * frozen across power-off, so pending game events need no catch-up) and the
 * low-priority NVS saver. Factory reset: hold the knob while plugging in.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "sensecap-watcher.h"
#include "esp_lvgl_port.h"
#include "iot_knob.h"
#include "iot_button.h"

#include "tama_port.h"
#include "tama_sfx.h"
#include "tama_ui.h"

static const char *TAG = "TAMA";

/* ---------------- input: flags only, applied in ui_tick ----------------
 * Every callback<->ui_tick channel is a monotonic counter with a single
 * writer (the callback) drained by a reader-local "seen" counter: the knob
 * callbacks run on a different task/core than the LVGL tick, and a shared
 * read-modify-write (the old `knob_steps += dir` / `-=`) can lose detents. */
static volatile uint32_t knob_fwd_cnt = 0;
static volatile uint32_t knob_back_cnt = 0;
static int               burst_n = 0;
static int64_t           knob_last_ev = 0;
#define KNOB_BURST_US    450000
#define KNOB_CONFIRM     3
static volatile uint32_t click_cnt = 0;
static volatile uint32_t long_cnt = 0;
static volatile int64_t  g_last_interact_us = 0;
#define IDLE_DIM_MS      60000

static void knob_ev(int dir)
{
    int64_t now = esp_timer_get_time();
    g_last_interact_us = now;
    if (now - knob_last_ev <= KNOB_BURST_US) burst_n++;
    else burst_n = 1;
    knob_last_ev = now;
    if (burst_n >= KNOB_CONFIRM) {
        if (dir > 0) knob_fwd_cnt++;
        else         knob_back_cnt++;
    }
}
static void knob_left_cb(void *a, void *d)  { knob_ev(-1); }
static void knob_right_cb(void *a, void *d) { knob_ev(+1); }
static void btn_click_cb(void *a, void *d)  { click_cnt++; g_last_interact_us = esp_timer_get_time(); }
static void btn_long_cb(void *a, void *d)   { long_cnt++;  g_last_interact_us = esp_timer_get_time(); }

static void input_init(void)
{
    knob_config_t kcfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_KNOB_A,
        .gpio_encoder_b = BSP_KNOB_B,
    };
    knob_handle_t knob = iot_knob_create(&kcfg);
    assert(knob);
    iot_knob_register_cb(knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(knob, KNOB_RIGHT, knob_right_cb, NULL);
    button_config_t bcfg = {
        .type = BUTTON_TYPE_CUSTOM,
        .long_press_time = 1000,
        .short_press_time = 200,
        .custom_button_config = {
            .active_level = 0,
            .button_custom_init = bsp_knob_btn_init,
            .button_custom_deinit = bsp_knob_btn_deinit,
            .button_custom_get_key_value = bsp_knob_btn_get_key_value,
        },
    };
    button_handle_t btn = iot_button_create(&bcfg);
    assert(btn);
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, btn_click_cb, NULL);
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, btn_long_cb, NULL);
}

/* ---------------- persistence (NVS "tama": "blob" + "epoch") ------------ */
#define TAMA_NVS_NS    "tama"
#define TAMA_KEY_BLOB  "blob"
#define TAMA_KEY_EPOCH "epoch"

static nvs_handle_t s_nvs = 0;         /* 0 = open failed; saves are skipped */
static TaskHandle_t s_saver = NULL;
static portMUX_TYPE s_save_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_save_buf[TAMA_BLOB_SIZE];   /* guarded by s_save_mux */
static size_t       s_save_len = 0;
static bool         s_blob_pending = false;

/* ---------------- pet clock ----------------
 * pet-epoch = NVS snapshot + scaled uptime; frozen while powered off.
 * TAMA_TIME_SCALE=60 demo builds run 1 real minute = 1 pet-hour. First boot
 * starts 30 days in: the pad keeps backwards clock-sets from ever
 * underflowing pet-epoch, and the absolute day index is invisible to the
 * logic (only hour-of-day and stored anchors matter). */
#ifndef TAMA_TIME_SCALE
#define TAMA_TIME_SCALE 1
#endif

/* base_epoch is SIGNED 64-bit and unclamped: the UI has already applied its
 * full rebase delta to every pet anchor before calling clock_shift, so a
 * clamp here would silently desynchronize the platform clock from the pet.
 * pet-now itself (base + scaled uptime) is clamped at the read. */
static int64_t boot_us = 0;
/* 64-bit loads/stores are not atomic on the S3: clock_shift (LVGL task) races
 * the saver task's tama_port_now() read, so base_epoch is guarded. */
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t base_epoch = 30 * 86400;

uint32_t tama_port_now(void)
{
    taskENTER_CRITICAL(&s_clock_mux);
    int64_t base = base_epoch;
    taskEXIT_CRITICAL(&s_clock_mux);
    int64_t e = base
              + ((esp_timer_get_time() - boot_us) / 1000000) * TAMA_TIME_SCALE;
    if (e < 0) e = 0;
    if (e > (int64_t)UINT32_MAX) e = (int64_t)UINT32_MAX;
    return (uint32_t)e;
}

void tama_port_clock_shift(int32_t delta_s)
{
    taskENTER_CRITICAL(&s_clock_mux);
    base_epoch += delta_s;
    taskEXIT_CRITICAL(&s_clock_mux);
    /* persist promptly: an epoch-only save unless a blob is already queued */
    if (s_saver) xTaskNotifyGive(s_saver);
}

uint32_t tama_port_random(void) { return esp_random(); }

/* ---------------- port implementation ---------------- */
void tama_port_sfx(int cue)        { tama_sfx_queue(cue); }
void tama_port_led_mood(int mood)  { tama_sfx_led_mood(mood); }
void *tama_port_big_alloc(size_t size)
{
    return heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
}

/* The UI dims to ~5 % during sleep+lights-off; waking the screen must not
 * blast 100 % into a dark bedroom. Record the UI's wish always; while
 * idle-dimmed only record — the ui_tick wake path applies it. (All callers
 * are on the LVGL task, same as ui_tick — no locking needed.) */
static bool g_screen_dim = false;
static int  ui_brightness = 100;

void tama_port_brightness(int pct)
{
    ui_brightness = pct;
    if (!g_screen_dim) bsp_lcd_brightness_set(pct);
}

/* LVGL task context: snapshot the live pet under a spinlock (a 76-byte copy)
 * and wake the saver — flash writes never run on the UI task. */
void tama_port_save_request(void)
{
    taskENTER_CRITICAL(&s_save_mux);
    if (tama_serialize(tama_ui_state(), s_save_buf, sizeof(s_save_buf), &s_save_len))
        s_blob_pending = true;
    taskEXIT_CRITICAL(&s_save_mux);
    if (s_saver) xTaskNotifyGive(s_saver);
}

/* Notify -> 1 s coalesce -> blob+epoch commit. 60 s timeout -> epoch-only
 * commit, so a power pull loses <=1 pet-minute. The pending flag is checked
 * on timeouts too, so a snapshot taken before this task existed still lands.
 * Save errors are logged, never fatal — the pet plays on from RAM. */
static void saver_task(void *arg)
{
    while (1) {
        bool notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000)) > 0;
        if (notified) {
            vTaskDelay(pdMS_TO_TICKS(1000));      /* coalesce event bursts */
            ulTaskNotifyTake(pdTRUE, 0);          /* drain gives that raced the delay */
        }
        uint8_t blob[TAMA_BLOB_SIZE];
        size_t len = 0;
        taskENTER_CRITICAL(&s_save_mux);
        if (s_blob_pending) {
            len = s_save_len;
            memcpy(blob, s_save_buf, len);
            s_blob_pending = false;
        }
        taskEXIT_CRITICAL(&s_save_mux);
        if (!s_nvs) continue;
        esp_err_t eb = len ? nvs_set_blob(s_nvs, TAMA_KEY_BLOB, blob, len) : ESP_OK;
        esp_err_t ee = nvs_set_u32(s_nvs, TAMA_KEY_EPOCH, tama_port_now());
        esp_err_t ec = nvs_commit(s_nvs);
        if (eb != ESP_OK || ee != ESP_OK || ec != ESP_OK)
            ESP_LOGW(TAG, "save failed: blob=%s epoch=%s commit=%s",
                     esp_err_to_name(eb), esp_err_to_name(ee), esp_err_to_name(ec));
    }
}

/* Factory reset gesture: knob held while power is applied. Key value is
 * active-low; re-check ~50 ms later to debounce. NEVER call
 * bsp_knob_btn_deinit here: in the BSP it deletes the SHARED PCA9535 io
 * expander handle (which touch, battery/VBUS diag and the knob button all
 * use) and leaves the freed pointer cached — instant use-after-free for
 * the rest of the boot. bsp_knob_btn_init is idempotent (the expander
 * handle is cached), so input_init's iot_button_create re-inits fine. */
static void factory_reset_check(void)
{
    bsp_knob_btn_init(NULL);
    if (bsp_knob_btn_get_key_value(NULL) == 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (bsp_knob_btn_get_key_value(NULL) == 0 && s_nvs) {
            ESP_LOGW(TAG, "==== FACTORY RESET: knob held at boot, erasing pet ====");
            esp_err_t e1 = nvs_erase_all(s_nvs);
            esp_err_t e2 = nvs_commit(s_nvs);
            if (e1 != ESP_OK || e2 != ESP_OK)
                ESP_LOGW(TAG, "factory reset failed: erase=%s commit=%s",
                         esp_err_to_name(e1), esp_err_to_name(e2));
        }
    }
}

/* ---------------- diagnostics task ----------------
 * Battery percent does 10 ADC reads + log lines — poll every 30 s, off the
 * LVGL task (factory-firmware cadence). VBUS via raw pin level: the
 * bsp_system_is_charging() wrapper has inverted polarity and even the
 * factory firmware avoids it. The battery status page consumes g_tama_diag. */
static void diag_task(void *arg)
{
    while (1) {
        g_tama_diag.batt_mv      = bsp_battery_get_voltage();
        g_tama_diag.batt_pct     = bsp_battery_get_percent();
        g_tama_diag.batt_present = bsp_battery_is_present();
        g_tama_diag.vbus_in      = (bsp_exp_io_get_level(BSP_PWR_VBUS_IN_DET) == 0);
        g_tama_diag.chrg_pin     = bsp_exp_io_get_level(BSP_PWR_CHRG_DET);
        g_tama_diag.stdby_pin    = bsp_exp_io_get_level(BSP_PWR_STDBY_DET);
        struct tm t;
        if (bsp_rtc_get_time(&t) == ESP_OK) {
            g_tama_diag.rtc_ok = 1;
            g_tama_diag.rtc_h = t.tm_hour; g_tama_diag.rtc_m = t.tm_min; g_tama_diag.rtc_s = t.tm_sec;
        } else {
            g_tama_diag.rtc_ok = 0;
        }
        g_tama_diag.updated_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "DIAG bat=%dmV %d%% present=%d vbus_in=%d chrg=%d stdby=%d rtc=%s %02d:%02d:%02d",
                 g_tama_diag.batt_mv, g_tama_diag.batt_pct, g_tama_diag.batt_present,
                 g_tama_diag.vbus_in, g_tama_diag.chrg_pin, g_tama_diag.stdby_pin,
                 g_tama_diag.rtc_ok ? "ok" : "ERR",
                 g_tama_diag.rtc_h, g_tama_diag.rtc_m, g_tama_diag.rtc_s);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ---------------- ui tick (LVGL task) ---------------- */
static void ui_tick(lv_timer_t *tmr)
{
    /* apply queued input */
    static uint32_t fwd_seen = 0, back_seen = 0;
    uint32_t f = knob_fwd_cnt, b = knob_back_cnt;
    int steps = (int)(f - fwd_seen) - (int)(b - back_seen);
    fwd_seen = f;
    back_seen = b;
    if (steps) {
        int n = steps > 0 ? steps : -steps;
        if (n > 8) n = 8;
        for (int i = 0; i < n; i++) tama_ui_on_knob(steps > 0 ? +1 : -1);
    }
    static uint32_t clicks_seen = 0, longs_seen = 0;
    if (click_cnt != clicks_seen) { clicks_seen = click_cnt; tama_ui_on_button(); }
    if (long_cnt != longs_seen)   { longs_seen = long_cnt;   tama_ui_on_long_press(); }

    /* idle dim / wake (touch resets LVGL inactivity; knob/button set the flag) */
    int64_t now = esp_timer_get_time();
    uint32_t lv_idle = lv_disp_get_inactive_time(NULL);
    bool active = (now - g_last_interact_us) < (int64_t)IDLE_DIM_MS * 1000
                  || lv_idle < IDLE_DIM_MS;
    if (active && g_screen_dim)  { g_screen_dim = false; bsp_lcd_brightness_set(ui_brightness); }
    if (!active && !g_screen_dim) { g_screen_dim = true;  bsp_lcd_brightness_set(0); }

    tama_ui_tick();
}

void app_main(void)
{
    /* bsp_codec_dev_stop() also closes the never-enabled mic channel; the
     * i2s driver logs an error for it on every sound. Cosmetic — silence it. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);

    boot_us = esp_timer_get_time();

    esp_io_expander_handle_t io = bsp_io_expander_init();
    assert(io != NULL);
    bsp_rgb_init();
    bsp_rtc_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    err = nvs_open(TAMA_NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        s_nvs = 0;
        ESP_LOGW(TAG, "nvs_open failed (%s): running without saves", esp_err_to_name(err));
    }

    factory_reset_check();

    uint32_t epoch;
    if (s_nvs && nvs_get_u32(s_nvs, TAMA_KEY_EPOCH, &epoch) == ESP_OK)
        base_epoch = epoch;                       /* else first boot: 30-day pad */
    uint8_t blob[TAMA_BLOB_SIZE];
    size_t blob_len = sizeof(blob);
    bool have_blob = s_nvs && nvs_get_blob(s_nvs, TAMA_KEY_BLOB, blob, &blob_len) == ESP_OK;
    ESP_LOGI(TAG, "boot: pet-epoch=%u blob=%s", (unsigned)base_epoch, have_blob ? "found" : "none");

    /* LVGL: internal DMA draw buffer (the critical fix). WiFi is gone, so a
     * larger double buffer is affordable for smoother animation. */
    bsp_display_cfg_t dcfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 412 * 40,
        .double_buffer = true,
        .flags = { .buff_dma = true, .buff_spiram = false },
    };
    dcfg.lvgl_port_cfg.task_affinity = 1;
    lv_disp_t *disp = bsp_lvgl_init_with_cfg(&dcfg);
    assert(disp != NULL);
    bsp_lcd_brightness_set(ui_brightness);        /* straight to the game, 100 % */

    tama_sfx_start();

    if (lvgl_port_lock(0)) {
        tama_ui_build();
        tama_ui_start(have_blob ? blob : NULL, have_blob ? blob_len : 0);
        lv_timer_create(ui_tick, 80, NULL);
        lvgl_port_unlock();
    }

    input_init();
    xTaskCreate(saver_task, "tama_save", 4096, NULL, 3, &s_saver);
    xTaskCreate(diag_task, "diag", 4096, NULL, 3, NULL);

    g_last_interact_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WatchaGotchi running. free_internal=%d free_psram=%d",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
