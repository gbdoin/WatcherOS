/*
 * tama_ui — all LVGL rendering. Portable: depends only on lvgl, tama_port,
 * tama_sprites. Compiles unchanged on the device and in the PC simulator.
 *
 * Pet rendering: chunky-pixel sprites are blitted at integer scale into an
 * RGB565 canvas (PSRAM on device — source buffers are CPU-copied by LVGL,
 * so the internal-DMA rule applies only to the draw buffer, never here).
 * Ring icons are sprites blitted into small chroma-keyed buffers (pure
 * green = transparent, LV_COLOR_CHROMA_KEY on both targets).
 * Colors go through lv_color_hex() so LV_COLOR_16_SWAP differences between
 * device (=1) and sim (=0) are handled by LVGL itself.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tama_ui.h"
#include "tama_port.h"
#include "tama_sprites.h"

#define SCREEN_W   412
#define SCREEN_H   412
#define RING_R     178
#define N_ICONS    8
#define TILE_SIZE  48
#define ICON_PX    32          /* 16x16 sprite at 2x */

#define CANVAS_W   288
#define CANVAS_H   288
#define BG_HEX     0x101418
#define CHROMA_HEX 0x00ff00

tama_diag_t g_tama_diag;

static const struct { int spr; uint32_t color; } ICONS[N_ICONS] = {
    { SPR_ICON_FEED,  0x6b4a26 },
    { SPR_ICON_LIGHT, 0x6e6426 },
    { SPR_ICON_GAME,  0x2e5c2e },
    { SPR_ICON_MEDS,  0x662a28 },
    { SPR_ICON_WC,    0x51422e },
    { SPR_ICON_STAT,  0x2a4c66 },
    { SPR_ICON_SCOLD, 0x533566 },
    { SPR_ICON_CLOCK, 0x46484c },
};

static lv_obj_t *tile_objs[N_ICONS];
static lv_obj_t *icon_imgs[N_ICONS];
static lv_img_dsc_t icon_dscs[N_ICONS];
static lv_color_t *icon_bufs[N_ICONS];
static lv_obj_t *canvas_img;
static lv_img_dsc_t canvas_dsc;
static lv_color_t *canvas_buf;
static lv_obj_t *diag_lbl;

static int  sel = -1;
static int  anim_cnt = 0;
static int  demo_pet = SPR_PET_EGG;
static bool canvas_dirty = true;
static bool diag_dirty = true;

/* ---------------- sprite blitter ---------------- */
static void buf_fill(lv_color_t *buf, int n, uint32_t hex)
{
    lv_color_t c = lv_color_hex(hex);
    for (int i = 0; i < n; i++) buf[i] = c;
}

/* Draw sprite `id` frame `fr` into an arbitrary buffer at (dx,dy), each
 * logical pixel scale×scale. Palette index 0 is transparent (skipped). */
static void blit_to(lv_color_t *buf, int bw, int bh,
                    int id, int fr, int dx, int dy, int scale)
{
    const tama_sprite_t *s = &TAMA_SPRITES[id];
    if (fr >= s->frames) fr = 0;
    int ppb = 8 / s->bpp;
    int row_bytes = (s->w + ppb - 1) / ppb;
    const uint8_t *base = s->data + (size_t)fr * row_bytes * s->h;
    for (int py = 0; py < s->h; py++) {
        const uint8_t *row = base + (size_t)py * row_bytes;
        for (int px = 0; px < s->w; px++) {
            int byte = row[px / ppb];
            int shift = (ppb - 1 - (px % ppb)) * s->bpp;
            int idx = (byte >> shift) & ((1 << s->bpp) - 1);
            if (idx == 0) continue;
            lv_color_t c = lv_color_hex(s->palette[idx]);
            int x0 = dx + px * scale, y0 = dy + py * scale;
            for (int yy = 0; yy < scale; yy++) {
                if ((unsigned)(y0 + yy) >= (unsigned)bh) continue;
                lv_color_t *dst = &buf[(y0 + yy) * bw + x0];
                for (int xx = 0; xx < scale; xx++) {
                    if ((unsigned)(x0 + xx) < (unsigned)bw) dst[xx] = c;
                }
            }
        }
    }
}

static void room_render(void)
{
    int fr = (anim_cnt / 6) % 2;
    buf_fill(canvas_buf, CANVAS_W * CANVAS_H, BG_HEX);
    blit_to(canvas_buf, CANVAS_W, CANVAS_H,
            demo_pet, fr, (CANVAS_W - 32 * 7) / 2, (CANVAS_H - 32 * 7) / 2, 7);
    lv_obj_invalidate(canvas_img);
}

