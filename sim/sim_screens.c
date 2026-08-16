/*
 * sim_screens — integration test + screenshot suite for the REAL tama_ui.
 *
 * Registers a 412x412 framebuffer display, implements the tama_port seam
 * (settable clock, deterministic RNG), then plays one continuous pet life
 * the way a user would: knob turns, presses, and the passage of time.
 * Each named scene is dumped as raw RGB565 (render.py -> docs/shots/).
 * Where a state is awkward to reach organically the script pokes
 * tama_ui_state() fields directly — the room renders purely from state,
 * so poked scenes are still honest renderings of reachable states.
 *
 * Prints "SCENES: PASS/FAIL" and exits nonzero if a precondition missed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "../main/tama_ui.h"
#include "../main/tama_port.h"

#define W 412
#define H 412

static uint16_t fb[W * H];
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[W * 60];

/* ---- tama_port implementation (the sim owns the clock) ---- */
static uint32_t g_now = 0;            /* pet-epoch seconds, scene-scripted */
static int      g_brightness = -1;    /* recorded to verify the sleep dim */
static uint32_t g_rng = 0x1234ABCDu;  /* fixed seed: scenes are reproducible */

uint32_t tama_port_now(void) { return g_now; }
void tama_port_clock_shift(int32_t d) { g_now = (uint32_t)((int64_t)g_now + d); }
uint32_t tama_port_random(void)
{
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}
void tama_port_sfx(int cue)          { (void)cue; }
void tama_port_led_mood(int mood)    { (void)mood; }
void tama_port_brightness(int pct)   { g_brightness = pct; }
void tama_port_save_request(void)    {}
void tama_port_screen_sleep(void)    {}
void *tama_port_big_alloc(size_t n)  { return calloc(1, n); }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = (px++)->full;
    lv_disp_flush_ready(drv);
}

/* ---- scene driver ---- */
static int g_fail = 0;

static void expect(int cond, const char *fmt, ...)
{
    if (cond) return;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail = 1;
}

/* advance the pet clock, then run n 80 ms UI frames */
static void step(int ticks, uint32_t secs)
{
    g_now += secs;
    while (ticks-- > 0) tama_ui_tick();
}

static void knob(int n)
{
    for (; n > 0; n--) tama_ui_on_knob(+1);
    for (; n < 0; n++) tama_ui_on_knob(-1);
}

static void press(void) { tama_ui_on_button(); }
static void hold(void)  { tama_ui_on_long_press(); }

static void dump(const char *scene)
{
    char path[64];
    snprintf(path, sizeof(path), "out_%s.565", scene);
    lv_refr_now(NULL);
    FILE *f = fopen(path, "wb");
    if (!f) { expect(0, "cannot write %s", path); return; }
    fwrite(fb, sizeof(fb), 1, f);
    fclose(f);
    printf("wrote %s\n", path);
}

