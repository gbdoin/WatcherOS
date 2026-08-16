/*
 * tama_ui — all LVGL rendering and input routing for WatchaGotchi.
 * Portable: depends only on lvgl, tama_logic, tama_port, tama_sprites;
 * compiles unchanged on the device and in the PC simulator.
 *
 * Architecture: every widget is created once in tama_ui_build(); after
 * that the UI only shows/hides containers and rewrites label text /
 * pixel buffers (diff-style, in tama_ui_tick). One mode enum routes
 * input and visibility. The room is re-rendered purely from the logic
 * state, so a scene script that pokes state fields and ticks the UI
 * renders correctly with no extra plumbing.
 *
 * Pet rendering: chunky-pixel sprites are blitted at integer scale into
 * an RGB565 canvas (PSRAM on device — source buffers are CPU-copied by
 * LVGL, so the internal-DMA rule applies only to the draw buffer, never
 * here). Small overlays (icons, hearts, food, attention, tombstone) are
 * sprites blitted into chroma-keyed buffers (pure green = transparent,
 * LV_COLOR_CHROMA_KEY on both targets). Colors go through
 * lv_color_hex() so LV_COLOR_16_SWAP differences between device (=1)
 * and sim (=0) are handled by LVGL itself.
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
#define PET_SCALE  7           /* 32x32 -> 224 px */
#define PET_XY     ((CANVAS_W - 32 * PET_SCALE) / 2)

#define BG_HEX        0x101418
#define BG_DARK_HEX   0x05060a  /* lights off at night */
#define BG_FLASH_HEX  0x232a3a  /* evolution strobe */
#define PANEL_HEX     0x0a0c12  /* sub-screen backdrop = screen bg */
#define TXT_HEX       0xe8ecf2
#define DIM_HEX       0x818b9c
#define ACCENT_HEX    0x2a4c66  /* edit-field / bar fill */
#define OK_HEX        0x3fa060
#define BAD_HEX       0xc05050
#define IDLE_BORDER   0x2a2e36
#define CHROMA_HEX    0x00ff00

#define F14 (&lv_font_montserrat_14)
#define F16 (&lv_font_montserrat_16)
#define F28 (&lv_font_montserrat_28)
#define F48 (&lv_font_montserrat_48)

/* 80 ms ticks: /6 = 480 ms per animation frame (~2 fps idle) */
#define ANIM_DIV          6
#define EAT_TICKS         13   /* ~1 s food overlay */
#define REVEAL_TICKS      10   /* ~0.8 s per game reveal */
#define RESULT_TICKS      19   /* ~1.5 s win/lose splash */
#define EVOLVE_TICKS_TOT  30   /* ~2.4 s cinematic */
#define EVOLVE_SWAP       2    /* sprite swap every ~160 ms */

#define GAME_ROUNDS       5
#define GAME_WIN_NEED     3

tama_diag_t g_tama_diag;

typedef enum {
    MODE_ROOM = 0, MODE_FEED, MODE_GAME, MODE_STAT, MODE_CLOCK,
    MODE_EVOLVE, MODE_DEAD,
} ui_mode_t;

enum { CLK_VIEW = 0, CLK_HOUR, CLK_MIN };          /* clock phases */
enum { CLKP_VIEW = 0, CLKP_FIRSTBOOT, CLKP_NEWEGG };
enum { GP_PICK = 0, GP_REVEAL, GP_RESULT };        /* game phases */

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

/* ---- room layer ---- */
static lv_obj_t *tile_objs[N_ICONS];
static lv_obj_t *icon_imgs[N_ICONS];
static lv_img_dsc_t icon_dscs[N_ICONS];
static lv_color_t *icon_bufs[N_ICONS];
static lv_obj_t *canvas_img;
static lv_img_dsc_t canvas_dsc;
static lv_color_t *canvas_buf;
static lv_obj_t *attn_img;
static lv_img_dsc_t attn_dsc;
static lv_color_t *attn_buf;

/* ---- feed screen ---- */
static lv_obj_t *feed_scr, *feed_items[2], *feed_lbls[2];
static lv_img_dsc_t feed_dscs[2];
static lv_color_t *feed_bufs[2];

/* ---- game screen ---- */
static lv_obj_t *game_scr, *game_num, *game_round_lbl, *game_result;
static lv_obj_t *game_opts[2], *game_opt_lbls[2], *game_dots[GAME_ROUNDS];

/* ---- status screen ---- */
static lv_obj_t *stat_scr, *stat_title, *stat_big, *stat_sub;
static lv_obj_t *stat_bar, *stat_page_lbl;
static lv_obj_t *stat_hearts[TAMA_MAX_HEARTS];
static lv_img_dsc_t heart_dscs[TAMA_MAX_HEARTS];
static lv_color_t *heart_bufs[TAMA_MAX_HEARTS];

/* ---- clock screen ---- */
static lv_obj_t *clock_scr, *clk_title, *clk_hh, *clk_colon, *clk_mm, *clk_hint;

/* ---- death screen ---- */
static lv_obj_t *death_scr, *death_lbl, *death_hint, *grave_img;
static lv_img_dsc_t grave_dsc;
static lv_color_t *grave_buf;

/* ---- UI state ---- */
static tama_state_t g_state;          /* the live pet (magic==0 pre-egg) */
static ui_mode_t mode = MODE_ROOM;
static int  sel = -1;                 /* ring selection */
static uint32_t anim_cnt = 0;   /* unsigned: a signed /-then-% would go
                                 * negative after ~5.4 years of uptime and
                                 * index sprites out of bounds */
static bool room_dirty = true;
static uint32_t last_pet_now = 0;

static int  room_spr = SPR_PET_EGG;   /* last pet sprite drawn (evolve "old") */
static uint32_t room_sig_last = 0xffffffffu;
static int  eat_spr = SPR_PROP_MEAL;
static int  eat_ticks = 0;
static int  attn_frame_drawn = -1;

