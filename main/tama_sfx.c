/*
 * tama_sfx — audio + RGB LED, own task, never blocks the UI.
 *
 * Pattern preserved from watcher_os.c fx_task: init the codec once, keep I2S
 * DISABLED when idle (bsp_codec_dev_stop) and resume only around playback.
 * Even without WiFi this keeps idle power down. The WS2812 refresh is a
 * blocking RMT transaction, so the LED is also driven only from this task.
 */
#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"

#include "sensecap-watcher.h"
#include "tama_port.h"
#include "tama_sfx.h"

#define SR 16000

static QueueHandle_t   snd_q;
static volatile int    led_mood = TLED_OFF;

static void play_tone(float f, int ms, int ampl)
{
    int n = (SR * ms) / 1000;
    int16_t *b = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!b) return;
    int env = SR / 100;
    for (int i = 0; i < n; i++) {
        float g = 1.0f;
        if (i < env) g = (float)i / env;
        else if (i > n - env) g = (float)(n - i) / env;
        b[i] = (int16_t)(ampl * g * sinf(2.0f * (float)M_PI * f * i / SR));
    }
    size_t w = 0;
    bsp_i2s_write(b, n * sizeof(int16_t), &w, 500);
    free(b);
}

/* Note tables: freq 0 = rest (vTaskDelay, not a silent buffer — play_tone
 * would still malloc). Each jingle stays under ~1.5 s so queued cues drain. */
typedef struct { int16_t freq; int16_t ms; } note_t;

static const note_t seq_tick_up[] = {{1200, 26}};
static const note_t seq_tick_dn[] = {{800, 26}};
static const note_t seq_confirm[] = {{880, 70}, {1320, 90}};
static const note_t seq_deny[]    = {{300, 120}};
static const note_t seq_back[]    = {{600, 60}};
static const note_t seq_eat[]     = {{500, 40}, {0, 20}, {420, 40}, {0, 20}, {500, 40}};
static const note_t seq_clean[]   = {{1400, 45}, {1050, 45}, {750, 45}, {500, 45}};
static const note_t seq_scold[]   = {{220, 90}, {0, 60}, {180, 140}};
static const note_t seq_cure[]    = {{660, 80}, {990, 160}};
static const note_t seq_call[]    = {{2093, 70}, {0, 60}, {2093, 70}};
static const note_t seq_win[]     = {{523, 90}, {659, 90}, {784, 90}, {1047, 180}};
static const note_t seq_lose[]    = {{392, 120}, {330, 120}, {262, 120}};
static const note_t seq_evolve[]  = {{523, 110}, {523, 110}, {659, 110}, {784, 110},
                                     {1047, 110}, {784, 110}, {1047, 320}};
static const note_t seq_death[]   = {{440, 300}, {415, 300}, {392, 300}, {330, 400}};

static const struct { const note_t *n; uint8_t len; int16_t ampl; } cue_tab[] = {
    [TSFX_TICK_UP] = {seq_tick_up, 1, 6000},
    [TSFX_TICK_DN] = {seq_tick_dn, 1, 6000},
    [TSFX_CONFIRM] = {seq_confirm, 2, 8000},
    [TSFX_DENY]    = {seq_deny,    1, 8000},
    [TSFX_BACK]    = {seq_back,    1, 7000},
    [TSFX_EAT]     = {seq_eat,     5, 7000},
    [TSFX_CLEAN]   = {seq_clean,   4, 7000},
    [TSFX_SCOLD]   = {seq_scold,   3, 8000},
    [TSFX_CURE]    = {seq_cure,    2, 8000},
    [TSFX_CALL]    = {seq_call,    3, 9000},
    [TSFX_WIN]     = {seq_win,     4, 8000},
    [TSFX_LOSE]    = {seq_lose,    3, 7000},
    [TSFX_EVOLVE]  = {seq_evolve,  7, 8000},
    [TSFX_DEATH]   = {seq_death,   4, 5000},  /* somber: low amplitude */
};

static void play_cue(int cue)
{
    if (cue < 0 || cue >= (int)(sizeof(cue_tab) / sizeof(cue_tab[0]))) return;
    const note_t *n = cue_tab[cue].n;
    if (!n) return;
    for (int i = 0; i < cue_tab[cue].len; i++) {
        if (n[i].freq) play_tone(n[i].freq, n[i].ms, cue_tab[cue].ampl);
        else vTaskDelay(pdMS_TO_TICKS(n[i].ms));
    }
}

/* one LED animation step; called every 50 ms from the task loop */
static void led_step(void)
{
    static int t = 0;
    t++;
    switch (led_mood) {
        case TLED_ATTENTION:  /* sharp red flash, 2.5 Hz */
            bsp_rgb_set((t / 4) % 2 ? 80 : 0, 0, 0);
            break;
        case TLED_SICK: {     /* slow sickly pulse */
            int v = (t % 40) < 20 ? (t % 40) : 40 - (t % 40);
            bsp_rgb_set(v * 2, v * 3, 0);
            break;
        }
        case TLED_EVOLVE: {   /* hue rotation */
            int ph = t % 30;
            uint8_t r = ph < 10 ? 60 : 0, g = (ph >= 10 && ph < 20) ? 60 : 0,
                    b = ph >= 20 ? 60 : 0;
            bsp_rgb_set(r, g, b);
            break;
        }
        case TLED_SLEEP: {    /* faint blue breathing, ~0.25 Hz */
            int ph = t % 80;
            int v = ph < 40 ? ph : 80 - ph;
            bsp_rgb_set(0, 0, v / 3);
            break;
        }
        default:
            bsp_rgb_set(0, 0, 0);
            break;
    }
}

static void fx_task(void *arg)
{
    bsp_codec_init();
    bsp_codec_volume_set(80, NULL);
    bsp_codec_dev_stop();               /* I2S off until a sound is needed */
    while (1) {
        int cue;
        if (xQueueReceive(snd_q, &cue, pdMS_TO_TICKS(50)) == pdTRUE) {
            bsp_codec_dev_resume();
            do {
                play_cue(cue);
            } while (xQueueReceive(snd_q, &cue, 0) == pdTRUE);
            bsp_codec_dev_stop();
        }
        led_step();
    }
}

void tama_sfx_start(void)
{
    snd_q = xQueueCreate(8, sizeof(int));
    xTaskCreatePinnedToCore(fx_task, "fx", 4096, NULL, 5, NULL, 1);
}

void tama_sfx_queue(int cue)
{
    if (snd_q) xQueueSend(snd_q, &cue, 0);
}

void tama_sfx_led_mood(int mood)
{
    led_mood = mood;
}
