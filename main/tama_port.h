/*
 * tama_port.h — the seam between portable code (tama_ui, tama_logic) and the
 * device. tama_main.c implements these on hardware; the PC sim stubs them.
 */
#ifndef TAMA_PORT_H
#define TAMA_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

/* sound cues (synthesized jingles; see tama_sfx.c) */
enum {
    TSFX_TICK_UP,     /* knob rotate clockwise */
    TSFX_TICK_DN,     /* knob rotate counter-clockwise */
    TSFX_CONFIRM,     /* action accepted */
    TSFX_DENY,        /* action refused (e.g. feeding when full) */
    TSFX_BACK,        /* long-press back/cancel */
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
void tama_port_save_request(void);

#ifdef __cplusplus
}
#endif

#endif /* TAMA_PORT_H */
