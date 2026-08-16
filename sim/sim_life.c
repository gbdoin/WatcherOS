/*
 * sim_life.c — headless invariant harness for tama_logic (WatchaGotchi core).
 *
 * Pure libc + tama_logic.h. No LVGL, no FreeRTOS, no ESP-IDF.
 *
 *   Build:  sim/build_life.sh        (links against main/tama_logic.c)
 *   Run:    sim/sim_life [-v]        (-v also logs sickness / care mistakes)
 *
 * Prints "PASS <name>" / "FAIL <name>: <why>" per test and exits nonzero on
 * any failure. The tests assert INVARIANTS of the game rules, not exact
 * tuning numbers — the stage parameter tables are meant to be tuned until
 * this harness passes (see tama_logic.h: "tuned via sim_life").
 *
 * Care-bot framework: each scenario advances a tama_state_t in fixed steps
 * from a fresh egg, with a bot policy reacting to the state after every tick:
 *   perfect       feed when hungry (never during a misbehave call), win the
 *                 game when unhappy, scold misbehaviour, medicine when sick,
 *                 clean poop, lights off at sleep / on at wake.
 *   neglect       does nothing, ever.
 *   snack_spammer only ever feeds snacks whenever happy < 4.
 *   no_discipline like perfect but never scolds.
 *
 * All randomness flows through the seeded xorshift32 in the state, so every
 * scenario is reproducible from the fixed seeds below.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tama_logic.h"

#define DAY_S     86400u
#define SIM_START 32400u /* pet-epoch 09:00 of day 0: early stages in daytime */

/* Fixed seeds — one per scenario, so failures are reproducible. */
#define SEED_PERFECT 0xC0FFEE01u
#define SEED_NEGLECT 0xDEADBEEFu
#define SEED_SNACKER 0x5EEDCAFEu
#define SEED_NODISC  0x0BADD00Du
#define SEED_REFUSE  0x12345679u
#define SEED_CAREWIN 0xA5A5A5A5u
#define SEED_SERIAL  0x0F0F0F0Fu
#define SEED_REBASE  0x77777777u
#define SEED_JUMP    0x13579BDFu

static int g_failures = 0;
static int g_verbose  = 0;

/* First TEV_STATE_DIRTY violation seen anywhere, for the dirty_on_mutation
 * test: any of these events implies a mutation that must be persisted. */
static uint32_t    g_dirty_bad_mask = 0;
static uint32_t    g_dirty_bad_now  = 0;
static const char *g_dirty_bad_bot  = NULL;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *stage_name(uint8_t st)
{
    static const char *n[] = { "EGG", "BABY", "CHILD", "TEEN",
                               "ADULT", "SECRET", "DEAD" };
    return (st <= TS_DEAD) ? n[st] : "?";
}

static const char *species_name(uint8_t st, uint8_t sp)
{
    static const char *teen[]  = { "GOOD", "BAD" };
    static const char *adult[] = { "HERO", "CHEER", "AVG", "GRUMP", "FERAL" };
    if (st == TS_TEEN && sp <= TEEN_BAD)
        return teen[sp];
    if ((st == TS_ADULT || st == TS_SECRET) && sp <= ADULT_FERAL)
        return adult[sp];
    return "-";
}

static void t_pass(const char *name) { printf("PASS %s\n", name); }