/* ---------------- icon ring ---------------- */
static void icon_place(lv_obj_t *o, int i)
{
    float a = -90.0f + i * (360.0f / N_ICONS);
    float rad = a * (float)M_PI / 180.0f;
    int cx = SCREEN_W / 2 + (int)(RING_R * cosf(rad));
    int cy = SCREEN_H / 2 + (int)(RING_R * sinf(rad));
    lv_obj_set_pos(o, cx - TILE_SIZE / 2, cy - TILE_SIZE / 2);
}

static void icon_style(int i, bool selected)
{
    lv_obj_set_style_bg_color(tile_objs[i], lv_color_hex(ICONS[i].color), 0);
    lv_obj_set_style_bg_opa(tile_objs[i], selected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_border_width(tile_objs[i], selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(tile_objs[i],
        selected ? lv_color_white() : lv_color_hex(0x2a2e36), 0);
}

void tama_ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0c12), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* pet room canvas */
    canvas_buf = tama_port_big_alloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t));
    canvas_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    canvas_dsc.header.w = CANVAS_W;
    canvas_dsc.header.h = CANVAS_H;
    canvas_dsc.data_size = CANVAS_W * CANVAS_H * sizeof(lv_color_t);
    canvas_dsc.data = (const uint8_t *)canvas_buf;
    canvas_img = lv_img_create(scr);
    lv_img_set_src(canvas_img, &canvas_dsc);
    lv_obj_align(canvas_img, LV_ALIGN_CENTER, 0, -8);

    /* compact diagnostics readout, kept inside the round safe zone */
    diag_lbl = lv_label_create(scr);
    lv_label_set_text(diag_lbl, "");
    lv_obj_set_style_text_color(diag_lbl, lv_color_hex(0x6d7a52), 0);
    lv_obj_set_style_text_font(diag_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(diag_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(diag_lbl, 210);
    lv_label_set_long_mode(diag_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_align(diag_lbl, LV_ALIGN_CENTER, 0, 108);

    for (int i = 0; i < N_ICONS; i++) {
        tile_objs[i] = lv_obj_create(scr);
        lv_obj_set_size(tile_objs[i], TILE_SIZE, TILE_SIZE);
        lv_obj_set_style_radius(tile_objs[i], 12, 0);
        lv_obj_set_style_pad_all(tile_objs[i], 0, 0);
        lv_obj_clear_flag(tile_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tile_objs[i], LV_OBJ_FLAG_CLICKABLE);
        icon_place(tile_objs[i], i);

        icon_bufs[i] = tama_port_big_alloc(ICON_PX * ICON_PX * sizeof(lv_color_t));
        buf_fill(icon_bufs[i], ICON_PX * ICON_PX, CHROMA_HEX);
        blit_to(icon_bufs[i], ICON_PX, ICON_PX, ICONS[i].spr, 0, 0, 0, 2);
        icon_dscs[i].header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
        icon_dscs[i].header.w = ICON_PX;
        icon_dscs[i].header.h = ICON_PX;
        icon_dscs[i].data_size = ICON_PX * ICON_PX * sizeof(lv_color_t);
        icon_dscs[i].data = (const uint8_t *)icon_bufs[i];
        icon_imgs[i] = lv_img_create(tile_objs[i]);
        lv_img_set_src(icon_imgs[i], &icon_dscs[i]);
        lv_obj_center(icon_imgs[i]);

        icon_style(i, false);
    }

    canvas_dirty = true;
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
    demo_pet = (demo_pet == SPR_PET_EGG) ? SPR_PET_BABY : SPR_PET_EGG;
    canvas_dirty = true;
    tama_port_sfx(TSFX_CONFIRM);
}

void tama_ui_on_long_press(void)
{
    if (sel >= 0) { icon_style(sel, false); sel = -1; }
    tama_port_sfx(TSFX_BACK);
}

void tama_ui_tick(void)
{
    int prev_fr = (anim_cnt / 6) % 2;
    anim_cnt++;
    int fr = (anim_cnt / 6) % 2;
    if (fr != prev_fr || canvas_dirty) {
        canvas_dirty = false;
        room_render();
    }

    static uint32_t shown_ms = 0;
    if (g_tama_diag.updated_ms != shown_ms || diag_dirty) {
        shown_ms = g_tama_diag.updated_ms;
        diag_dirty = false;
        char b[96];
        snprintf(b, sizeof(b), "BAT %d%% %s  USB %s\n%02d:%02d",
                 g_tama_diag.batt_pct,
                 g_tama_diag.batt_present ? "ok" : "none",
                 g_tama_diag.vbus_in ? "in" : "out",
                 g_tama_diag.rtc_h, g_tama_diag.rtc_m);
        lv_label_set_text(diag_lbl, b);
    }
}
