/*
 * tama_logic.c — WatchaGotchi game core (original Tamagotchi P1 structure).
 *
 * PURE C99: only tama_logic.h and libc. No ESP-IDF, no LVGL, no FreeRTOS,
 * no malloc. Compiles on the device and on a PC (sim_life tuning harness).
 *
 * Architecture
 * ------------
 * Everything scheduled is an ABSOLUTE pet-epoch stored in the state blob
 * (0 = not scheduled). tama_tick() is a loop over "the earliest due event
 * <= now": it finds the next thing that should have happened, fires it AT
 * ITS OWN EPOCH (handlers reschedule relative to their fire time, never to
 * `now`), and repeats. This makes a 3-week power-off identical to three
 * weeks of 1 Hz ticks: the same events fire in the same causal order, and
 * because all randomness flows through the persisted xorshift32 state, a
 * saved blob replays the exact same life.
 *
 * Sleep/wake transitions are derived, not stored: the anchor is
 * max(last_wake_epoch, stage_epoch) and the stage's sleep/wake hours give
 * the next boundary. This self-corrects across any time jump and needs no
 * extra fields in the blob.
 *
 * Interpretation choices (the header is the contract; these fill its gaps):
 *  - One attention call at a time. When a call clears (met or missed) we
 *    immediately re-check other unmet needs, so pressure is never lost;
 *    a missed call re-raises itself (repeat calls, like the original).
 *  - Bedtime forgives open calls without a mistake (hearts freeze all
 *    night anyway); the only nighttime mistake is leaving the lights on.
 *  - Two kinds of mistake: NEGLECT (an empty need or the lights left on
 *    past the window) counts for life — lifetime neglect is what kills an
 *    adult and shortens old age. An IGNORED DISCIPLINE call shapes only
 *    the evolution branch (stage_mistakes); nobody dies of bad manners.
 *  - Sickness: adults roll the stage's one random illness each morning
 *    wake (1 in 8). Younger pets only fall ill from filth (poop) or
 *    outright neglect — a mistake landing while EVERY need is empty
 *    (1 in 4). Rolling at event points keeps replays reproducible.
 *  - Sick pets still eat (the contract says snacks are always accepted);
 *    medicine works even while asleep so a night illness is treatable.
 *  - Old age is a morning roll with rising odds past a natural lifespan
 *    that care determines: a spotless life reaches the secret threshold
 *    (16 days), a decent one 14, a sloppy one fades from day 12.
 */

#include <string.h>

#include "tama_logic.h"

/* ------------------------------------------------------------------ */
/* tunables local to the core                                          */
/* ------------------------------------------------------------------ */

#define SECS_PER_HOUR      3600u
#define SECS_PER_DAY       86400u

/* digestion: a meal produces a poop 10..90 min later */
#define POOP_MIN_DELAY_S   (10u * 60u)
#define POOP_MAX_DELAY_S   (90u * 60u)
/* a poop left on screen this long forces sickness */
#define POOP_ROT_S         (2u * 3600u)
/* an overnight-held poop drops again shortly after waking */
#define MORNING_POOP_MIN_S 60u
#define MORNING_POOP_MAX_S 600u

/* discipline calls: ~2-4x per ~12 h waking day -> one every 3..6 h */
#define MISBEHAVE_MIN_S    (3u * 3600u)
#define MISBEHAVE_MAX_S    (6u * 3600u)
/* if the pet isn't in a state to act out, try again a bit later */
#define MISBEHAVE_RETRY_S  (30u * 60u)

#define SCOLD_DISCIPLINE   25u
#define DISCIPLINE_MAX     100u
#define WEIGHT_MAX         99u        /* classic 2-digit scale display */

/* lights-on grace after falling asleep before it counts as a mistake */
#define LIGHTS_GRACE_S     TAMA_CARE_WINDOW_S

/* old age: morning death roll starts at this age, odds rise each day */
#define OLD_AGE_DAYS       12u
#define SECRET_AGE_DAYS    14u

/* hearts a fresh hatchling starts with (needy, but not calling yet) */
#define HATCH_HEARTS       2u

/* safety net for the tick loop; real worst cases are ~100 events/day */
#define TICK_MAX_EVENTS    1000000u

#define RNG_FALLBACK_SEED  0x6C078965u

/* ------------------------------------------------------------------ */
/* stage parameter table                                               */
/* ------------------------------------------------------------------ */
/* Heart-decay intervals lengthen as the pet matures: babies need
 * near-constant care, adults can be left alone for over an hour.
 * sleep_hour == wake_hour means the stage has no sleep cycle (egg/dead).
 * NOTE: TS_SECRET keeps wake_hour 9 (same as adult) — the secret
 * evolution happens inside the wake handler, and a later wake hour would
 * put the fresh anchor back inside the sleep window and double-fire the
 * morning rollover. */