int main(void)
{
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, W * 60);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = W; drv.ver_res = H;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&drv);

    /* representative battery diagnostics for the status page */
    g_tama_diag.batt_mv = 3921; g_tama_diag.batt_pct = 78;
    g_tama_diag.batt_present = 1; g_tama_diag.vbus_in = 0;
    g_tama_diag.chrg_pin = 1; g_tama_diag.stdby_pin = 0;
    g_tama_diag.rtc_ok = 1; g_tama_diag.updated_ms = 1;

    tama_ui_build();
    tama_ui_start(NULL, 0);            /* no blob -> first-boot clock-set */
    tama_state_t *st = tama_ui_state();
    expect(st->magic == 0, "first boot must not have a live pet yet");

    /* 01: first-boot clock-set mid-edit — hour dialed to 10, still on it */
    step(2, 0);
    knob(10);
    step(2, 0);
    dump("01_setclock");

    /* confirm 10:30 -> egg is born at that wall time */
    press();                           /* hour -> minute */
    knob(30);
    press();                           /* apply -> new egg, room */
    expect(st->magic == TAMA_MAGIC && st->stage == TS_EGG,
           "clock confirm must create an egg (stage=%d)", st->stage);
    expect(g_now == 10u * 3600u + 30u * 60u, "pet clock must read 10:30");

    /* 02: egg idle in the room, nothing selected */
    step(3, 0);
    dump("02_egg");

    /* 03: egg hatches after 5 min; ride out the cinematic (30 ticks) */
    step(1, 5 * 60 + 1);
    expect(st->stage == TS_BABY, "egg must hatch to baby (stage=%d)", st->stage);
    step(32, 0);
    dump("03_hatch");

    /* 04: open the feed picker (first ring icon), MEAL highlighted */
    knob(1);                           /* select FEED */
    press();
    step(2, 0);
    dump("04_feed");

    /* 05: eat the meal — back in the room with the munch overlay up */
    press();
    expect(st->hunger == 3, "meal must land (hunger=%d)", st->hunger);
    step(2, 0);
    dump("05_eat");

    /* 06: fast-forward 65 min -> child evolution, then a bad afternoon:
     * attention call + 2 poops (poked: exact counts, deterministic) */
    step(1, 65 * 60);
    expect(st->stage == TS_CHILD, "baby must evolve to child (stage=%d)",
           st->stage);
    step(32, 0);                       /* cinematic -> room */
    hold();                            /* drop the ring selection */
    st->flags &= (uint8_t)~(TF_SICK | TF_ASLEEP | TF_LIGHTS_OFF);
    st->flags |= TF_ATTENTION;
    st->attention_kind = TATT_HUNGRY;
    st->attention_deadline = g_now + 900;
    st->hunger = 0;
    st->poop_count = 2;
    st->oldest_poop_epoch = g_now;     /* fresh: rot stays 2 h away */
    st->next_poop = 0;
    st->sick_death_deadline = 0;
    st->medicine_doses_left = 0;
    step(2, 0);
    dump("06_attention");

    /* 07: play the game into round 3 with a 2-round tally on the dots */
    knob(3);                           /* FEED -> LIGHT -> GAME */
    press();                           /* child is awake: game opens */
    step(2, 0);
    press();                           /* round 1 guess (HIGHER) */
    step(11, 0);                       /* reveal -> round 2 */
    press();                           /* round 2 guess */
    step(11, 0);                       /* reveal -> round 3 */
    knob(1);                           /* make sure HIGHER is highlighted */
    step(1, 0);
    dump("07_game");
    hold();                            /* abort: no outcome applied */

    /* 08: status page 1 — hunger at 3 of 4 hearts */
    st->flags &= (uint8_t)~(TF_ATTENTION | TF_MISBEHAVING);
    st->attention_kind = TATT_NONE;
    st->attention_deadline = 0;
    st->hunger = 3;
    st->happy = 2;
    knob(3);                           /* GAME -> MEDS -> WC -> STAT */
    press();
    step(2, 0);
    dump("08_status");

    /* 09: knob forward to the battery page */
    knob(4);
    step(2, 0);
    dump("09_battery");

    /* 10: clock view (live wall time) */
    press();                           /* status -> room */
    knob(2);                           /* SCOLD -> CLOCK */
    press();
    step(2, 0);
    dump("10_clock");
    hold();                            /* clock view -> room */
    hold();                            /* drop the ring selection */

    /* 11: sickness — skull overlay in a clean room */
    st->poop_count = 0;
    st->oldest_poop_epoch = 0;
    st->flags |= TF_SICK;
    st->medicine_doses_left = 1;
    st->sick_death_deadline = g_now + 6 * 3600;
    step(2, 0);
    dump("11_sick");

    /* 12: asleep, lights off — dark room, zzz, backlight dimmed */
    st->flags &= (uint8_t)~TF_SICK;
    st->sick_death_deadline = 0;
    st->flags |= TF_ASLEEP | TF_LIGHTS_OFF;
    step(2, 0);
    expect(g_brightness == 5, "sleep must dim backlight (%d%%)", g_brightness);
    dump("12_sleep");

    /* 13: child -> teen, caught mid-flash (frame alternates every 2 ticks) */
    st->flags &= (uint8_t)~(TF_ASLEEP | TF_LIGHTS_OFF);
    st->stage_mistakes = 0;            /* clean childhood -> TEEN_GOOD */
    st->evolve_epoch = g_now + 1;
    step(1, 2);                        /* trigger tick: cinematic starts */
    expect(st->stage == TS_TEEN && st->species == TEEN_GOOD,
           "must evolve to good teen (stage=%d sp=%d)", st->stage, st->species);
    step(2, 0);                        /* land on a lit flash frame */
    dump("13_evolve");
    step(32, 0);                       /* finish the cinematic */

    /* 14: perfect care -> HERO adult idling in the room */
    st->discipline = 100;
    st->stage_mistakes = 0;
    st->care_mistakes = 0;
    st->evolve_epoch = g_now + 1;
    step(1, 2);
    expect(st->stage == TS_ADULT && st->species == ADULT_HERO,
           "must evolve to HERO (stage=%d sp=%d)", st->stage, st->species);
    step(32, 0);
    dump("14_adult");

    /* 15: untreated sickness runs out — tombstone */
    st->age_days = 9;                  /* a life worth mourning */
    st->flags |= TF_SICK;
    st->medicine_doses_left = 2;
    st->sick_death_deadline = g_now + 1;
    step(1, 2);
    expect(st->stage == TS_DEAD, "sickness must be fatal (stage=%d)", st->stage);
    step(2, 0);
    dump("15_death");

    printf(g_fail ? "SCENES: FAIL\n" : "SCENES: PASS\n");
    return g_fail;
}
