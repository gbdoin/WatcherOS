/*
 * tama_port.h — the seam between portable code (tama_ui, tama_logic) and the
 * device. tama_main.c implements these on hardware; the PC sim stubs them.
 */
#ifndef TAMA_PORT_H
#define TAMA_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sound cues (synthesized chirps/jingles; see tama_sfx.c) */
enum {
    TSFX_TICK_UP,     /* knob rotate clockwise */
    TSFX_TICK_DN,     /* knob rotate counter-clockwise */
    TSFX_CONFIRM,     /* action accepted */
    TSFX_DENY,        /* action refused (e.g. feeding when full) */
    TSFX_BACK,        /* long-press back/cancel */
    TSFX_EAT,         /* munching chirp */
    TSFX_CLEAN,       /* flush sweep */
    TSFX_SCOLD,       /* stern buzz */
    TSFX_CURE,        /* medicine worked */
    TSFX_CALL,        /* attention call (icon lit) */
    TSFX_WIN,         /* game won */
    TSFX_LOSE,        /* game lost */
    TSFX_EVOLVE,      /* evolution fanfare */
    TSFX_DEATH,       /* death chime */
};

/* LED moods (owned by the fx task; one active at a time) */
enum {
    TLED_OFF,
    TLED_ATTENTION,   /* red flash */
    TLED_SICK,        /* slow yellow-green pulse */
    TLED_EVOLVE,      /* rainbow glow */
    TLED_SLEEP,       /* faint blue breathing */
};

void tama_port_sfx(int cue);
void tama_port_led_mood(int mood);
void tama_port_brightness(int pct);

/* Ask the platform to persist the pet. Called from the LVGL/UI task right
 * after a TEV_STATE_DIRTY; the device serializes tama_ui_state() immediately
 * (cheap memcpy) and commits to NVS from a lower-priority task. */
void tama_port_save_request(void);

/* large buffer alloc: PSRAM on device, malloc in the sim */
void *tama_port_big_alloc(size_t size);

/* Current pet-epoch, in seconds. Pet-epoch 0 is midnight of day 0, so
 * hour-of-day = (now/3600)%24 — the pet's wall clock and its life clock are
 * the same axis. Device: NVS-persisted snapshot + esp_timer (scaled by
 * TAMA_TIME_SCALE for demos); frozen across power-off (hibernation).
 * Sim: a plain settable variable. Monotonic between clock_shift calls. */
uint32_t tama_port_now(void);

/* The user moved the wall clock by delta_s seconds (new - old). The UI has
 * already called tama_clock_rebase() on the pet; the platform shifts its
 * epoch base by the same delta and persists it. tama_port_now() reflects
 * the shift immediately. */
void tama_port_clock_shift(int32_t delta_s);

/* Non-gameplay randomness (game numbers, new-egg seeds): esp_random() on
 * device, any seeded PRNG in the sim. Gameplay randomness stays inside
 * tama_logic's persisted RNG. */
uint32_t tama_port_random(void);

#ifdef __cplusplus
}
#endif

#endif /* TAMA_PORT_H */