static void t_fail(const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("FAIL %s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    g_failures++;
}

/* Fail the current test (a void function) and bail out of it. Message args
 * are only evaluated on failure. */
#define REQUIRE(name, cond, ...)                                            \
    do {                                                                    \
        if (!(cond)) { t_fail((name), __VA_ARGS__); return; }               \
    } while (0)

/* ------------------------------------------------------------------ */
/* run context: one simulated pet plus everything observed about it    */
/* ------------------------------------------------------------------ */

typedef struct {
    tama_state_t s;
    const char  *name;
    uint32_t     start, now;
    /* milestones, pet-epoch seconds (0 = never happened) */
    uint32_t hatched_at, adult_at, secret_at, died_at;
    uint8_t  teen_species, adult_species;
    /* maxima over the whole run */
    uint8_t  max_discipline;
    uint16_t max_weight;
    uint16_t max_over_base;  /* max of (weight - current stage base_weight) */
    uint32_t mistakes_seen;  /* TEV_CARE_MISTAKE occurrences */
    uint32_t ev_or;          /* OR of every event mask seen */
} run_ctx_t;

static double d_days(const run_ctx_t *c, uint32_t t)
{
    return (double)(t - c->start) / (double)DAY_S;
}

/* TAMA_TIME log line: "[bot] d<N> hh:mm <msg>" (pet-epoch days/hours). */
static void logt(const run_ctx_t *c, const char *fmt, ...)
{
    va_list ap;
    printf("  [%-9s] d%u %02u:%02u ", c->name,
           (unsigned)(c->now / DAY_S),
           (unsigned)((c->now / 3600u) % 24u),
           (unsigned)((c->now / 60u) % 60u));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void track_maxima(run_ctx_t *c)
{
    const tama_stage_params_t *p;
    if (c->s.discipline > c->max_discipline)
        c->max_discipline = c->s.discipline;
    if (c->s.weight > c->max_weight)
        c->max_weight = c->s.weight;
    p = tama_stage_params((tama_stage_t)c->s.stage);
    if (p != NULL && c->s.weight > p->base_weight) {
        uint16_t over = (uint16_t)(c->s.weight - p->base_weight);
        if (over > c->max_over_base)
            c->max_over_base = over;
    }
}

static void record(run_ctx_t *c, uint32_t ev)
{
    const uint32_t mutating =
        TEV_ATE | TEV_POOPED | TEV_CLEANED | TEV_GOT_SICK | TEV_CURED |
        TEV_FELL_ASLEEP | TEV_WOKE | TEV_DAY_ROLLOVER | TEV_EVOLVED |
        TEV_DIED | TEV_CARE_MISTAKE | TEV_SCOLDED | TEV_HATCHED;

    c->ev_or |= ev;
    if ((ev & mutating) != 0 && (ev & TEV_STATE_DIRTY) == 0 &&
        g_dirty_bad_bot == NULL) {
        g_dirty_bad_mask = ev;
        g_dirty_bad_now  = c->now;
        g_dirty_bad_bot  = c->name;
    }

    if (ev & TEV_HATCHED) {
        c->hatched_at = c->now;
        logt(c, "hatched -> %s", stage_name(c->s.stage));
    }
    if (ev & TEV_EVOLVED) {
        if (c->s.stage == TS_TEEN)
            c->teen_species = c->s.species;
        if (c->s.stage == TS_ADULT && c->adult_at == 0) {
            c->adult_at      = c->now;
            c->adult_species = c->s.species;
        }
        if (c->s.stage == TS_SECRET && c->secret_at == 0)
            c->secret_at = c->now;
        logt(c, "evolved -> %s/%s (disc=%u care_mistakes=%u weight=%u)",
             stage_name(c->s.stage),
             species_name(c->s.stage, c->s.species),
             (unsigned)c->s.discipline, (unsigned)c->s.care_mistakes,
             (unsigned)c->s.weight);
    }
    if (ev & TEV_DIED) {
        if (c->died_at == 0)
            c->died_at = c->now;
        logt(c, "died (age=%u days, care_mistakes=%u)",
             (unsigned)c->s.age_days, (unsigned)c->s.care_mistakes);
    }
    if (ev & TEV_CARE_MISTAKE) {
        c->mistakes_seen++;
        if (g_verbose)
            logt(c, "care mistake #%u", (unsigned)c->mistakes_seen);
    }
    if (g_verbose) {
        if (ev & TEV_GOT_SICK)
            logt(c, "got sick (doses=%u)",
                 (unsigned)c->s.medicine_doses_left);
        if (ev & TEV_CURED)
            logt(c, "cured");
    }
}

static void ctx_init(run_ctx_t *c, const char *name, uint32_t seed)
{
    memset(c, 0, sizeof *c);
    c->name  = name;
    c->start = SIM_START;
    c->now   = SIM_START;
    tama_new_egg(&c->s, SIM_START, seed);
    track_maxima(c);
}

static uint32_t ctx_tick_to(run_ctx_t *c, uint32_t t)
{
    uint32_t ev;
    c->now = t;
    ev = tama_tick(&c->s, t);
    record(c, ev);
    if (c->s.stage == TS_DEAD && c->died_at == 0)
        c->died_at = t;
    track_maxima(c);
    return ev;
}

static uint32_t do_action(run_ctx_t *c, tama_action_t a)
{
    uint32_t ev = tama_action(&c->s, a, c->now);
    record(c, ev);
    track_maxima(c);
    return ev;
}

/* ------------------------------------------------------------------ */
/* bot policies                                                        */
/* ------------------------------------------------------------------ */

static void apply_policy(run_ctx_t *c, bool feed, bool scold, bool play)
{
    tama_state_t *s = &c->s;
    int g;

    if (s->stage == TS_EGG || s->stage == TS_DEAD)
        return;
    if (tama_is_asleep(s)) {
        if (!tama_lights_off(s))
            do_action(c, TA_LIGHT_TOGGLE); /* lights off at sleep */
        return;
    }
    if (tama_lights_off(s))
        do_action(c, TA_LIGHT_TOGGLE); /* lights on at wake */
    if (scold && (s->flags & TF_MISBEHAVING))
        do_action(c, TA_SCOLD);
    for (g = 0; tama_is_sick(s) && g < 4; g++)
        do_action(c, TA_MEDICINE);
    if (s->poop_count > 0)
        do_action(c, TA_CLEAN);
    if (feed && !(s->flags & TF_MISBEHAVING)) {
        for (g = 0; s->hunger < TAMA_MAX_HEARTS && g < 8; g++)
            if (do_action(c, TA_FEED_MEAL) & TEV_REFUSED)
                break;
    }
    if (play) {
        for (g = 0; s->happy < TAMA_MAX_HEARTS && g < 8; g++)
            if (do_action(c, TA_GAME_WIN) & TEV_REFUSED)
                break;
    }
}

static void bot_perfect(run_ctx_t *c)       { apply_policy(c, true, true, true); }
static void bot_no_discipline(run_ctx_t *c) { apply_policy(c, true, false, true); }
/* care-window helper: full care except feeding */
static void bot_keeper_nofeed(run_ctx_t *c) { apply_policy(c, false, true, true); }

static void bot_snack_spammer(run_ctx_t *c)
{
    tama_state_t *s = &c->s;
    int g;
    if (s->stage == TS_EGG || s->stage == TS_DEAD || tama_is_asleep(s))
        return;
    for (g = 0; s->happy < TAMA_MAX_HEARTS && g < 8; g++)
        if (do_action(c, TA_FEED_SNACK) & TEV_REFUSED)
            break;
}

/* Run a bot from a fresh egg for `days` pet-days in `step`-second steps
 * (stops early on death). */
static void run_bot(run_ctx_t *c, const char *name, uint32_t seed,
                    void (*bot)(run_ctx_t *), double days, uint32_t step)
{
    uint32_t end, t;
    ctx_init(c, name, seed);
    printf("-- %s (seed 0x%08x, %.1f pet-days, %us steps)\n",
           name, (unsigned)seed, days, (unsigned)step);
    end = SIM_START + (uint32_t)(days * (double)DAY_S);
    for (t = SIM_START + step; t <= end; t += step) {
        ctx_tick_to(c, t);
        if (c->s.stage == TS_DEAD)
            break;
        if (bot != NULL)
            bot(c);
    }
    logt(c, "end: %s/%s age=%ud disc(max)=%u weight(max)=%u care_mistakes=%u",
         stage_name(c->s.stage), species_name(c->s.stage, c->s.species),
         (unsigned)c->s.age_days, (unsigned)c->max_discipline,
         (unsigned)c->max_weight, (unsigned)c->s.care_mistakes);
}

/* ------------------------------------------------------------------ */
/* scenario tests                                                      */
/* ------------------------------------------------------------------ */

static void test_perfect_bot(void)
{
    static const char *T = "perfect_bot";
    run_ctx_t c;
    run_bot(&c, "perfect", SEED_PERFECT, bot_perfect, 16.0, 60);

    REQUIRE(T, !(c.died_at != 0 && d_days(&c, c.died_at) < 14.0),
            "died at day %.2f (must never die before day 14)",
            d_days(&c, c.died_at));
    REQUIRE(T, c.adult_at != 0,
            "never reached TS_ADULT (ended %s at day %.2f)",
            stage_name(c.s.stage), d_days(&c, c.now));
    REQUIRE(T, c.adult_species == ADULT_HERO,
            "adult species %s, expected HERO (teen=%s, care mistakes seen=%u)",
            species_name(TS_ADULT, c.adult_species),
            species_name(TS_TEEN, c.teen_species),
            (unsigned)c.mistakes_seen);
    REQUIRE(T, c.max_discipline >= 100,
            "discipline never reached 100 (max %u)",
            (unsigned)c.max_discipline);
    REQUIRE(T, c.secret_at != 0,
            "never evolved to TS_SECRET within 16 days (disc max=%u, "
            "care_mistakes=%u, age=%u)",
            (unsigned)c.max_discipline, (unsigned)c.s.care_mistakes,
            (unsigned)c.s.age_days);
    REQUIRE(T, d_days(&c, c.secret_at) <= 15.0,
            "reached TS_SECRET at day %.2f (expected by day 15)",
            d_days(&c, c.secret_at));
    t_pass(T);
}

static void test_neglect_bot(void)
{
    static const char *T = "neglect_bot";
    run_ctx_t c;
    run_bot(&c, "neglect", SEED_NEGLECT, NULL, 8.0, 60);

    REQUIRE(T, c.died_at != 0,
            "still alive after 8 days of total neglect (%s/%s, "
            "care_mistakes=%u)",
            stage_name(c.s.stage), species_name(c.s.stage, c.s.species),
            (unsigned)c.s.care_mistakes);
    REQUIRE(T, d_days(&c, c.died_at) < 5.0,
            "died at day %.2f, expected well before day 5",
            d_days(&c, c.died_at));
    REQUIRE(T, (c.ev_or & TEV_DIED) != 0,
            "stage is TS_DEAD but TEV_DIED was never reported");
    t_pass(T);
}

static void test_snack_spammer(void)
{
    static const char *T = "snack_spammer";
    run_ctx_t c;
    run_bot(&c, "snacker", SEED_SNACKER, bot_snack_spammer, 8.0, 60);

    REQUIRE(T, c.max_over_base > 20,
            "never became overweight: max weight-over-stage-base %u "
            "(need > 20; max weight %u)",
            (unsigned)c.max_over_base, (unsigned)c.max_weight);
    REQUIRE(T, c.adult_at != 0,
            "never reached TS_ADULT (ended %s at day %.2f)",
            stage_name(c.s.stage),
            d_days(&c, c.died_at != 0 ? c.died_at : c.now));
    REQUIRE(T, c.adult_species == ADULT_GRUMP || c.adult_species == ADULT_FERAL,
            "adult species %s, expected GRUMP or FERAL (teen=%s)",
            species_name(TS_ADULT, c.adult_species),
            species_name(TS_TEEN, c.teen_species));
    t_pass(T);
}

static void test_no_discipline_bot(void)
{
    static const char *T = "no_discipline_bot";
    run_ctx_t c;
    run_bot(&c, "nodisc", SEED_NODISC, bot_no_discipline, 12.0, 60);

    REQUIRE(T, c.adult_at != 0,
            "never reached TS_ADULT (ended %s at day %.2f, died_at day %.2f)",
            stage_name(c.s.stage), d_days(&c, c.now),
            c.died_at != 0 ? d_days(&c, c.died_at) : -1.0);
    REQUIRE(T, !(c.died_at != 0 && d_days(&c, c.died_at) < 10.0),
            "died at day %.2f (care_mistakes=%u); an otherwise perfectly "
            "cared-for pet must survive lack of scolding",
            d_days(&c, c.died_at), (unsigned)c.s.care_mistakes);
    REQUIRE(T, c.max_discipline < 50,
            "discipline reached %u without any scolding (expected < 50)",
            (unsigned)c.max_discipline);
    REQUIRE(T, c.adult_species != ADULT_HERO,
            "became ADULT_HERO despite discipline max %u",
            (unsigned)c.max_discipline);
    t_pass(T);
}

/* ------------------------------------------------------------------ */
/* unit-style invariant tests                                          */
/* ------------------------------------------------------------------ */

static void test_refuse_when_full(void)
{
    static const char *T = "refuse_when_full";
    run_ctx_t c;
    uint32_t t, ev;
    int i;

    printf("-- refuse_when_full (seed 0x%08x)\n", (unsigned)SEED_REFUSE);
    ctx_init(&c, "refuse", SEED_REFUSE);

    /* wait for the egg to hatch */
    for (t = SIM_START + 30; t <= SIM_START + 4u * 3600u; t += 30) {
        ctx_tick_to(&c, t);
        if (c.s.stage != TS_EGG)
            break;
    }
    REQUIRE(T, c.s.stage != TS_EGG, "egg never hatched within 4h");
    REQUIRE(T, c.s.stage != TS_DEAD, "pet died before the test could run");

    /* perfect care until hunger is full */
    for (i = 0; i < 60 && c.s.hunger < TAMA_MAX_HEARTS; i++) {
        t += 30;
        ctx_tick_to(&c, t);
        REQUIRE(T, c.s.stage != TS_DEAD, "pet died while topping up hunger");
        bot_perfect(&c);
    }
    REQUIRE(T, c.s.hunger == TAMA_MAX_HEARTS,
            "could not fill hunger to %u (hunger=%u flags=0x%02x)",
            (unsigned)TAMA_MAX_HEARTS, (unsigned)c.s.hunger,
            (unsigned)c.s.flags);

    /* make sure a misbehave call is not the reason for the refusal */
    if (c.s.flags & TF_MISBEHAVING)
        do_action(&c, TA_SCOLD);
    REQUIRE(T, !(c.s.flags & TF_MISBEHAVING),
            "misbehave call would not clear before the refusal check");

    ev = do_action(&c, TA_FEED_MEAL);
    REQUIRE(T, (ev & TEV_REFUSED) != 0,
            "meal at hunger==4 was not refused (ev=0x%x)", ev);
    REQUIRE(T, (ev & TEV_ATE) == 0,
            "meal at hunger==4 reported TEV_ATE (ev=0x%x)", ev);
    REQUIRE(T, c.s.hunger == TAMA_MAX_HEARTS,
            "hunger changed to %u after refused meal", (unsigned)c.s.hunger);
    t_pass(T);
}

static void test_care_window(void)
{
    static const char *T = "care_window";
    run_ctx_t c;
    uint32_t t, cap, t_att = 0, before;
    uint8_t stage_snap;

    printf("-- care_window (seed 0x%08x)\n", (unsigned)SEED_CAREWIN);
    ctx_init(&c, "care_win", SEED_CAREWIN);

    /* phase 1: perfect care up to CHILD, so the whole test fits inside one
     * long daytime stage */
    t   = SIM_START;
    cap = SIM_START + 6u * 3600u;
    while (t < cap && c.s.stage != TS_CHILD && c.s.stage != TS_DEAD) {
        t += 30;
        ctx_tick_to(&c, t);
        if (c.s.stage != TS_DEAD)
            bot_perfect(&c);
    }
    REQUIRE(T, c.s.stage == TS_CHILD,
            "never reached CHILD within 6h (stage %s); adjust early stage "
            "durations or this test's warmup cap", stage_name(c.s.stage));
    stage_snap = c.s.stage;

    /* phase 2: stop feeding (everything else stays perfect) and wait for the
     * hunger heart to hit 0 -> hungry attention call */
    cap = t + 12u * 3600u;
    while (t < cap) {
        uint32_t ev;
        t += 10;
        ev = ctx_tick_to(&c, t);
        if ((ev & TEV_ATTENTION_ON) != 0 &&
            c.s.attention_kind == TATT_HUNGRY) {
            t_att = t;
            break;
        }
        REQUIRE(T, c.s.stage != TS_DEAD, "died before the hungry attention");
        REQUIRE(T, !tama_is_asleep(&c.s),
                "pet fell asleep before the hungry attention fired");
        bot_keeper_nofeed(&c);
    }
    REQUIRE(T, t_att != 0,
            "hungry attention never fired within 12h without food");
    REQUIRE(T, c.s.stage == stage_snap,
            "stage changed to %s mid-test; lengthen the CHILD stage or "
            "shorten this test", stage_name(c.s.stage));

    /* phase 3: ignore the hungry call past the deadline; everything else
     * stays cared for so no other mistake can fire */
    before = c.mistakes_seen;
    cap    = t_att + (uint32_t)TAMA_CARE_WINDOW_S + 120u;
    while (t < cap) {
        t += 10;
        ctx_tick_to(&c, t);
        REQUIRE(T, c.s.stage != TS_DEAD, "died inside the care window");
        bot_keeper_nofeed(&c);
    }
    REQUIRE(T, c.s.stage == stage_snap,
            "stage changed to %s inside the care window",
            stage_name(c.s.stage));
    REQUIRE(T, c.mistakes_seen - before == 1,
            "expected exactly 1 care mistake within window+2min, got %u",
            (unsigned)(c.mistakes_seen - before));
    t_pass(T);
}

static void test_serialize(void)
{
    static const char *T1 = "serialize_roundtrip";
    static const char *T2 = "serialize_reject_corrupt";
    run_ctx_t c;
    uint8_t b1[sizeof(tama_state_t) + 16];
    uint8_t b2[sizeof(tama_state_t) + 16];
    uint8_t b3[sizeof(tama_state_t) + 16];
    size_t l1 = 0, l2 = 0, l3 = 0;
    tama_state_t s2;

    run_bot(&c, "serialize", SEED_SERIAL, bot_perfect, 2.0, 60);

    do {
        if (!tama_serialize(&c.s, b1, sizeof b1, &l1)) {
            t_fail(T1, "serialize returned false");
            break;
        }
        if (l1 != sizeof(tama_state_t)) {
            t_fail(T1, "serialized length %zu != blob size %zu",
                   l1, sizeof(tama_state_t));
            break;
        }
        memset(&s2, 0xAA, sizeof s2);
        if (!tama_deserialize(&s2, b1, l1)) {
            t_fail(T1, "deserialize rejected a valid blob");
            break;
        }
        if (memcmp(&c.s, &s2, sizeof s2) != 0) {
            t_fail(T1, "state not byte-identical after round-trip");
            break;
        }
        if (!tama_serialize(&s2, b2, sizeof b2, &l2) || l2 != l1 ||
            memcmp(b1, b2, l1) != 0) {
            t_fail(T1, "re-serialized bytes differ from the original blob");
            break;
        }
        t_pass(T1);
    } while (0);

    if (l1 == sizeof(tama_state_t)) {
        do {
            memcpy(b3, b1, l1);
            b3[0] ^= 0x5A; /* corrupt the (little-endian) magic */
            memset(&s2, 0, sizeof s2);
            if (tama_deserialize(&s2, b3, l1)) {
                t_fail(T2, "blob with corrupted magic was accepted");
                break;
            }
            if (tama_deserialize(&s2, b1, l1 - 1)) {
                t_fail(T2, "truncated blob (len-1) was accepted");
                break;
            }
            if (tama_serialize(&c.s, b3, 4, &l3)) {
                t_fail(T2, "serialize into a 4-byte buffer claimed success");
                break;
            }
            t_pass(T2);
        } while (0);
    } else {
        t_fail(T2, "skipped: roundtrip serialization failed first");
    }
}

/* Advance ctx with perfect care (60 s steps) until now falls exactly on the
 * next hh:00:00. Returns false if the pet dies on the way. */
static bool align_to_hour(run_ctx_t *c, uint32_t h)
{
    uint32_t target = (c->now / DAY_S) * DAY_S + h * 3600u;
    while (target <= c->now)
        target += DAY_S;
    while (c->now < target) {
        ctx_tick_to(c, c->now + 60u);
        if (c->s.stage == TS_DEAD)
            return false;
        bot_perfect(c);
    }
    return true;
}

static void rebase_dir(run_ctx_t *c, int32_t delta, const char *T)
{
    const tama_stage_params_t *p;
    tama_state_t base, bl, rb;
    uint32_t w, sl, h, t0, n0, dt;
    uint32_t dt0 = 0, m0 = 0, dt1 = 0, m1 = 0;
    uint32_t ev;

    /* Pick an hour so that both the baseline scan window [h, h+2h] and the
     * post-rebase window [h+delta, h+delta+2h] sit strictly inside the awake
     * hours (sleep transitions are legitimately hour-of-day driven and would
     * pollute the comparison). */
    p = tama_stage_params((tama_stage_t)c->s.stage);
    REQUIRE(T, p != NULL, "no stage params for %s", stage_name(c->s.stage));
    w  = p->wake_hour;
    sl = p->sleep_hour;
    REQUIRE(T, w < sl && w + 9u <= sl,
            "awake span too short for a +/-6h rebase test "
            "(wake=%u sleep=%u; need sleep-wake >= 9)",
            (unsigned)w, (unsigned)sl);
    h = (delta > 0) ? (w + 1u) : (w + 7u);

    REQUIRE(T, align_to_hour(c, h), "pet died while aligning to %02u:00",
            (unsigned)h);

    /* the pet may have evolved during alignment: re-verify with its
     * current stage's hours */
    p = tama_stage_params((tama_stage_t)c->s.stage);
    REQUIRE(T, p != NULL, "no stage params after alignment");
    w  = p->wake_hour;
    sl = p->sleep_hour;
    {
        uint32_t lo = (delta > 0) ? h : (h - 6u);
        uint32_t hi = (delta > 0) ? (h + 8u) : (h + 2u);
        REQUIRE(T, w < sl && lo >= w && hi <= sl,
                "awake window shifted after an evolution "
                "(h=%u wake=%u sleep=%u)",
                (unsigned)h, (unsigned)w, (unsigned)sl);
    }
    REQUIRE(T, !tama_is_asleep(&c->s), "pet asleep at aligned hour %02u:00",
            (unsigned)h);

    t0   = c->now;
    base = c->s;

    /* baseline: seconds from t0 until the next state mutation of any
     * kind (a plain TEV_STATE_DIRTY heart drop counts — teen+ stages go
     * over an hour between "significant" events, and a dirty tick pins
     * the pending interval just as precisely) */
    bl = base;
    for (dt = 1; dt <= 7200; dt++) {
        ev = tama_tick(&bl, t0 + dt);
        if (ev != 0) { dt0 = dt; m0 = ev; break; }
    }
    REQUIRE(T, dt0 != 0,
            "no baseline mutation within the 2h scan cap; raise the cap "
            "here if stage intervals exceed 2h");

    /* rebase and replay: same relative timing expected, and the rebase
     * itself must not make anything due (not even a dirty tick) */
    rb = base;
    n0 = (uint32_t)((int64_t)t0 + (int64_t)delta);
    tama_clock_rebase(&rb, delta, n0);

    ev = tama_tick(&rb, n0);
    REQUIRE(T, ev == 0, "event mask 0x%x fired at the rebase instant", ev);
    for (dt = 1; dt <= 7200; dt++) {
        ev = tama_tick(&rb, n0 + dt);
        if (ev != 0) { dt1 = dt; m1 = ev; break; }
    }
    REQUIRE(T, dt1 != 0,
            "no post-rebase event within 2h (baseline fired at +%us)", dt0);
    REQUIRE(T, !(dt1 <= 1 && dt0 > 1),
            "event 0x%x fired within 1s purely due to the rebase "
            "(baseline was +%us)", m1, dt0);
    REQUIRE(T, dt1 == dt0 && m1 == m0,
            "pending interval not preserved: baseline +%us mask=0x%x, "
            "rebased +%us mask=0x%x", dt0, m0, dt1, m1);
    t_pass(T);
}

static void test_clock_rebase(void)
{
    run_ctx_t c;
    run_bot(&c, "rebase", SEED_REBASE, bot_perfect, 2.0, 60);
    if (c.s.stage == TS_DEAD) {
        t_fail("clock_rebase_plus6h", "pet died during the 2-day warmup");
        t_fail("clock_rebase_minus6h", "pet died during the 2-day warmup");
        return;
    }
    rebase_dir(&c, 6 * 3600, "clock_rebase_plus6h");
    rebase_dir(&c, -6 * 3600, "clock_rebase_minus6h");
}

/* Clock-sets that cross the sleep boundary: forward into the night must put
 * the pet to sleep AT ONCE (P1 time-cheat; regression: the old anchor guard
 * left it awake all night, starving it), and a set back into the previous
 * daytime must leave an awake pet awake with no phantom rollover. */
static void rebase_cross(run_ctx_t *c, uint32_t align_h, int32_t delta,
                         bool expect_asleep, const char *T)
{
    tama_state_t rb;
    uint32_t n0, ev;

    REQUIRE(T, align_to_hour(c, align_h), "pet died aligning to %02u:00",
            (unsigned)align_h);
    REQUIRE(T, !tama_is_asleep(&c->s), "pet unexpectedly asleep at %02u:00",
            (unsigned)align_h);

    rb = c->s;
    n0 = (uint32_t)((int64_t)c->now + (int64_t)delta);
    tama_clock_rebase(&rb, delta, n0);
    ev = tama_tick(&rb, n0);

    if (expect_asleep) {
        REQUIRE(T, (ev & TEV_FELL_ASLEEP) != 0,
                "no TEV_FELL_ASLEEP after setting the clock into the night "
                "(ev=0x%x, asleep=%d)", ev, tama_is_asleep(&rb) ? 1 : 0);
        REQUIRE(T, tama_is_asleep(&rb), "TEV_FELL_ASLEEP but pet not asleep");
        REQUIRE(T, (ev & (TEV_WOKE | TEV_DAY_ROLLOVER | TEV_DIED)) == 0,
                "phantom wake/rollover replayed with the sleep (ev=0x%x)", ev);
    } else {
        REQUIRE(T, !tama_is_asleep(&rb),
                "pet fell asleep after a set landing in daytime (ev=0x%x)", ev);
        REQUIRE(T, (ev & (TEV_FELL_ASLEEP | TEV_WOKE | TEV_DAY_ROLLOVER
                          | TEV_DIED)) == 0,
                "phantom sleep/wake replayed at the set instant (ev=0x%x)", ev);
    }
    t_pass(T);
}

static void test_clock_rebase_cross_bed(void)
{
    run_ctx_t c;
    run_bot(&c, "rebasebed", SEED_REBASE, bot_perfect, 2.0, 60);
    if (c.s.stage == TS_DEAD) {
        t_fail("clock_rebase_into_night", "pet died during the 2-day warmup");
        t_fail("clock_rebase_back_to_day", "pet died during the 2-day warmup");
        return;
    }
    /* teen (sleep 21, wake 9): 10:00 +12h -> 22:00 = into the night */
    rebase_cross(&c, 10, 12 * 3600, true, "clock_rebase_into_night");
    /* fresh warmup: the first cross left the pet asleep mid-"day" */
    run_bot(&c, "rebasebed", SEED_JUMP, bot_perfect, 2.0, 60);
    if (c.s.stage == TS_DEAD) {
        t_fail("clock_rebase_back_to_day", "pet died during the 2nd warmup");
        return;
    }
    /* 19:00 -8h -> 11:00 = back into daytime; anchor (09:00-6h=03:00 wall)
     * lands in the sleep window — the exact phantom-night case */
    rebase_cross(&c, 19, -8 * 3600, false, "clock_rebase_back_to_day");
}

static void test_large_jump(void)
{
    static const char *T = "large_jump_tick";
    tama_state_t a, b;
    uint32_t t, ma = 0, mb, sig;

    printf("-- large_jump_tick (seed 0x%08x, neglected for 24h)\n",
           (unsigned)SEED_JUMP);
    tama_new_egg(&a, SIM_START, SEED_JUMP);
    tama_new_egg(&b, SIM_START, SEED_JUMP);
    REQUIRE(T, memcmp(&a, &b, sizeof a) == 0,
            "identical seeds produced different fresh eggs");

    for (t = SIM_START + 1; t <= SIM_START + DAY_S; t++)
        ma |= tama_tick(&a, t);
    mb = tama_tick(&b, SIM_START + DAY_S);

    sig = TEV_HATCHED | TEV_EVOLVED | TEV_DIED;
    printf("  1Hz : stage=%s species=%u age=%u care_mistakes=%u mask=0x%x\n",
           stage_name(a.stage), (unsigned)a.species, (unsigned)a.age_days,
           (unsigned)a.care_mistakes, ma);
    printf("  jump: stage=%s species=%u age=%u care_mistakes=%u mask=0x%x\n",
           stage_name(b.stage), (unsigned)b.species, (unsigned)b.age_days,
           (unsigned)b.care_mistakes, mb);

    REQUIRE(T, a.stage == b.stage, "stage differs: 1Hz=%s vs jump=%s",
            stage_name(a.stage), stage_name(b.stage));
    REQUIRE(T, a.species == b.species, "species differs: %u vs %u",
            (unsigned)a.species, (unsigned)b.species);
    REQUIRE(T, a.age_days == b.age_days, "age_days differs: %u vs %u",
            (unsigned)a.age_days, (unsigned)b.age_days);
    REQUIRE(T, (ma & sig) == (mb & sig),
            "cumulative hatch/evolve/die events differ: 0x%x vs 0x%x",
            ma & sig, mb & sig);
    t_pass(T);
}

static void test_dirty_flag(void)
{
    static const char *T = "dirty_on_mutation";
    if (g_dirty_bad_bot != NULL) {
        t_fail(T,
               "event mask 0x%x returned without TEV_STATE_DIRTY "
               "([%s] at pet-epoch %u = d%u %02u:%02u)",
               g_dirty_bad_mask, g_dirty_bad_bot, g_dirty_bad_now,
               (unsigned)(g_dirty_bad_now / DAY_S),
               (unsigned)((g_dirty_bad_now / 3600u) % 24u),
               (unsigned)((g_dirty_bad_now / 60u) % 60u));
    } else {
        t_pass(T);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-v") == 0)
            g_verbose = 1;

    printf("sim_life: tama_logic invariant harness (blob %zu bytes)\n",
           sizeof(tama_state_t));

    test_perfect_bot();
    test_neglect_bot();
    test_snack_spammer();
    test_no_discipline_bot();
    test_refuse_when_full();
    test_care_window();
    test_serialize();
    test_clock_rebase();
    test_clock_rebase_cross_bed();
    test_large_jump();
    test_dirty_flag(); /* accumulated across every run above; keep last */

    printf("== %d failure(s)\n", g_failures);
    return g_failures != 0 ? 1 : 0;
}