static int  evolve_ticks = 0, evolve_old = SPR_PET_EGG, evolve_phase_drawn = -1;

static int  feed_sel = 0;

static int  game_phase = GP_PICK, game_round = 0, game_score = 0;
static int  game_cur = 1, game_sel = 1;   /* 1 = HIGHER */
static int  game_wait = 0;

static int  stat_page = 0;
static int  st_hunger = -1, st_happy = -1, st_disc = -1, st_weight = -1;
static int  st_age = -1, st_bpct = -1, st_bpres = -1, st_vbus = -1;

static int  clk_phase = CLK_VIEW, clk_purpose = CLKP_VIEW;
static int  clk_h = 0, clk_m = 0, clk_shown_min = -1;

static int  cur_led = -1, cur_bright = -1;   /* -1 forces first apply */

static void set_mode(ui_mode_t m);
static void stat_render(void);
static void clock_refresh(void);
static void enter_death(void);

static bool state_valid(void) { return g_state.magic == TAMA_MAGIC; }

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
    if (fr < 0 || fr >= s->frames) fr = 0;
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

/* chroma-keyed lv_img backed by a heap buffer (icons/overlays pattern) */
static lv_obj_t *chroma_img(lv_obj_t *parent, lv_img_dsc_t *dsc,
                            lv_color_t **buf, int px)
{
    *buf = tama_port_big_alloc((size_t)px * px * sizeof(lv_color_t));
    buf_fill(*buf, px * px, CHROMA_HEX);
    dsc->header.cf = LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED;
    dsc->header.w = px;
    dsc->header.h = px;
    dsc->data_size = (uint32_t)px * px * sizeof(lv_color_t);
    dsc->data = (const uint8_t *)*buf;
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, dsc);
    return img;
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
        selected ? lv_color_white() : lv_color_hex(IDLE_BORDER), 0);
}

/* ---------------- pet & room rendering ---------------- */
static int pet_sprite(const tama_state_t *s)
{
    switch ((tama_stage_t)s->stage) {
    case TS_EGG:   return SPR_PET_EGG;
    case TS_BABY:  return SPR_PET_BABY;
    case TS_CHILD: return SPR_PET_CHILD;
    case TS_TEEN:
        return s->species == TEEN_GOOD ? SPR_PET_TEEN_GOOD : SPR_PET_TEEN_BAD;
    case TS_ADULT:
        switch (s->species) {
        case ADULT_HERO:  return SPR_PET_ADULT_HERO;
        case ADULT_CHEER: return SPR_PET_ADULT_CHEER;
        case ADULT_AVG:   return SPR_PET_ADULT_AVG;
        case ADULT_GRUMP: return SPR_PET_ADULT_GRUMP;
        default:          return SPR_PET_ADULT_FERAL;
        }
    case TS_SECRET: return SPR_PET_SECRET;
    default:        return SPR_PET_EGG;   /* dead: death screen covers it */
    }
}

/* The whole room from STATE, nothing else: bg (lights), poops, pet,
 * skull/zzz overlays, transient food. Self-corrects after any state poke. */
static void room_render(void)
{
    const tama_state_t *s = &g_state;
    bool asleep = state_valid() && tama_is_asleep(s);
    bool dark = asleep && tama_lights_off(s);
    int  fr2 = (anim_cnt / ANIM_DIV) % 2;

    buf_fill(canvas_buf, CANVAS_W * CANVAS_H, dark ? BG_DARK_HEX : BG_HEX);

    if (state_valid()) {
        int poops = s->poop_count > TAMA_MAX_POOP ? TAMA_MAX_POOP : s->poop_count;
        for (int i = 0; i < poops; i++)   /* floor row; canvas x 46..244 so
                                           * the STAT/MEDS ring tiles
                                           * (screen x<=104 / x>=308) never
                                           * cover a poop */
            blit_to(canvas_buf, CANVAS_W, CANVAS_H,
                    SPR_PROP_POOP, fr2, 46 + i * 50, CANVAS_H - 58, 3);

        /* 4-frame pets are idle/idle/eat/sleep (docs/ART-REVIEW.md); the
         * 2-frame egg/baby just bounce. Idle only ever alternates 0/1. */
        int id = pet_sprite(s);
        int nfr = TAMA_SPRITES[id].frames;
        int fr = fr2;
        if (nfr >= 4) {
            if (asleep)            fr = 3;
            else if (eat_ticks > 0) fr = 2;
        } else if (asleep) {
            fr = 0;
        }
        blit_to(canvas_buf, CANVAS_W, CANVAS_H, id, fr, PET_XY, PET_XY, PET_SCALE);
        room_spr = id;

        /* head overlays sit inside the canvas edge: further out they'd
         * slide under the LIGHT/CLOCK ring tiles (screen x>=308 / x<=104) */
        if (tama_is_sick(s))   /* near the head, right side */
            blit_to(canvas_buf, CANVAS_W, CANVAS_H, SPR_PROP_SKULL, 0, 192, 10, 3);
        if (asleep)            /* low left: clears the skull, the CLOCK
                                * tile (screen y<=104) and the bedtime
                                * attention badge (y<=95), all of which
                                * can be up at once on a lights-on night */
            blit_to(canvas_buf, CANVAS_W, CANVAS_H, SPR_PROP_ZZZ, 0, 0, 52, 3);
        if (eat_ticks > 0)     /* food in front of the pet while munching */
            blit_to(canvas_buf, CANVAS_W, CANVAS_H, eat_spr, 0, 20, 168, 4);
    }
    lv_obj_invalidate(canvas_img);
}

