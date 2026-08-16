/*
 * tama_ui — all LVGL rendering. Portable: depends only on lvgl + tama_port.
 *
 * Phase 0 scope: the main room with a placeholder pet (bouncing square),
 * the 8-icon ring around the round bezel (knob rotates selection, press
 * "activates" with a confirm beep, long-press deselects), and a diagnostics
 * panel (battery / charger / RTC) that answers the dead-battery question.
 */
#include <stdio.h>
#include <math.h>

#include "tama_ui.h"
#include "tama_port.h"

#define SCREEN_W   412
#define SCREEN_H   412
#define RING_R     178          /* icon centers, from screen center */
#define N_ICONS    8
#define ICON_SIZE  44

tama_diag_t g_tama_diag;

/* Phase 0 placeholder icons: label text + accent color. Real pixel-art
 * sprites replace these in Phase 1. */
static const struct { const char *txt; uint32_t color; } ICONS[N_ICONS] = {
    { "FEED",  0xe8a13a },   /* meal/snack */
    { "LITE",  0xf5e04b },   /* lights */
    { "GAME",  0x62c962 },   /* higher/lower */
    { "MEDS",  0xd8544f },   /* medicine */
    { "WC",    0x8a6f4d },   /* toilet */
    { "STAT",  0x5aa2d8 },   /* meters */
    { "SCLD",  0xb06fd8 },   /* discipline */
    { "CLOK",  0x9aa0a6 },   /* clock (P1: attention slot; clock in v0) */
};

static lv_obj_t *icon_objs[N_ICONS];
static lv_obj_t *icon_lbls[N_ICONS];
static lv_obj_t *pet_obj;
static lv_obj_t *pet_face;
static lv_obj_t *diag_lbl;
static lv_obj_t *title_lbl;

static int  sel = -1;               /* selected ring icon, -1 = none */
static int  anim_cnt = 0;           /* frame counter for the placeholder bounce */
static bool diag_dirty = true;

static void icon_place(lv_obj_t *o, int i)
{
    /* icon 0 at 12 o'clock, clockwise */
    float a = -90.0f + i * (360.0f / N_ICONS);
    float rad = a * (float)M_PI / 180.0f;
    int cx = SCREEN_W / 2 + (int)(RING_R * cosf(rad));
    int cy = SCREEN_H / 2 + (int)(RING_R * sinf(rad));
    lv_obj_set_pos(o, cx - ICON_SIZE / 2, cy - ICON_SIZE / 2);
}

static void icon_style(int i, bool selected)
{
    lv_obj_set_style_bg_color(icon_objs[i], lv_color_hex(ICONS[i].color), 0);
    lv_obj_set_style_bg_opa(icon_objs[i], selected ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_border_width(icon_objs[i], selected ? 3 : 0, 0);
    lv_obj_set_style_border_color(icon_objs[i], lv_color_white(), 0);
    lv_obj_set_style_text_color(icon_lbls[i],
        selected ? lv_color_black() : lv_color_white(), 0);
}

void tama_ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0c12), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, "WATCHAGOTCHI v0");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x4a5568), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, -110);

    /* placeholder pet: a rounded square that "bounces" (frame-flip anim) */
    pet_obj = lv_obj_create(scr);
    lv_obj_set_size(pet_obj, 96, 96);
    lv_obj_align(pet_obj, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_color(pet_obj, lv_color_hex(0x62c962), 0);
    lv_obj_set_style_radius(pet_obj, 18, 0);
    lv_obj_set_style_border_width(pet_obj, 0, 0);
    lv_obj_clear_flag(pet_obj, LV_OBJ_FLAG_SCROLLABLE);
    pet_face = lv_label_create(pet_obj);
    lv_label_set_text(pet_face, "'o'");
    lv_obj_set_style_text_color(pet_face, lv_color_black(), 0);
    lv_obj_set_style_text_font(pet_face, &lv_font_montserrat_28, 0);
    lv_obj_center(pet_face);

    /* diagnostics panel (Phase 0 only) */
    diag_lbl = lv_label_create(scr);
    lv_label_set_text(diag_lbl, "diag: waiting...");
    lv_obj_set_style_text_color(diag_lbl, lv_color_hex(0x8ce63a), 0);
    lv_obj_set_style_text_font(diag_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(diag_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(diag_lbl, LV_ALIGN_CENTER, 0, 78);

    /* icon ring */
    for (int i = 0; i < N_ICONS; i++) {
        icon_objs[i] = lv_obj_create(scr);
        lv_obj_set_size(icon_objs[i], ICON_SIZE, ICON_SIZE);
        lv_obj_set_style_radius(icon_objs[i], 10, 0);
        lv_obj_clear_flag(icon_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon_objs[i], LV_OBJ_FLAG_CLICKABLE);
        icon_place(icon_objs[i], i);
        icon_lbls[i] = lv_label_create(icon_objs[i]);
        lv_label_set_text(icon_lbls[i], ICONS[i].txt);
        lv_obj_set_style_text_font(icon_lbls[i], &lv_font_montserrat_16, 0);
        lv_obj_center(icon_lbls[i]);
        icon_style(i, false);
    }
}

void tama_ui_on_knob(int dir)
{
    int prev = sel;
    if (sel < 0) sel = dir > 0 ? 0 : N_ICONS - 1;
    else sel = (sel + dir + N_ICONS) % N_ICONS;
    if (prev >= 0) icon_style(prev, false);
    icon_style(sel, true);
    tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
}

void tama_ui_on_button(void)
{
    if (sel < 0) return;
    /* Phase 0: acknowledge only; actions arrive with the game logic */
    tama_port_sfx(TSFX_CONFIRM);
    lv_label_set_text(pet_face, "^o^");
}

void tama_ui_on_long_press(void)
{
    if (sel >= 0) { icon_style(sel, false); sel = -1; }
    tama_port_sfx(TSFX_BACK);
    lv_label_set_text(pet_face, "'o'");
}

void tama_ui_tick(void)
{
    /* placeholder pet bounce at ~2 fps (tick is 80 ms) */
    if (++anim_cnt % 6 == 0) {
        bool up = (anim_cnt / 6) % 2;
        lv_obj_align(pet_obj, LV_ALIGN_CENTER, 0, up ? -24 : -20);
    }

    /* refresh diag text when the diag task published new numbers */
    static uint32_t shown_ms = 0;
    if (g_tama_diag.updated_ms != shown_ms || diag_dirty) {
        shown_ms = g_tama_diag.updated_ms;
        diag_dirty = false;
        char b[160];
        snprintf(b, sizeof(b),
                 "BAT %dmV %d%% %s\nUSB:%s CHRG:%d STDBY:%d\nRTC %s %02d:%02d:%02d",
                 g_tama_diag.batt_mv, g_tama_diag.batt_pct,
                 g_tama_diag.batt_present ? "present" : "MISSING",
                 g_tama_diag.vbus_in ? "in" : "out",
                 g_tama_diag.chrg_pin, g_tama_diag.stdby_pin,
                 g_tama_diag.rtc_ok ? "ok" : "ERR",
                 g_tama_diag.rtc_h, g_tama_diag.rtc_m, g_tama_diag.rtc_s);
        lv_label_set_text(diag_lbl, b);
    }
}