static const tama_stage_params_t k_stage_params[TS_DEAD + 1] = {
    [TS_EGG] = {
        .hunger_interval_s10 = 0,   .happy_interval_s10 = 0,
        .sleep_hour = 0,            .wake_hour = 0,
        .stage_duration_s = 5u * 60u,
        .base_weight = 1,           .min_weight = 1,
    },
    [TS_BABY] = {
        .hunger_interval_s10 = 24,  .happy_interval_s10 = 30,   /* 4 / 5 min */
        .sleep_hour = 20,           .wake_hour = 9,
        .stage_duration_s = 65u * 60u,
        .base_weight = 5,           .min_weight = 5,
    },
    [TS_CHILD] = {
        .hunger_interval_s10 = 360, .happy_interval_s10 = 420,  /* 60 / 70 min */
        .sleep_hour = 20,           .wake_hour = 9,
        .stage_duration_s = 24u * 3600u,
        .base_weight = 10,          .min_weight = 10,
    },
    [TS_TEEN] = {
        .hunger_interval_s10 = 420, .happy_interval_s10 = 480,  /* 70 / 80 min */
        .sleep_hour = 21,           .wake_hour = 9,
        .stage_duration_s = 48u * 3600u,
        .base_weight = 20,          .min_weight = 15,
    },
    [TS_ADULT] = {
        .hunger_interval_s10 = 480, .happy_interval_s10 = 540,  /* 80 / 90 min */
        .sleep_hour = 22,           .wake_hour = 9,
        .stage_duration_s = 0,      /* terminal: lifespan is the old-age roll */
        .base_weight = 30,          .min_weight = 20,
    },
    [TS_SECRET] = {
        .hunger_interval_s10 = 600, .happy_interval_s10 = 600,  /* 100 min */
        .sleep_hour = 23,           .wake_hour = 9,
        .stage_duration_s = 0,
        .base_weight = 25,          .min_weight = 20,
    },
    [TS_DEAD] = {
        .hunger_interval_s10 = 0,   .happy_interval_s10 = 0,
        .sleep_hour = 0,            .wake_hour = 0,
        .stage_duration_s = 0,
        .base_weight = 0,           .min_weight = 0,
    },
};

const tama_stage_params_t *tama_stage_params(tama_stage_t stage)
{
    unsigned idx = (unsigned)stage;
    if (idx > TS_DEAD) idx = TS_DEAD;
    return &k_stage_params[idx];
}

static const tama_stage_params_t *cur_params(const tama_state_t *s)
{
    return tama_stage_params((tama_stage_t)s->stage);
}

/* ------------------------------------------------------------------ */
/* RNG: xorshift32 on the persisted state                              */
/* ------------------------------------------------------------------ */
/* The state struct is packed, so we never take the address of a member
 * (clang's -Waddress-of-packed-member is fatal under -Werror): the RNG
 * reads and writes s->rng_state by value. */