/* evolution cinematic: strobe old/new sprite every EVOLVE_SWAP ticks */
static void evolve_render(void)
{
    int e = evolve_ticks / EVOLVE_SWAP;
    bool flash = (e & 1) != 0;
    int id = flash ? pet_sprite(&g_state) : evolve_old;
    buf_fill(canvas_buf, CANVAS_W * CANVAS_H, flash ? BG_FLASH_HEX : BG_HEX);
    blit_to(canvas_buf, CANVAS_W, CANVAS_H, id, 0, PET_XY, PET_XY, PET_SCALE);
    lv_obj_invalidate(canvas_img);
}

/* Everything the room draws, folded into one word. Scene scripts poke
 * state fields directly and expect the very next tick to render them, so
 * the redraw trigger must be the state itself, not just our own events. */
static uint32_t room_sig(void)
{
    if (!state_valid()) return 0xfffffffeu;
    return (uint32_t)g_state.stage
         | ((uint32_t)g_state.species << 4)
         | ((uint32_t)(g_state.poop_count & 7u) << 8)
         | ((uint32_t)(g_state.flags
                       & (TF_SICK | TF_ASLEEP | TF_LIGHTS_OFF)) << 12);
}

/* attention bubble: blinks via its 2 frames; only ever visible in ROOM */
static void attn_update(void)
{
    bool show = state_valid() && tama_attention(&g_state) && mode == MODE_ROOM;
    if (show) {
        int fr = (anim_cnt / ANIM_DIV) % 2;
        if (fr != attn_frame_drawn) {
            attn_frame_drawn = fr;
            buf_fill(attn_buf, TILE_SIZE * TILE_SIZE, CHROMA_HEX);
            blit_to(attn_buf, TILE_SIZE, TILE_SIZE, SPR_PROP_ATTENTION, fr, 0, 0, 3);
            lv_obj_invalidate(attn_img);
        }
        lv_obj_clear_flag(attn_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        attn_frame_drawn = -1;
        lv_obj_add_flag(attn_img, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------- mode switching ---------------- */
static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_mode(ui_mode_t m)
{
    mode = m;
    show(feed_scr,  m == MODE_FEED);
    show(game_scr,  m == MODE_GAME);
    show(stat_scr,  m == MODE_STAT);
    show(clock_scr, m == MODE_CLOCK);
    show(death_scr, m == MODE_DEAD);
    if (m == MODE_ROOM) room_dirty = true;
}

/* ---------------- common event fan-out ---------------- */
/* Every tama_tick()/tama_action() result goes through here exactly once.
 * `user_act` gates TSFX_DENY: a refusal chirp is feedback for a button,
 * never for a background tick. */
static void handle_events(uint32_t ev, bool user_act)
{
    if (ev == 0) return;
    room_dirty = true;
    if (ev & TEV_STATE_DIRTY) tama_port_save_request();
    if (ev & TEV_ATE)     tama_port_sfx(TSFX_EAT);
    if (ev & TEV_CLEANED) tama_port_sfx(TSFX_CLEAN);
    if (ev & TEV_CURED)   tama_port_sfx(TSFX_CURE);
    if (ev & TEV_SCOLDED) tama_port_sfx(TSFX_SCOLD);
    if (ev & (TEV_ATTENTION_ON | TEV_GOT_SICK)) tama_port_sfx(TSFX_CALL);
    if (user_act && (ev & TEV_REFUSED)) tama_port_sfx(TSFX_DENY);
    if (ev & TEV_DIED) {
        enter_death();
        return;                       /* death outranks a same-tick evolve */
    }
    if (ev & (TEV_EVOLVED | TEV_HATCHED)) {
        /* room_spr still holds the pre-evolution sprite (it only changes
         * inside room_render), so capture it before the first re-render */
        evolve_old = room_spr;
        evolve_ticks = EVOLVE_TICKS_TOT;
        evolve_phase_drawn = -1;
        set_mode(MODE_EVOLVE);
        tama_port_sfx(TSFX_EVOLVE);
        return;
    }
    /* bedtime arriving mid-game aborts it (like long-press): the pending
     * outcome would only be refused by the asleep gate, and a WIN! splash
     * over a voided reward would lie to the player */
    if ((ev & TEV_FELL_ASLEEP) && mode == MODE_GAME)
        set_mode(MODE_ROOM);
}

static uint32_t do_action(tama_action_t a)
{
    uint32_t ev = tama_action(&g_state, a, tama_port_now());
    handle_events(ev, true);
    return ev;
}

/* ---------------- feed screen ---------------- */
static void feed_style(void)
{
    for (int i = 0; i < 2; i++) {
        bool hot = (feed_sel == i);
        lv_obj_set_style_border_width(feed_items[i], hot ? 3 : 1, 0);
        lv_obj_set_style_border_color(feed_items[i],
            hot ? lv_color_white() : lv_color_hex(IDLE_BORDER), 0);
        lv_obj_set_style_bg_opa(feed_items[i], hot ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_text_color(feed_lbls[i],
            lv_color_hex(hot ? TXT_HEX : DIM_HEX), 0);
    }
}

/* ---------------- higher-lower game ---------------- */
static void game_style(void)
{
    for (int i = 0; i < 2; i++) {
        bool hot = (game_sel == i);
        lv_obj_set_style_border_width(game_opts[i], hot ? 3 : 1, 0);
        lv_obj_set_style_border_color(game_opts[i],
            hot ? lv_color_white() : lv_color_hex(IDLE_BORDER), 0);
        lv_obj_set_style_bg_opa(game_opts[i], hot ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_style_text_color(game_opt_lbls[i],
            lv_color_hex(hot ? TXT_HEX : DIM_HEX), 0);
    }
}

static void game_show_round(void)
{
    char b[24];
    snprintf(b, sizeof(b), "ROUND %d/%d",
             game_round < GAME_ROUNDS ? game_round + 1 : GAME_ROUNDS,
             GAME_ROUNDS);
    lv_label_set_text(game_round_lbl, b);
    snprintf(b, sizeof(b), "%d", game_cur);
    lv_label_set_text(game_num, b);
}

static int game_roll(int cur)
{
    /* bounded re-roll (a stuck RNG must not hang the UI task) */
    for (int i = 0; i < 8; i++) {
        int n = (int)(tama_port_random() % 9u) + 1;
        if (n != cur) return n;
    }
    return (cur % 9) + 1;
}

static void game_open(void)
{
    game_phase = GP_PICK;
    game_round = 0;
    game_score = 0;
    game_sel = 1;
    game_cur = (int)(tama_port_random() % 9u) + 1;
    for (int i = 0; i < GAME_ROUNDS; i++)
        lv_obj_set_style_bg_color(game_dots[i], lv_color_hex(IDLE_BORDER), 0);
    show(game_result, false);
    show(game_num, true);
    show(game_round_lbl, true);
    show(game_opts[0], true);
    show(game_opts[1], true);
    lv_obj_set_style_text_color(game_num, lv_color_hex(TXT_HEX), 0);
    game_style();
    game_show_round();
    set_mode(MODE_GAME);
}

static void game_confirm_guess(void)
{
    int next = game_roll(game_cur);
    bool correct = ((next > game_cur) == (game_sel == 1));
    if (correct) game_score++;
    lv_obj_set_style_bg_color(game_dots[game_round],
                              lv_color_hex(correct ? OK_HEX : BAD_HEX), 0);
    game_cur = next;
    game_round++;
    game_phase = GP_REVEAL;
    game_wait = REVEAL_TICKS;
    /* reveal: the new number, tinted by the outcome */
    char b[8];
    snprintf(b, sizeof(b), "%d", next);
    lv_label_set_text(game_num, b);
    lv_obj_set_style_text_color(game_num,
                                lv_color_hex(correct ? OK_HEX : BAD_HEX), 0);
    tama_port_sfx(correct ? TSFX_CONFIRM : TSFX_DENY);
}

static void game_finish(void)
{
    bool win = (game_score >= GAME_WIN_NEED);
    /* apply BEFORE celebrating: bedtime may have passed mid-game, and a
     * WIN! splash over an asleep-refused reward would lie to the player */
    uint32_t ev = do_action(win ? TA_GAME_WIN : TA_GAME_LOSE);
    if (mode != MODE_GAME) return;    /* evolve/death/bedtime took over */
    if (ev & TEV_REFUSED) {
        set_mode(MODE_ROOM);
        return;
    }
    tama_port_sfx(win ? TSFX_WIN : TSFX_LOSE);
    game_phase = GP_RESULT;
    game_wait = RESULT_TICKS;
    show(game_num, false);
    show(game_round_lbl, false);
    show(game_opts[0], false);
    show(game_opts[1], false);
    lv_label_set_text(game_result, win ? "WIN!" : "LOSE");
    lv_obj_set_style_text_color(game_result,
                                lv_color_hex(win ? OK_HEX : BAD_HEX), 0);
    show(game_result, true);
}

static void game_tick(void)
{
    if (game_wait > 0 && --game_wait == 0) {
        if (game_phase == GP_REVEAL) {
            if (game_round >= GAME_ROUNDS) {
                game_finish();
            } else {
                game_phase = GP_PICK;
                lv_obj_set_style_text_color(game_num, lv_color_hex(TXT_HEX), 0);
                game_show_round();
            }
        } else if (game_phase == GP_RESULT) {
            set_mode(MODE_ROOM);
        }
    }
}

/* ---------------- status pages ---------------- */
enum { SP_HUNGER = 0, SP_HAPPY, SP_DISC, SP_BODY, SP_BATT, SP_COUNT };

static void hearts_draw(int val)
{
    if (val > TAMA_MAX_HEARTS) val = TAMA_MAX_HEARTS;
    for (int i = 0; i < TAMA_MAX_HEARTS; i++) {
        buf_fill(heart_bufs[i], 64 * 64, CHROMA_HEX);
        blit_to(heart_bufs[i], 64, 64,
                i < val ? SPR_PROP_HEART_FULL : SPR_PROP_HEART_EMPTY, 0, 0, 0, 4);
        lv_obj_invalidate(stat_hearts[i]);
    }
}

static void stat_render(void)
{
    const tama_state_t *s = &g_state;
    char b[64];
    bool hearts = false, bar = false, big = false, sub = false;

    switch (stat_page) {
    case SP_HUNGER:
        lv_label_set_text(stat_title, "HUNGER");
        hearts_draw(s->hunger);
        hearts = true;
        break;
    case SP_HAPPY:
        lv_label_set_text(stat_title, "HAPPY");
        hearts_draw(s->happy);
        hearts = true;
        break;
    case SP_DISC:
        lv_label_set_text(stat_title, "DISCIPLINE");
        snprintf(b, sizeof(b), "%d%%", s->discipline);
        lv_label_set_text(stat_big, b);
        lv_obj_align(stat_big, LV_ALIGN_CENTER, 0, -25);
        lv_bar_set_value(stat_bar, s->discipline, LV_ANIM_OFF);
        big = bar = true;
        break;
    case SP_BODY:
        lv_label_set_text(stat_title, "WEIGHT & AGE");
        snprintf(b, sizeof(b), "%d lb\n%d yr", s->weight, s->age_days);
        lv_label_set_text(stat_big, b);
        lv_obj_align(stat_big, LV_ALIGN_CENTER, 0, 5);
        big = true;
        break;
    case SP_BATT:
        lv_label_set_text(stat_title, "BATTERY");
        if (g_tama_diag.batt_present) {
            snprintf(b, sizeof(b), "%d%%", g_tama_diag.batt_pct);
            lv_label_set_text(stat_big, b);
            lv_label_set_text(stat_sub,
                g_tama_diag.vbus_in ? "charging (USB)" : "on battery");
        } else {
            /* this unit's pack is dead: powered straight from USB */
            lv_label_set_text(stat_big, "USB");
            lv_label_set_text(stat_sub, "no battery");
        }
        lv_obj_align(stat_big, LV_ALIGN_CENTER, 0, -25);
        big = sub = true;
        break;
    default:
        break;
    }

    for (int i = 0; i < TAMA_MAX_HEARTS; i++) show(stat_hearts[i], hearts);
    show(stat_bar, bar);
    show(stat_big, big);
    show(stat_sub, sub);
    snprintf(b, sizeof(b), "%d/%d", stat_page + 1, SP_COUNT);
    lv_label_set_text(stat_page_lbl, b);

    st_hunger = s->hunger;  st_happy = s->happy;
    st_disc = s->discipline; st_weight = s->weight; st_age = s->age_days;
    st_bpct = g_tama_diag.batt_pct; st_bpres = g_tama_diag.batt_present;
    st_vbus = g_tama_diag.vbus_in;
}

/* re-render only when a displayed value actually moved */
static void stat_tick(void)
{
    const tama_state_t *s = &g_state;
    if (st_hunger != s->hunger || st_happy != s->happy
        || st_disc != s->discipline || st_weight != s->weight
        || st_age != s->age_days || st_bpct != g_tama_diag.batt_pct
        || st_bpres != g_tama_diag.batt_present
        || st_vbus != g_tama_diag.vbus_in)
        stat_render();
}

/* ---------------- clock screen ---------------- */
static void clk_field_style(lv_obj_t *lbl, bool hot)
{
    lv_obj_set_style_bg_opa(lbl, hot ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(lbl, lv_color_hex(ACCENT_HEX), 0);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(hot || clk_phase == CLK_VIEW ? TXT_HEX : DIM_HEX), 0);
}

static void clock_refresh(void)
{
    char b[8];
    lv_label_set_text(clk_title,
        clk_purpose == CLKP_VIEW ? "CLOCK" : "SET CLOCK");
    snprintf(b, sizeof(b), "%02d", clk_h);
    lv_label_set_text(clk_hh, b);
    snprintf(b, sizeof(b), "%02d", clk_m);
    lv_label_set_text(clk_mm, b);
    clk_field_style(clk_hh, clk_phase == CLK_HOUR);
    clk_field_style(clk_mm, clk_phase == CLK_MIN);
    lv_label_set_text(clk_hint,
        clk_phase == CLK_VIEW ? "press to set"
        : clk_phase == CLK_HOUR ? "set hour" : "set minute");
}

static void clock_load_now(void)
{
    uint32_t now = tama_port_now();
    clk_h = (int)((now / 3600u) % 24u);
    clk_m = (int)((now / 60u) % 60u);
    clk_shown_min = (int)((now / 60u) % 1440u);
}

static void clock_open(int purpose)
{
    clk_purpose = purpose;
    clk_phase = (purpose == CLKP_VIEW) ? CLK_VIEW : CLK_HOUR;
    clock_load_now();
    clock_refresh();
    set_mode(MODE_CLOCK);
}

static void clock_apply(void)
{
    uint32_t now = tama_port_now();
    int32_t delta = (int32_t)(clk_h * 3600 + clk_m * 60)
                  - (int32_t)(now % 86400u);

    switch (clk_purpose) {
    case CLKP_VIEW:
        /* rebase the pet FIRST so its pending intervals survive the shift */
        tama_clock_rebase(&g_state, delta, (uint32_t)((int64_t)now + delta));
        tama_port_clock_shift(delta);
        tama_port_save_request();
        tama_port_sfx(TSFX_CONFIRM);
        clk_phase = CLK_VIEW;
        clock_load_now();
        clock_refresh();
        break;
    case CLKP_FIRSTBOOT: {
        tama_port_clock_shift(delta);
        uint32_t seed = tama_port_random();
        tama_new_egg(&g_state, tama_port_now(), seed ? seed : 1u);
        tama_port_save_request();
        tama_port_sfx(TSFX_CONFIRM);
        set_mode(MODE_ROOM);
        break;
    }
    case CLKP_NEWEGG:
        /* P1 authenticity: death -> clock-set -> fresh egg. No rebase of
         * the dead state: TA_NEW_EGG rewrites every field anyway. */
        tama_port_clock_shift(delta);
        tama_port_sfx(TSFX_CONFIRM);
        handle_events(tama_action(&g_state, TA_NEW_EGG, tama_port_now()), true);
        set_mode(MODE_ROOM);
        break;
    }
    last_pet_now = tama_port_now();
}

/* view mode tracks the wall clock live */
static void clock_tick(void)
{
    if (clk_phase != CLK_VIEW) return;
    uint32_t now = tama_port_now();
    int m = (int)((now / 60u) % 1440u);
    if (m != clk_shown_min) {
        clock_load_now();
        clock_refresh();
    }
}

/* ---------------- death screen ---------------- */
static void enter_death(void)
{
    char b[32];
    snprintf(b, sizeof(b), "lived %d days", g_state.age_days);
    lv_label_set_text(death_lbl, b);
    tama_port_sfx(TSFX_DEATH);
    set_mode(MODE_DEAD);
}

/* ---------------- ring icon activation ---------------- */
static void activate_icon(int i)
{
    switch (ICONS[i].spr) {
    case SPR_ICON_FEED:
        feed_sel = 0;
        feed_style();
        set_mode(MODE_FEED);
        tama_port_sfx(TSFX_CONFIRM);
        break;
    case SPR_ICON_LIGHT: {
        uint32_t ev = do_action(TA_LIGHT_TOGGLE);
        if (!(ev & TEV_REFUSED)) tama_port_sfx(TSFX_CONFIRM);
        break;
    }
    case SPR_ICON_GAME:
        /* a sleeping pet (or an egg, or a corpse) won't play */
        if (!state_valid() || g_state.stage == TS_EGG
            || g_state.stage == TS_DEAD || tama_is_asleep(&g_state)) {
            tama_port_sfx(TSFX_DENY);
        } else {
            tama_port_sfx(TSFX_CONFIRM);
            game_open();
        }
        break;
    case SPR_ICON_MEDS: {
        uint32_t ev = do_action(TA_MEDICINE);
        /* dose swallowed but not yet cured: acknowledge quietly */
        if (!(ev & (TEV_REFUSED | TEV_CURED))) tama_port_sfx(TSFX_CONFIRM);
        break;
    }
    case SPR_ICON_WC:
        do_action(TA_CLEAN);   /* clean/deny cues via handle_events */
        break;
    case SPR_ICON_STAT:
        stat_page = 0;
        stat_render();
        set_mode(MODE_STAT);
        tama_port_sfx(TSFX_CONFIRM);
        break;
    case SPR_ICON_SCOLD:
        do_action(TA_SCOLD);   /* scold/deny cues via handle_events */
        break;
    case SPR_ICON_CLOCK:
        tama_port_sfx(TSFX_CONFIRM);
        clock_open(CLKP_VIEW);
        break;
    default:
        break;
    }
}

/* ---------------- build ---------------- */
static lv_obj_t *panel_create(lv_obj_t *scr)
{
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(PANEL_HEX), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t hex, lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hex), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, align, (lv_coord_t)x, (lv_coord_t)y);
    return l;
}

/* selectable option card (feed items, game higher/lower) */
static lv_obj_t *card_create(lv_obj_t *parent, uint32_t hex, int w, int h,
                             int x, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, (lv_coord_t)w, (lv_coord_t)h);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(o, LV_ALIGN_CENTER, (lv_coord_t)x, (lv_coord_t)y);
    return o;
}

void tama_ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(PANEL_HEX), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- room: pet canvas --- */
    canvas_buf = tama_port_big_alloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t));
    canvas_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    canvas_dsc.header.w = CANVAS_W;
    canvas_dsc.header.h = CANVAS_H;
    canvas_dsc.data_size = CANVAS_W * CANVAS_H * sizeof(lv_color_t);
    canvas_dsc.data = (const uint8_t *)canvas_buf;
    buf_fill(canvas_buf, CANVAS_W * CANVAS_H, BG_HEX);
    canvas_img = lv_img_create(scr);
    lv_img_set_src(canvas_img, &canvas_dsc);
    lv_obj_align(canvas_img, LV_ALIGN_CENTER, 0, -8);

    /* --- room: attention bubble, inside the ring between CLOCK and FEED
     * (top-left diagonal, r=146 clears both tiles and the canvas pet) --- */
    attn_img = chroma_img(scr, &attn_dsc, &attn_buf, TILE_SIZE);
    {
        float rad = -112.5f * (float)M_PI / 180.0f;
        int cx = SCREEN_W / 2 + (int)(146.0f * cosf(rad));
        int cy = SCREEN_H / 2 + (int)(146.0f * sinf(rad));
        lv_obj_set_pos(attn_img, cx - TILE_SIZE / 2, cy - TILE_SIZE / 2);
    }
    lv_obj_add_flag(attn_img, LV_OBJ_FLAG_HIDDEN);

    /* --- room: icon ring --- */
    for (int i = 0; i < N_ICONS; i++) {
        tile_objs[i] = lv_obj_create(scr);
        lv_obj_set_size(tile_objs[i], TILE_SIZE, TILE_SIZE);
        lv_obj_set_style_radius(tile_objs[i], 12, 0);
        lv_obj_set_style_pad_all(tile_objs[i], 0, 0);
        lv_obj_clear_flag(tile_objs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(tile_objs[i], LV_OBJ_FLAG_CLICKABLE);
        icon_place(tile_objs[i], i);

        icon_imgs[i] = chroma_img(tile_objs[i], &icon_dscs[i],
                                  &icon_bufs[i], ICON_PX);
        blit_to(icon_bufs[i], ICON_PX, ICON_PX, ICONS[i].spr, 0, 0, 0, 2);
        lv_obj_center(icon_imgs[i]);
        icon_style(i, false);
    }

    /* --- feed panel (created after the ring: sub-screens stack on top) --- */
    feed_scr = panel_create(scr);
    lv_obj_t *feed_title = label_create(feed_scr, F28, TXT_HEX,
                                        LV_ALIGN_CENTER, 0, -118);
    lv_label_set_text(feed_title, "FEED");
    for (int i = 0; i < 2; i++) {
        feed_items[i] = card_create(feed_scr, ICONS[0].color, 116, 116,
                                    i == 0 ? -70 : 70, -4);
        lv_obj_t *img = chroma_img(feed_items[i], &feed_dscs[i],
                                   &feed_bufs[i], 64);
        blit_to(feed_bufs[i], 64, 64,
                i == 0 ? SPR_PROP_MEAL : SPR_PROP_SNACK, 0, 0, 0, 4);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 10);
        feed_lbls[i] = label_create(feed_items[i], F16, TXT_HEX,
                                    LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_label_set_text(feed_lbls[i], i == 0 ? "MEAL" : "SNACK");
    }
    feed_style();

    /* --- game panel --- */
    game_scr = panel_create(scr);
    game_round_lbl = label_create(game_scr, F16, DIM_HEX,
                                  LV_ALIGN_CENTER, 0, -128);
    game_num = label_create(game_scr, F48, TXT_HEX, LV_ALIGN_CENTER, 0, -55);
    for (int i = 0; i < 2; i++) {
        game_opts[i] = card_create(game_scr, ICONS[2].color, 144, 54,
                                   i == 0 ? -78 : 78, 45);
        game_opt_lbls[i] = label_create(game_opts[i], F28, TXT_HEX,
                                        LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(game_opt_lbls[i], i == 0 ? "LOWER" : "HIGHER");
    }
    for (int i = 0; i < GAME_ROUNDS; i++) {
        game_dots[i] = lv_obj_create(game_scr);
        lv_obj_set_size(game_dots[i], 14, 14);
        lv_obj_set_style_radius(game_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(game_dots[i], 0, 0);
        lv_obj_set_style_bg_color(game_dots[i], lv_color_hex(IDLE_BORDER), 0);
        lv_obj_clear_flag(game_dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(game_dots[i], LV_ALIGN_CENTER, (i - 2) * 26, 108);
    }
    game_result = label_create(game_scr, F48, TXT_HEX, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_flag(game_result, LV_OBJ_FLAG_HIDDEN);

    /* --- status panel --- */
    stat_scr = panel_create(scr);
    stat_title = label_create(stat_scr, F28, TXT_HEX, LV_ALIGN_CENTER, 0, -125);
    for (int i = 0; i < TAMA_MAX_HEARTS; i++) {
        stat_hearts[i] = chroma_img(stat_scr, &heart_dscs[i],
                                    &heart_bufs[i], 64);
        lv_obj_align(stat_hearts[i], LV_ALIGN_CENTER,
                     (lv_coord_t)(-105 + i * 70), 5);
    }
    stat_big = label_create(stat_scr, F48, TXT_HEX, LV_ALIGN_CENTER, 0, -25);
    stat_sub = label_create(stat_scr, F28, DIM_HEX, LV_ALIGN_CENTER, 0, 55);
    stat_bar = lv_bar_create(stat_scr);
    lv_obj_set_size(stat_bar, 220, 16);
    lv_obj_align(stat_bar, LV_ALIGN_CENTER, 0, 55);
    lv_bar_set_range(stat_bar, 0, 100);
    lv_obj_set_style_bg_color(stat_bar, lv_color_hex(0x1c222c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stat_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(stat_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stat_bar, lv_color_hex(0x4c86c8), LV_PART_INDICATOR);
    lv_obj_set_style_radius(stat_bar, 8, LV_PART_INDICATOR);
    stat_page_lbl = label_create(stat_scr, F16, DIM_HEX, LV_ALIGN_CENTER, 0, 145);

    /* --- clock panel --- */
    clock_scr = panel_create(scr);
    clk_title = label_create(clock_scr, F28, TXT_HEX, LV_ALIGN_CENTER, 0, -110);
    clk_hh = label_create(clock_scr, F48, TXT_HEX, LV_ALIGN_CENTER, -66, 0);
    clk_colon = label_create(clock_scr, F48, TXT_HEX, LV_ALIGN_CENTER, 0, -4);
    lv_label_set_text(clk_colon, ":");
    clk_mm = label_create(clock_scr, F48, TXT_HEX, LV_ALIGN_CENTER, 66, 0);
    lv_obj_set_style_pad_hor(clk_hh, 8, 0);
    lv_obj_set_style_pad_ver(clk_hh, 4, 0);
    lv_obj_set_style_radius(clk_hh, 8, 0);
    lv_obj_set_style_pad_hor(clk_mm, 8, 0);
    lv_obj_set_style_pad_ver(clk_mm, 4, 0);
    lv_obj_set_style_radius(clk_mm, 8, 0);
    clk_hint = label_create(clock_scr, F16, DIM_HEX, LV_ALIGN_CENTER, 0, 110);

    /* --- death panel --- */
    death_scr = panel_create(scr);
    lv_obj_set_style_bg_color(death_scr, lv_color_hex(BG_DARK_HEX), 0);
    grave_img = chroma_img(death_scr, &grave_dsc, &grave_buf, 144);
    blit_to(grave_buf, 144, 144, SPR_PROP_TOMBSTONE, 0, 0, 0, 6);
    lv_obj_align(grave_img, LV_ALIGN_CENTER, 0, -45);
    death_lbl = label_create(death_scr, F28, TXT_HEX, LV_ALIGN_CENTER, 0, 70);
    death_hint = label_create(death_scr, F16, DIM_HEX, LV_ALIGN_CENTER, 0, 120);
    lv_label_set_text(death_hint, "press");

    room_dirty = true;
}

/* ---------------- start / state ---------------- */
void tama_ui_start(const uint8_t *blob, size_t len)
{
    last_pet_now = tama_port_now();
    if (blob != NULL && tama_deserialize(&g_state, blob, len)) {
        /* seed the evolve-"old" capture: an evolution due on the very
         * first pet-second must not strobe against the EGG default */
        room_spr = pet_sprite(&g_state);
        if (g_state.stage == TS_DEAD) enter_death();
        else set_mode(MODE_ROOM);
    } else {
        /* first boot: the pet is born only after the clock is set */
        memset(&g_state, 0, sizeof(g_state));
        clock_open(CLKP_FIRSTBOOT);
    }
    room_dirty = true;
}

tama_state_t *tama_ui_state(void)
{
    return &g_state;
}

/* ---------------- input ---------------- */
void tama_ui_on_knob(int dir)
{
    switch (mode) {
    case MODE_ROOM: {
        int prev = sel;
        if (sel < 0) sel = dir > 0 ? 0 : N_ICONS - 1;
        else sel = (sel + dir + N_ICONS) % N_ICONS;
        if (prev >= 0) icon_style(prev, false);
        icon_style(sel, true);
        tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        break;
    }
    case MODE_FEED:
        feed_sel ^= 1;
        feed_style();
        tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        break;
    case MODE_GAME:
        if (game_phase == GP_PICK) {
            game_sel = dir > 0 ? 1 : 0;   /* right = HIGHER, left = LOWER */
            game_style();
            tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        }
        break;
    case MODE_STAT:
        stat_page = (stat_page + (dir > 0 ? 1 : -1) + SP_COUNT) % SP_COUNT;
        stat_render();
        tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        break;
    case MODE_CLOCK:
        if (clk_phase == CLK_HOUR) {
            clk_h = (clk_h + dir + 24) % 24;
            clock_refresh();
            tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        } else if (clk_phase == CLK_MIN) {
            clk_m = (clk_m + dir + 60) % 60;
            clock_refresh();
            tama_port_sfx(dir > 0 ? TSFX_TICK_UP : TSFX_TICK_DN);
        }
        break;
    default:   /* evolve cinematic and death ignore the knob */
        break;
    }
}

void tama_ui_on_button(void)
{
    switch (mode) {
    case MODE_ROOM:
        if (sel >= 0) activate_icon(sel);
        break;
    case MODE_FEED: {
        uint32_t ev = do_action(feed_sel == 0 ? TA_FEED_MEAL : TA_FEED_SNACK);
        if (ev & TEV_ATE) {
            eat_spr = feed_sel == 0 ? SPR_PROP_MEAL : SPR_PROP_SNACK;
            eat_ticks = EAT_TICKS;
            if (mode == MODE_FEED) set_mode(MODE_ROOM);
        }
        break;   /* refused: deny already chirped, stay on the menu */
    }
    case MODE_GAME:
        if (game_phase == GP_PICK) game_confirm_guess();
        break;   /* reveal/result phases run on their own timers */
    case MODE_STAT:
        tama_port_sfx(TSFX_BACK);
        set_mode(MODE_ROOM);
        break;
    case MODE_CLOCK:
        if (clk_phase == CLK_VIEW) {
            clk_phase = CLK_HOUR;
            clock_load_now();
            clock_refresh();
            tama_port_sfx(TSFX_CONFIRM);
        } else if (clk_phase == CLK_HOUR) {
            clk_phase = CLK_MIN;
            clock_refresh();
            tama_port_sfx(TSFX_CONFIRM);
        } else {
            clock_apply();
        }
        break;
    case MODE_DEAD:
        tama_port_sfx(TSFX_CONFIRM);
        clock_open(CLKP_NEWEGG);
        break;
    default:   /* evolve cinematic ignores the button */
        break;
    }
}

void tama_ui_on_long_press(void)
{
    switch (mode) {
    case MODE_ROOM:
        if (sel >= 0) { icon_style(sel, false); sel = -1; }
        tama_port_sfx(TSFX_BACK);
        break;
    case MODE_FEED:
    case MODE_STAT:
        tama_port_sfx(TSFX_BACK);
        set_mode(MODE_ROOM);
        break;
    case MODE_GAME:
        if (game_phase != GP_RESULT) {   /* abort: no outcome applied */
            tama_port_sfx(TSFX_BACK);
            set_mode(MODE_ROOM);
        }
        break;
    case MODE_CLOCK:
        if (clk_phase == CLK_VIEW) {
            tama_port_sfx(TSFX_BACK);
            set_mode(MODE_ROOM);
        } else if (clk_purpose == CLKP_VIEW) {
            clk_phase = CLK_VIEW;        /* cancel: discard the edit */
            clock_load_now();
            clock_refresh();
            tama_port_sfx(TSFX_BACK);
        } else {
            /* first-boot / new-egg cannot leave: reset the edit instead */
            clk_phase = CLK_HOUR;
            clock_load_now();
            clock_refresh();
            tama_port_sfx(TSFX_BACK);
        }
        break;
    default:   /* evolve cinematic and death ignore long-press */
        break;
    }
}

/* ---------------- 80 ms tick ---------------- */
void tama_ui_tick(void)
{
    uint32_t prev_frame = anim_cnt / ANIM_DIV;
    anim_cnt++;
    bool frame_flip = (anim_cnt / ANIM_DIV) != prev_frame;

    /* 1) advance the pet whenever the pet-second moved (also catches the
     * jumps a clock shift or a long host pause produce) */
    uint32_t now = tama_port_now();
    if (now != last_pet_now) {
        last_pet_now = now;
        handle_events(tama_tick(&g_state, now), false);
    }

    /* 2) UI-side timers */
    if (eat_ticks > 0 && --eat_ticks == 0) room_dirty = true;
    if (mode == MODE_GAME) game_tick();
    if (mode == MODE_EVOLVE) {
        if (--evolve_ticks <= 0) {
            set_mode(MODE_ROOM);
        } else {
            int e = evolve_ticks / EVOLVE_SWAP;
            if (e != evolve_phase_drawn) {
                evolve_phase_drawn = e;
                evolve_render();
            }
        }
    }

    /* 3) diff-style redraws */
    if (mode == MODE_ROOM) {
        uint32_t sig = room_sig();
        if (sig != room_sig_last) {
            room_sig_last = sig;
            room_dirty = true;
        }
        if (frame_flip || room_dirty) {
            room_dirty = false;
            room_render();
        }
    }
    attn_update();
    if (mode == MODE_STAT) stat_tick();
    if (mode == MODE_CLOCK) clock_tick();

    /* 4) LED mood + backlight policy, applied only on change */
    {
        int led;
        if (!state_valid() || g_state.stage == TS_DEAD) led = TLED_OFF;
        else if (mode == MODE_EVOLVE)                   led = TLED_EVOLVE;
        else if (tama_is_sick(&g_state))                led = TLED_SICK;
        else if (tama_attention(&g_state))              led = TLED_ATTENTION;
        else if (tama_is_asleep(&g_state))              led = TLED_SLEEP;
        else                                            led = TLED_OFF;
        if (led != cur_led) {
            cur_led = led;
            tama_port_led_mood(led);
        }

        int bright = (state_valid() && tama_is_asleep(&g_state)
                      && tama_lights_off(&g_state)) ? 5 : 100;
        if (bright != cur_bright) {
            cur_bright = bright;
            tama_port_brightness(bright);
        }
    }
}
