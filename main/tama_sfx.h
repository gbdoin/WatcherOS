#ifndef TAMA_SFX_H
#define TAMA_SFX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Spawns the fx task (audio + RGB LED). Call once from app_main. */
void tama_sfx_start(void);

/* Thread-safe producers (implement tama_port_sfx / tama_port_led_mood). */
void tama_sfx_queue(int cue);
void tama_sfx_led_mood(int mood);

#ifdef __cplusplus
}
#endif

#endif /* TAMA_SFX_H */
