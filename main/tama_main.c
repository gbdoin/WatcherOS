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
 * Phase 0 extras: battery/charger/RTC diagnostics task (the user suspects a
 * dead battery), boot brightness probe (5/20/50/100 for sleep-dim planning).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "sensecap-watcher.h"
#include "esp_lvgl_port.h"
#include "iot_knob.h"
#include "iot_button.h"

#include "tama_port.h"
#include "tama_sfx.h"
#include "tama_ui.h"

static const char *TAG = "TAMA";

/* ---------------- input: flags only, applied in ui_tick ---------------- */
static volatile int      knob_steps = 0;
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
    if (burst_n >= KNOB_CONFIRM) knob_steps += dir;
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

/* ---------------- port implementation ---------------- */
void tama_port_sfx(int cue)        { tama_sfx_queue(cue); }
void tama_port_led_mood(int mood)  { tama_sfx_led_mood(mood); }
void tama_port_brightness(int pct) { bsp_lcd_brightness_set(pct); }
void tama_port_save_request(void)  { /* Phase 2: NVS save */ }
void *tama_port_big_alloc(size_t size)
{
    return heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM);
}

/* ---------------- diagnostics task (Phase 0) ----------------
 * Battery percent does 10 ADC reads + log lines — poll every 30 s, off the
 * LVGL task (factory-firmware cadence). VBUS via raw pin level: the
 * bsp_system_is_charging() wrapper has inverted polarity and even the
 * factory firmware avoids it. */
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
static bool g_screen_dim = false;

static void ui_tick(lv_timer_t *tmr)
{
    /* apply queued input */
    int steps = knob_steps;
    if (steps) {
        knob_steps -= steps;
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
    if (active && g_screen_dim)  { g_screen_dim = false; bsp_lcd_brightness_set(100); }
    if (!active && !g_screen_dim) { g_screen_dim = true;  bsp_lcd_brightness_set(0); }

    tama_ui_tick();
}

void app_main(void)
{
    /* bsp_codec_dev_stop() also closes the never-enabled mic channel; the
     * i2s driver logs an error for it on every sound. Cosmetic — silence it. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);

    esp_io_expander_handle_t io = bsp_io_expander_init();
    assert(io != NULL);
    bsp_rgb_init();
    bsp_rtc_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

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

    /* Phase 0 probe: intermediate brightness steps for the future sleep dim */
    static const int steps[] = { 5, 20, 50, 100 };
    for (int i = 0; i < 4; i++) {
        bsp_lcd_brightness_set(steps[i]);
        ESP_LOGI(TAG, "brightness probe: %d%%", steps[i]);
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    tama_sfx_start();

    if (lvgl_port_lock(0)) {
        tama_ui_build();
        lv_timer_create(ui_tick, 80, NULL);
        lvgl_port_unlock();
    }

    input_init();
    xTaskCreate(diag_task, "diag", 4096, NULL, 3, NULL);

    g_last_interact_us = esp_timer_get_time();
    ESP_LOGI(TAG, "WatchaGotchi v0 running. free_internal=%d free_psram=%d",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    while (1) vTaskDelay(pdMS_TO_TICKS(5000));
}
