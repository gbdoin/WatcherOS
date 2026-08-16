#ifndef TAMA_UI_H
#define TAMA_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "tama_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live diagnostics (filled by tama_main's diag task; consumed on the LVGL
 * task by the battery status page). The sim fills representative values. */
typedef struct {
    volatile int      batt_mv;        /* bsp_battery_get_voltage() */
    volatile int      batt_pct;       /* bsp_battery_get_percent() */
    volatile int      batt_present;   /* bsp_battery_is_present() */
    volatile int      vbus_in;        /* VBUS pin level == 0 -> plugged */
    volatile int      chrg_pin;       /* raw Charger_CHRG level */
    volatile int      stdby_pin;      /* raw Charger_STDBY level */
    volatile int      rtc_ok;         /* bsp_rtc_get_time() succeeded */
    volatile int      rtc_h, rtc_m, rtc_s;
    volatile uint32_t updated_ms;     /* last refresh, esp_timer ms */
} tama_diag_t;

extern tama_diag_t g_tama_diag;

/* Build all widgets once (call with the LVGL lock held, before start). */
void tama_ui_build(void);

/* Bring the game to life. `blob`/`len` is the persisted state from NVS (or
 * NULL/0). A valid blob resumes the pet in the room; anything else is a
 * first boot: the UI opens the clock-set screen and creates a fresh egg
 * once the user confirms the time. Call once, after tama_ui_build(). */
void tama_ui_start(const uint8_t *blob, size_t len);

/* The live pet. Owned by the UI and mutated only on the LVGL/UI task; the
 * platform reads it inside tama_port_save_request() (same task context)
 * to serialize, and the sim uses it to script scenes. */
tama_state_t *tama_ui_state(void);

void tama_ui_tick(void);             /* 80 ms tick in the LVGL task */
void tama_ui_on_knob(int dir);       /* +1 / -1, already burst-gated */
void tama_ui_on_button(void);        /* short press */
void tama_ui_on_long_press(void);    /* long press */

#ifdef __cplusplus
}
#endif

#endif /* TAMA_UI_H */
