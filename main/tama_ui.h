#ifndef TAMA_UI_H
#define TAMA_UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Live diagnostics shown on the Phase 0 screen (filled by tama_main's
 * diag task; consumed by tama_ui_tick on the LVGL task). */
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

void tama_ui_build(void);            /* build all widgets once (LVGL lock held) */
void tama_ui_tick(void);             /* 80 ms tick in the LVGL task */
void tama_ui_on_knob(int dir);       /* +1 / -1, already burst-gated */
void tama_ui_on_button(void);        /* short press */
void tama_ui_on_long_press(void);    /* long press */

#ifdef __cplusplus
}
#endif

#endif /* TAMA_UI_H */
