/*
 * sim_screens — headless renderer for the REAL tama_ui.c.
 *
 * Registers a 412x412 framebuffer display, stubs the tama_port_* seam,
 * drives tama_ui through representative scenes, and dumps each to a raw
 * RGB565 file (render.py turns them into round PNGs in ../docs/).
 */
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

/* ---- tama_port stubs ---- */
void tama_port_sfx(int cue)        { (void)cue; }
void tama_port_led_mood(int mood)  { (void)mood; }
void tama_port_brightness(int pct) { (void)pct; }
void tama_port_save_request(void)  {}
void *tama_port_big_alloc(size_t n) { return calloc(1, n); }

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            fb[y * W + x] = (px++)->full;
    lv_disp_flush_ready(drv);
}

static void dump(const char *path)
{
    lv_refr_now(NULL);
    FILE *f = fopen(path, "wb");
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

    /* representative diagnostics so the readout isn't empty */
    g_tama_diag.batt_mv = 4182; g_tama_diag.batt_pct = 100;
    g_tama_diag.batt_present = 0; g_tama_diag.vbus_in = 1;
    g_tama_diag.chrg_pin = 1; g_tama_diag.stdby_pin = 0;
    g_tama_diag.rtc_ok = 1; g_tama_diag.rtc_h = 8; g_tama_diag.rtc_m = 49; g_tama_diag.rtc_s = 19;
    g_tama_diag.updated_ms = 1;

    tama_ui_build();

    /* scene 1: egg, no selection */
    tama_ui_tick();
    dump("out_tama_egg.565");

    /* scene 2: baby, FEED icon selected */
    tama_ui_on_button();          /* egg -> baby */
    tama_ui_on_knob(+1);          /* select icon 0 (FEED) */
    tama_ui_tick();
    dump("out_tama_baby.565");

    return 0;
}