static uint32_t rng_next(tama_state_t *s)
{
    uint32_t x = s->rng_state;
    if (x == 0) x = RNG_FALLBACK_SEED;   /* xorshift32 must never be 0 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng_state = x;
    return x;
}

/* uniform-ish integer in [lo, hi]; spans here are small so modulo bias
 * is irrelevant for gameplay */
static uint32_t rng_range(tama_state_t *s, uint32_t lo, uint32_t hi)
{
    return lo + rng_next(s) % (hi - lo + 1u);
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void sat_inc_u8(uint8_t *v)
{
    if (*v < 0xFFu) (*v)++;
}

static void add_weight(tama_state_t *s, uint16_t lbs)
{
    uint32_t w = (uint32_t)s->weight + lbs;
    s->weight = (w > WEIGHT_MAX) ? (uint16_t)WEIGHT_MAX : (uint16_t)w;
}

static bool is_asleep(const tama_state_t *s)
{
    return (s->flags & TF_ASLEEP) != 0;
}

static bool stage_sleeps(const tama_stage_params_t *p)
{
    return p->sleep_hour != p->wake_hour;
}

/* Is wall-hour(t) inside the stage's sleep window? Handles the usual
 * wrap-around window (e.g. 20:00 -> 09:00). */
static bool in_sleep_hours(uint32_t t, const tama_stage_params_t *p)
{
    int h = tama_hour_of_day(t);
    if (p->sleep_hour < p->wake_hour)
        return h >= p->sleep_hour && h < p->wake_hour;
    return h >= p->sleep_hour || h < p->wake_hour;
}

/* first epoch strictly after `after` whose wall-hour is `hour`:00:00 */
static uint32_t next_hour_epoch(uint32_t after, uint8_t hour)
{
    uint32_t e = (after / SECS_PER_DAY) * SECS_PER_DAY
               + (uint32_t)hour * SECS_PER_HOUR;
    while (e <= after) e += SECS_PER_DAY;
    return e;
}

/* Sleep/wake anchor: the later of "last woke up" and "entered this
 * stage". A pet that hatches or evolves in the middle of the night must
 * go to sleep from that moment, not from a boundary it never lived. */
static uint32_t sched_anchor(const tama_state_t *s)
{
    return (s->last_wake_epoch > s->stage_epoch) ? s->last_wake_epoch
                                                 : s->stage_epoch;
}

/* ------------------------------------------------------------------ */
/* heart-decay scheduling                                              */
/* ------------------------------------------------------------------ */
/* A drop is only scheduled while the stat has hearts left; at 0 the
 * schedule slot is cleared (the attention call carries the pressure). */
static void sched_hunger(tama_state_t *s, uint32_t t)
{
    const tama_stage_params_t *p = cur_params(s);
    s->next_hunger_drop = (s->hunger > 0 && p->hunger_interval_s10 > 0)
                        ? t + (uint32_t)p->hunger_interval_s10 * 10u
                        : 0;
}

static void sched_happy(tama_state_t *s, uint32_t t)
{
    const tama_stage_params_t *p = cur_params(s);
    s->next_happy_drop = (s->happy > 0 && p->happy_interval_s10 > 0)
                       ? t + (uint32_t)p->happy_interval_s10 * 10u
                       : 0;
}

/* ------------------------------------------------------------------ */
/* attention call management (one call at a time)                      */
/* ------------------------------------------------------------------ */

static void raise_attention(tama_state_t *s, uint8_t kind, uint32_t t,
                            uint32_t *ev)
{
    if (s->flags & TF_ATTENTION) return;   /* a call is already up */
    s->flags |= TF_ATTENTION;
    if (kind == TATT_MISBEHAVE) s->flags |= TF_MISBEHAVING;
    s->attention_kind = kind;
    s->attention_deadline = t + TAMA_CARE_WINDOW_S;
    *ev |= TEV_ATTENTION_ON | TEV_STATE_DIRTY;
}

static void clear_attention(tama_state_t *s, uint32_t *ev)
{
    if (!(s->flags & TF_ATTENTION)) return;
    s->flags &= (uint8_t)~(TF_ATTENTION | TF_MISBEHAVING);
    s->attention_kind = TATT_NONE;
    s->attention_deadline = 0;
    *ev |= TEV_ATTENTION_OFF | TEV_STATE_DIRTY;
}

/* With a single call slot, a second empty heart goes unadvertised until
 * the slot frees up. Whenever a call ends, look again so no unmet need
 * ever silently stops asking. Awake-gated: needs are frozen overnight. */
static void recheck_attention(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    if (s->flags & (TF_ATTENTION | TF_ASLEEP)) return;
    if (s->stage < TS_BABY || s->stage > TS_SECRET) return;
    if (s->hunger == 0)
        raise_attention(s, TATT_HUNGRY, t, ev);
    else if (s->happy == 0)
        raise_attention(s, TATT_UNHAPPY, t, ev);
}

/* ------------------------------------------------------------------ */
/* sickness / death / mistakes                                         */
/* ------------------------------------------------------------------ */

static void die(tama_state_t *s, uint32_t *ev)
{
    s->stage = TS_DEAD;
    s->flags = 0;
    s->attention_kind = TATT_NONE;
    s->medicine_doses_left = 0;
    s->next_hunger_drop = 0;
    s->next_happy_drop = 0;
    s->next_poop = 0;
    s->oldest_poop_epoch = 0;
    s->next_misbehave = 0;
    s->attention_deadline = 0;
    s->sick_death_deadline = 0;
    s->evolve_epoch = 0;
    *ev |= TEV_DIED | TEV_STATE_DIRTY;
}

/* poop_induced sickness bypasses the once-per-stage limit: filth always
 * makes the pet ill, no matter how often. */
static void make_sick(tama_state_t *s, uint32_t t, bool poop_induced,
                      uint32_t *ev)
{
    if (s->flags & TF_SICK) return;
    s->flags |= TF_SICK;
    if (!poop_induced) s->flags |= TF_SICK_THIS_STAGE;
    s->medicine_doses_left = (uint8_t)(1u + (rng_next(s) & 1u));
    s->sick_death_deadline = t + TAMA_SICK_DEATH_S;
    *ev |= TEV_GOT_SICK | TEV_STATE_DIRTY;
}

/* `neglect` marks a mistake of physical care (empty need, lights on all
 * night): those count for life. An ignored discipline call only shapes
 * the evolution branch — nobody dies of bad manners. */
static void care_mistake(tama_state_t *s, uint32_t t, bool neglect,
                         uint32_t *ev)
{
    if (neglect)                     /* plain uint8_t locals-of-struct:  */
        sat_inc_u8(&s->care_mistakes);
    sat_inc_u8(&s->stage_mistakes);  /* single-byte members are align-1, */
                                     /* safe to address even when packed */
    *ev |= TEV_CARE_MISTAKE | TEV_STATE_DIRTY;

    /* only outright neglect — every need empty at once — can bring on
     * the young pet's illness (daytime only: the only overnight mistake
     * is lights-on, and waking the user with a sickness they cannot see
     * would be unfair) */
    if (neglect && s->hunger == 0 && s->happy == 0
        && !(s->flags & (TF_ASLEEP | TF_SICK | TF_SICK_THIS_STAGE))
        && s->stage >= TS_BABY && s->stage <= TS_SECRET
        && (rng_next(s) & 3u) == 0) {
        make_sick(s, t, false, ev);
    }

    /* adults die of accumulated lifetime neglect */
    if ((s->stage == TS_ADULT || s->stage == TS_SECRET)
        && s->care_mistakes >= TAMA_DEATH_MISTAKES) {
        die(s, ev);
    }
}

/* ------------------------------------------------------------------ */
/* evolution                                                           */
/* ------------------------------------------------------------------ */

static uint8_t pick_adult_species(uint8_t teen_species, uint8_t mistakes,
                                  uint8_t discipline)
{
    if (teen_species == TEEN_GOOD) {
        if (mistakes <= 1 && discipline >= 75) return ADULT_HERO;
        if (mistakes <= 3) return ADULT_CHEER;
        return ADULT_AVG;
    }
    if (mistakes <= 2) return ADULT_AVG;
    if (mistakes <= 4) return ADULT_GRUMP;
    return ADULT_FERAL;
}

/* Common stage-entry bookkeeping. Weight resets to the species base
 * (each form has its own body); hearts and discipline carry over. */
static void enter_stage(tama_state_t *s, uint8_t stage, uint8_t species,
                        uint32_t t)
{
    const tama_stage_params_t *p;

    s->stage = stage;
    s->species = species;
    s->stage_epoch = t;
    s->stage_mistakes = 0;
    s->flags &= (uint8_t)~TF_SICK_THIS_STAGE;  /* fresh stage, fresh illness */

    p = cur_params(s);
    s->weight = p->base_weight;
    s->evolve_epoch = (p->stage_duration_s != 0) ? t + p->stage_duration_s : 0;

    sched_hunger(s, t);
    sched_happy(s, t);

    /* discipline calls start at child age */
    if (stage >= TS_CHILD && stage <= TS_SECRET) {
        if (s->next_misbehave == 0)
            s->next_misbehave = t + rng_range(s, MISBEHAVE_MIN_S,
                                                 MISBEHAVE_MAX_S);
    } else {
        s->next_misbehave = 0;
    }
}

static void on_evolve(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    switch (s->stage) {
    case TS_EGG:
        enter_stage(s, TS_BABY, 0, t);
        /* hatchlings start needy but not yet calling */
        s->hunger = HATCH_HEARTS;
        s->happy = HATCH_HEARTS;
        sched_hunger(s, t);
        sched_happy(s, t);
        *ev |= TEV_EVOLVED | TEV_HATCHED | TEV_STATE_DIRTY;
        return;
    case TS_BABY:
        enter_stage(s, TS_CHILD, 0, t);
        break;
    case TS_CHILD:
        /* good care in childhood decides the teen branch */
        enter_stage(s, TS_TEEN,
                    (s->stage_mistakes <= 2) ? TEEN_GOOD : TEEN_BAD, t);
        break;
    case TS_TEEN:
        enter_stage(s, TS_ADULT,
                    pick_adult_species(s->species, s->stage_mistakes,
                                       s->discipline), t);
        break;
    default:
        /* terminal stage with a stray evolve epoch: disarm it */
        s->evolve_epoch = 0;
        *ev |= TEV_STATE_DIRTY;
        return;
    }
    *ev |= TEV_EVOLVED | TEV_STATE_DIRTY;
}

/* ------------------------------------------------------------------ */
/* sleep / wake                                                        */
/* ------------------------------------------------------------------ */

static void on_fall_asleep(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    /* Bedtime forgives an open call without a mistake: needs freeze all
     * night, so carrying a 15-min deadline into 13 h of sleep would turn
     * one lapse into an unavoidable failure. */
    clear_attention(s, ev);

    s->flags |= TF_ASLEEP;
    *ev |= TEV_FELL_ASLEEP | TEV_STATE_DIRTY;

    /* ask for the lights; the call's deadline doubles as the lights-on
     * mistake check (one per night, guarded by TF_NIGHT_MISTAKE) */
    if (!(s->flags & TF_LIGHTS_OFF))
        raise_attention(s, TATT_SLEEPY, t, ev);
}

static void on_wake(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    s->flags &= (uint8_t)~(TF_ASLEEP | TF_LIGHTS_OFF | TF_NIGHT_MISTAKE);
    if (s->attention_kind == TATT_SLEEPY)
        clear_attention(s, ev);      /* stale lights call, if any */

    sat_inc_u8(&s->age_days);
    s->last_wake_epoch = t;
    *ev |= TEV_WOKE | TEV_DAY_ROLLOVER | TEV_STATE_DIRTY;

    /* the secret evolution happens quietly one morning: a perfect hero */
    if (s->stage == TS_ADULT && s->species == ADULT_HERO
        && s->discipline >= DISCIPLINE_MAX
        && s->stage_mistakes == 0
        && s->age_days >= SECRET_AGE_DAYS) {
        enter_stage(s, TS_SECRET, 0, t);
        *ev |= TEV_EVOLVED;
    }

    /* restart everything the night froze, from the wake instant */
    sched_hunger(s, t);
    sched_happy(s, t);
    if (s->next_poop != 0)           /* held it in all night */
        s->next_poop = t + rng_range(s, MORNING_POOP_MIN_S,
                                        MORNING_POOP_MAX_S);
    if (s->poop_count > 0)           /* rot clock also froze overnight —
                                      * never backdate a sickness chain */
        s->oldest_poop_epoch = t;
    if (s->stage >= TS_CHILD && s->stage <= TS_SECRET)
        s->next_misbehave = t + rng_range(s, MISBEHAVE_MIN_S,
                                             MISBEHAVE_MAX_S);
    else
        s->next_misbehave = 0;

    /* a need left empty at bedtime starts calling again at once */
    recheck_attention(s, t, ev);

    /* morning roll: the adult stage's one random sickness (younger pets
     * only fall ill from filth or outright neglect — see care_mistake) */
    if (!(s->flags & (TF_SICK | TF_SICK_THIS_STAGE))
        && (s->stage == TS_ADULT || s->stage == TS_SECRET)
        && (rng_next(s) & 7u) == 0) {
        make_sick(s, t, false, ev);
    }

    /* morning roll: old age. Odds climb each day past a natural lifespan
     * that care determines — a spotless life reaches the secret threshold,
     * a sloppy one fades from OLD_AGE_DAYS. */
    if (s->stage == TS_ADULT || s->stage == TS_SECRET) {
        uint32_t life = OLD_AGE_DAYS;
        if (s->care_mistakes == 0)      life = OLD_AGE_DAYS + 4u;
        else if (s->care_mistakes <= 2) life = OLD_AGE_DAYS + 2u;
        if (s->age_days >= life) {
            uint32_t over = (uint32_t)s->age_days - (life - 1u);
            if (rng_next(s) % 6u < over)
                die(s, ev);
        }
    }
}

/* ------------------------------------------------------------------ */
/* timed-event handlers                                                */
/* ------------------------------------------------------------------ */

static void on_hunger_drop(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    if (s->hunger > 0) s->hunger--;
    *ev |= TEV_STATE_DIRTY;
    if (s->hunger == 0) {
        s->next_hunger_drop = 0;     /* the call carries the pressure now */
        *ev |= TEV_HEART_EMPTY;
        raise_attention(s, TATT_HUNGRY, t, ev);
    } else {
        sched_hunger(s, t);
    }
}

static void on_happy_drop(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    if (s->happy > 0) s->happy--;
    *ev |= TEV_STATE_DIRTY;
    if (s->happy == 0) {
        s->next_happy_drop = 0;
        *ev |= TEV_HEART_EMPTY;
        raise_attention(s, TATT_UNHAPPY, t, ev);
    } else {
        sched_happy(s, t);
    }
}

static void on_poop(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    s->next_poop = 0;
    if (s->poop_count < TAMA_MAX_POOP) s->poop_count++;
    if (s->poop_count == 1) s->oldest_poop_epoch = t;
    *ev |= TEV_POOPED | TEV_STATE_DIRTY;

    /* a screen full of filth makes the pet ill immediately
     * (edge-triggered here so a cured-but-uncleaned pet doesn't loop
     * inside a single tick) */
    if (s->poop_count >= TAMA_MAX_POOP)
        make_sick(s, t, true, ev);
}

static void on_poop_rot(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    /* restart the episode clock: one sickness per stale-poop episode,
     * and the handler always makes progress for the tick loop */
    s->oldest_poop_epoch = t;
    make_sick(s, t, true, ev);
}

static void on_misbehave_due(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    /* the pet only acts out when it is otherwise content: awake
     * (guaranteed by the scan), healthy, and not already calling */
    if (s->stage >= TS_CHILD && s->stage <= TS_SECRET
        && !(s->flags & (TF_SICK | TF_ATTENTION))) {
        raise_attention(s, TATT_MISBEHAVE, t, ev);
        s->next_misbehave = t + rng_range(s, MISBEHAVE_MIN_S,
                                             MISBEHAVE_MAX_S);
    } else {
        s->next_misbehave = t + MISBEHAVE_RETRY_S;
    }
    *ev |= TEV_STATE_DIRTY;
}

static void on_attention_deadline(tama_state_t *s, uint32_t t, uint32_t *ev)
{
    uint8_t kind = s->attention_kind;
    clear_attention(s, ev);

    if (kind == TATT_SLEEPY) {
        /* the lights call expiring with lights still on is the one
         * nightly mistake; if they're off this is just a stale call */
        if (!(s->flags & TF_LIGHTS_OFF) && !(s->flags & TF_NIGHT_MISTAKE)) {
            s->flags |= TF_NIGHT_MISTAKE;
            care_mistake(s, t, true, ev);
        }
        return;
    }

    care_mistake(s, t, kind != TATT_MISBEHAVE, ev);
    if (s->stage != TS_DEAD)
        recheck_attention(s, t, ev);   /* unmet needs keep calling */
}

/* ------------------------------------------------------------------ */
/* the "next due event" scan                                           */
/* ------------------------------------------------------------------ */
/* Invariant: a candidate is offered exactly when its handler would
 * change state, and every handler advances or zeroes its own epoch at
 * fire time — so the loop always makes progress. */

typedef enum {
    DUE_NONE = 0,
    DUE_WAKE,          /* tie priority: transitions first, so a wake at */
    DUE_SLEEP,         /* the same instant as an evolve lands pre-evolve */
    DUE_EVOLVE,
    DUE_SICK_DEATH,
    DUE_ATTENTION,
    DUE_HUNGER,
    DUE_HAPPY,
    DUE_POOP,
    DUE_POOP_ROT,
    DUE_MISBEHAVE,
} due_kind_t;

static void consider(uint32_t epoch, due_kind_t kind, uint32_t now,
                     uint32_t *best_when, due_kind_t *best_kind)
{
    if (epoch == 0 || epoch > now) return;      /* unscheduled / not due */
    if (*best_kind == DUE_NONE || epoch < *best_when) {  /* ties: first  */
        *best_when = epoch;                              /* offer wins   */
        *best_kind = kind;
    }
}

static due_kind_t next_due(const tama_state_t *s, uint32_t now,
                           uint32_t *when)
{
    const tama_stage_params_t *p = cur_params(s);
    due_kind_t kind = DUE_NONE;
    bool asleep = is_asleep(s);

    *when = 0;

    if (stage_sleeps(p)) {
        uint32_t anchor = sched_anchor(s);
        if (asleep) {
            consider(next_hour_epoch(anchor, p->wake_hour), DUE_WAKE,
                     now, when, &kind);
        } else {
            /* an anchor already inside the sleep window (hatched or
             * evolved at night) means sleep is due immediately */
            uint32_t due = in_sleep_hours(anchor, p)
                         ? anchor
                         : next_hour_epoch(anchor, p->sleep_hour);
            consider(due, DUE_SLEEP, now, when, &kind);
        }
    }

    consider(s->evolve_epoch, DUE_EVOLVE, now, when, &kind);

    if (s->flags & TF_SICK)
        consider(s->sick_death_deadline, DUE_SICK_DEATH, now, when, &kind);
    if (s->flags & TF_ATTENTION)
        consider(s->attention_deadline, DUE_ATTENTION, now, when, &kind);

    /* the night freezes body and behavior: no decay, digestion, rot or
     * mischief until morning (the wake handler restarts them all) */
    if (!asleep) {
        consider(s->next_hunger_drop, DUE_HUNGER, now, when, &kind);
        consider(s->next_happy_drop, DUE_HAPPY, now, when, &kind);
        consider(s->next_poop, DUE_POOP, now, when, &kind);
        if (s->poop_count > 0 && s->oldest_poop_epoch != 0
            && !(s->flags & TF_SICK))
            consider(s->oldest_poop_epoch + POOP_ROT_S, DUE_POOP_ROT,
                     now, when, &kind);
        consider(s->next_misbehave, DUE_MISBEHAVE, now, when, &kind);
    }

    return kind;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

void tama_new_egg(tama_state_t *s, uint32_t now, uint32_t seed)
{
    if (s == NULL) return;
    memset(s, 0, sizeof *s);
    s->magic = TAMA_MAGIC;
    s->version = TAMA_VERSION;
    s->stage = TS_EGG;
    s->species = 0;
    s->born_epoch = now;
    s->stage_epoch = now;
    s->last_wake_epoch = now;
    s->weight = k_stage_params[TS_EGG].base_weight;
    s->evolve_epoch = now + k_stage_params[TS_EGG].stage_duration_s;
    s->attention_kind = TATT_NONE;
    s->rng_state = (seed != 0) ? seed : RNG_FALLBACK_SEED;
}

uint32_t tama_tick(tama_state_t *s, uint32_t now)
{
    uint32_t ev = 0;
    uint32_t guard;

    if (s == NULL || s->magic != TAMA_MAGIC || s->stage > TS_DEAD)
        return 0;
    if (s->stage == TS_DEAD)
        return 0;   /* the dead are beyond time */

    /* replay everything due, earliest first, each at its own instant */
    for (guard = 0; guard < TICK_MAX_EVENTS; guard++) {
        uint32_t t = 0;
        due_kind_t kind = next_due(s, now, &t);
        if (kind == DUE_NONE) break;

        switch (kind) {
        case DUE_WAKE:       on_wake(s, t, &ev);               break;
        case DUE_SLEEP:      on_fall_asleep(s, t, &ev);        break;
        case DUE_EVOLVE:     on_evolve(s, t, &ev);             break;
        case DUE_SICK_DEATH: die(s, &ev);                      break;
        case DUE_ATTENTION:  on_attention_deadline(s, t, &ev); break;
        case DUE_HUNGER:     on_hunger_drop(s, t, &ev);        break;
        case DUE_HAPPY:      on_happy_drop(s, t, &ev);         break;
        case DUE_POOP:       on_poop(s, t, &ev);               break;
        case DUE_POOP_ROT:   on_poop_rot(s, t, &ev);           break;
        case DUE_MISBEHAVE:  on_misbehave_due(s, t, &ev);      break;
        default: /* unreachable */                             break;
        }

        if (s->stage == TS_DEAD) break;
    }

    return ev;
}

uint32_t tama_action(tama_state_t *s, tama_action_t a, uint32_t now)
{
    uint32_t ev;
    bool asleep;

    if (s == NULL || s->magic != TAMA_MAGIC || s->stage > TS_DEAD)
        return 0;

    /* catch up first so the action lands in the correct present (a scold
     * after the window really is too late, a due death really happened) */
    ev = tama_tick(s, now);

    if (s->stage == TS_DEAD) {
        if (a == TA_NEW_EGG) {
            /* carry the RNG lineage into the next life so a saved run
             * stays reproducible across generations */
            uint32_t seed = rng_next(s);
            tama_new_egg(s, now, seed);
            return ev | TEV_STATE_DIRTY;
        }
        return ev;   /* dead pets ignore everything else */
    }

    if (s->stage == TS_EGG)
        return ev | TEV_REFUSED;   /* nothing to care for yet */

    asleep = is_asleep(s);

    switch (a) {
    case TA_FEED_MEAL:
        if (asleep) return ev | TEV_REFUSED;
        if (s->flags & TF_MISBEHAVING) return ev | TEV_REFUSED;
        if (s->hunger >= TAMA_MAX_HEARTS) return ev | TEV_REFUSED; /* full */
        s->hunger++;
        add_weight(s, 1);
        if (s->next_hunger_drop == 0) sched_hunger(s, now);
        /* digestion: this meal will come out 10..90 min from now (one
         * pending poop at a time; an earlier one keeps its schedule) */
        if (s->next_poop == 0)
            s->next_poop = now + rng_range(s, POOP_MIN_DELAY_S,
                                              POOP_MAX_DELAY_S);
        ev |= TEV_ATE | TEV_STATE_DIRTY;
        if (s->attention_kind == TATT_HUNGRY) {
            clear_attention(s, &ev);
            recheck_attention(s, now, &ev);
        }
        break;

    case TA_FEED_SNACK:
        if (asleep) return ev | TEV_REFUSED;
        if (s->flags & TF_MISBEHAVING) return ev | TEV_REFUSED;
        /* snacks are always accepted — even at full hearts the pet
         * happily takes the calories (that's the trap) */
        if (s->happy < TAMA_MAX_HEARTS) s->happy++;
        add_weight(s, 2);
        if (s->next_happy_drop == 0) sched_happy(s, now);
        ev |= TEV_ATE | TEV_STATE_DIRTY;
        if (s->attention_kind == TATT_UNHAPPY && s->happy > 0) {
            clear_attention(s, &ev);
            recheck_attention(s, now, &ev);
        }
        break;

    case TA_LIGHT_TOGGLE:
        s->flags ^= TF_LIGHTS_OFF;
        ev |= TEV_STATE_DIRTY;
        /* turning the lights off answers the bedtime call */
        if ((s->flags & TF_LIGHTS_OFF) && s->attention_kind == TATT_SLEEPY)
            clear_attention(s, &ev);
        break;

    case TA_GAME_WIN:
        if (asleep) return ev | TEV_REFUSED;
        if (s->happy < TAMA_MAX_HEARTS) s->happy++;
        /* exercise burns a pound, but never below the species floor */
        if (s->weight > cur_params(s)->min_weight)
            s->weight--;
        if (s->next_happy_drop == 0) sched_happy(s, now);
        ev |= TEV_STATE_DIRTY;
        if (s->attention_kind == TATT_UNHAPPY && s->happy > 0) {
            clear_attention(s, &ev);
            recheck_attention(s, now, &ev);
        }
        break;

    case TA_GAME_LOSE:
        if (asleep) return ev | TEV_REFUSED;
        /* losing changes nothing; clear a stale unhappy call only if the
         * hearts somehow already recovered */
        if (s->attention_kind == TATT_UNHAPPY && s->happy > 0) {
            clear_attention(s, &ev);
            recheck_attention(s, now, &ev);
        }
        break;

    case TA_MEDICINE:
        /* allowed even while asleep: a pet that falls ill before bed
         * must not be untreatable for 13 hours */
        if (!(s->flags & TF_SICK)) return ev | TEV_REFUSED;
        if (s->medicine_doses_left > 0) s->medicine_doses_left--;
        if (s->medicine_doses_left == 0) {
            s->flags &= (uint8_t)~TF_SICK;
            s->sick_death_deadline = 0;
            ev |= TEV_CURED;
        }
        ev |= TEV_STATE_DIRTY;
        break;

    case TA_CLEAN:
        if (asleep) return ev | TEV_REFUSED;
        if (s->poop_count == 0)              /* nothing to flush: tell the
                                              * UI so the press still gets
                                              * an acknowledging deny cue */
            return ev | TEV_REFUSED;
        s->poop_count = 0;
        s->oldest_poop_epoch = 0;
        ev |= TEV_CLEANED | TEV_STATE_DIRTY;
        break;

    case TA_SCOLD:
        if (asleep) return ev | TEV_REFUSED;
        /* scolding only lands during a discipline call; punishing an
         * innocent pet teaches nothing */
        if (!(s->flags & TF_MISBEHAVING)) return ev | TEV_REFUSED;
        s->discipline = (uint8_t)((s->discipline + SCOLD_DISCIPLINE
                                   > DISCIPLINE_MAX)
                                  ? DISCIPLINE_MAX
                                  : s->discipline + SCOLD_DISCIPLINE);
        ev |= TEV_SCOLDED | TEV_STATE_DIRTY;
        clear_attention(s, &ev);   /* also clears TF_MISBEHAVING */
        recheck_attention(s, now, &ev);
        break;

    case TA_NEW_EGG:
        return ev | TEV_REFUSED;   /* only from the death screen */

    default:
        return ev | TEV_REFUSED;
    }

    return ev;
}

/* shift a stored epoch, preserving the 0 = "not scheduled" sentinel and
 * saturating instead of wrapping if the clock moves past the pet's life */
static uint32_t rebased_epoch(uint32_t e, int32_t delta)
{
    int64_t v = (int64_t)e + (int64_t)delta;
    if (v < 1) v = 1;
    if (v > (int64_t)0xFFFFFFFFll) v = (int64_t)0xFFFFFFFFll;
    return (uint32_t)v;
}

void tama_clock_rebase(tama_state_t *s, int32_t delta_s, uint32_t now)
{
    if (s == NULL || delta_s == 0) return;
    /* per-field (no pointers into the packed struct): every nonzero
     * stored epoch shifts by the same delta so all pending intervals —
     * and the pet's position in its day — are preserved */
#define TAMA_REBASE(field) \
    do { if (s->field != 0) s->field = rebased_epoch(s->field, delta_s); } while (0)
    TAMA_REBASE(born_epoch);
    TAMA_REBASE(stage_epoch);
    TAMA_REBASE(last_wake_epoch);
    TAMA_REBASE(next_hunger_drop);
    TAMA_REBASE(next_happy_drop);
    TAMA_REBASE(next_poop);
    TAMA_REBASE(oldest_poop_epoch);
    TAMA_REBASE(next_misbehave);
    TAMA_REBASE(attention_deadline);
    TAMA_REBASE(sick_death_deadline);
    TAMA_REBASE(evolve_epoch);
#undef TAMA_REBASE

    /* Sleep/wake is derived from the anchor's wall-hour, which the shift
     * just changed. Re-derive against the NEW wall time: if an awake pet's
     * anchor landed inside the sleep window while the new clock says
     * daytime, the "hatched at night" rule would replay a phantom
     * sleep->wake pair (and a day rollover) for a night the pet actually
     * lived awake — declare the current day's wake boundary as the morning
     * it woke on instead. When the new clock says night, the anchor is
     * left alone ON PURPOSE: the pending sleep fires immediately, i.e.
     * setting the clock to bedtime puts the pet to sleep — the classic P1
     * time-cheat, symmetric with the kept asleep->early-wake cheat. */
    {
        const tama_stage_params_t *p = cur_params(s);
        if (!(s->flags & TF_ASLEEP) && stage_sleeps(p)
            && in_sleep_hours(sched_anchor(s), p)
            && !in_sleep_hours(now, p)) {
            uint32_t w = (now / SECS_PER_DAY) * SECS_PER_DAY
                       + (uint32_t)p->wake_hour * SECS_PER_HOUR;
            if (w > now) w = (w >= SECS_PER_DAY) ? w - SECS_PER_DAY : 1u;
            s->last_wake_epoch = w;
        }
    }
}

/* ------------------------------------------------------------------ */
/* serialization                                                       */
/* ------------------------------------------------------------------ */

bool tama_serialize(const tama_state_t *s, uint8_t *buf, size_t cap,
                    size_t *len)
{
    if (s == NULL || buf == NULL || len == NULL) return false;
    if (cap < sizeof(tama_state_t)) return false;
    if (s->magic != TAMA_MAGIC || s->version != TAMA_VERSION) return false;
    memcpy(buf, s, sizeof(tama_state_t));
    *len = sizeof(tama_state_t);
    return true;
}

bool tama_deserialize(tama_state_t *s, const uint8_t *buf, size_t len)
{
    tama_state_t tmp;

    if (s == NULL || buf == NULL || len != sizeof(tama_state_t))
        return false;

    memcpy(&tmp, buf, sizeof tmp);
    if (tmp.magic != TAMA_MAGIC || tmp.version != TAMA_VERSION)
        return false;
    if (tmp.stage > TS_DEAD)
        return false;

    /* clamp soft fields so a corrupted-but-plausible blob cannot push
     * the core out of its invariants */
    if (tmp.hunger > TAMA_MAX_HEARTS) tmp.hunger = TAMA_MAX_HEARTS;
    if (tmp.happy > TAMA_MAX_HEARTS) tmp.happy = TAMA_MAX_HEARTS;
    if (tmp.discipline > DISCIPLINE_MAX) tmp.discipline = DISCIPLINE_MAX;
    if (tmp.poop_count > TAMA_MAX_POOP) tmp.poop_count = TAMA_MAX_POOP;
    if (tmp.attention_kind > TATT_SLEEPY) tmp.attention_kind = TATT_NONE;
    if (tmp.rng_state == 0) tmp.rng_state = RNG_FALLBACK_SEED;

    *s = tmp;
    return true;
}
