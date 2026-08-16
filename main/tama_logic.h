/*
 * tama_logic — the WatchaGotchi game state machine. PURE C:
 * no LVGL, no FreeRTOS, no ESP-IDF. Compiles on the device and on a PC.
 *
 * Time model: the caller owns the clock and passes `now` = pet-epoch seconds
 * (seconds of pet-life wall time; frozen while the device is off). Every
 * scheduled event is stored as an ABSOLUTE pet-epoch, so resuming after a
 * power-off needs no catch-up: pending events are simply still pending.
 * Pet-epoch 0 is midnight of day 0 — hour-of-day = (now / 3600) % 24.
 */
#ifndef TAMA_LOGIC_H
#define TAMA_LOGIC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- life stages & species ---------------- */
typedef enum {
    TS_EGG = 0, TS_BABY, TS_CHILD, TS_TEEN, TS_ADULT, TS_SECRET, TS_DEAD,
} tama_stage_t;

/* species ids within TS_TEEN */
enum { TEEN_GOOD = 0, TEEN_BAD = 1 };
/* species ids within TS_ADULT (best..worst care) */
enum { ADULT_HERO = 0, ADULT_CHEER, ADULT_AVG, ADULT_GRUMP, ADULT_FERAL };

/* ---------------- state flags ---------------- */
#define TF_SICK            (1u << 0)
#define TF_SICK_THIS_STAGE (1u << 1)   /* stage's random sickness already spent */
#define TF_ASLEEP          (1u << 2)
#define TF_LIGHTS_OFF      (1u << 3)
#define TF_ATTENTION       (1u << 4)   /* attention icon lit */
#define TF_MISBEHAVING     (1u << 5)   /* attention is a discipline call */
#define TF_NIGHT_MISTAKE   (1u << 6)   /* lights-on mistake already counted tonight */

/* why the attention icon is lit */
enum { TATT_NONE = 0, TATT_HUNGRY, TATT_UNHAPPY, TATT_MISBEHAVE, TATT_SLEEPY };

/* ---------------- persistent state (NVS blob) ---------------- */
#define TAMA_MAGIC   0x54414d41u   /* 'TAMA' */
#define TAMA_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  stage;               /* tama_stage_t */
    uint8_t  species;             /* id within stage (teen/adult tables) */
    uint32_t born_epoch;
    uint32_t stage_epoch;         /* when the current stage began */
    uint32_t last_wake_epoch;     /* age rollover anchor */
    uint8_t  hunger, happy;       /* 0..4 hearts */
    uint8_t  discipline;          /* 0..100 */
    uint16_t weight;              /* lb */
    uint8_t  age_days;
    uint8_t  care_mistakes;       /* lifetime; adult dies at TAMA_DEATH_MISTAKES */
    uint8_t  stage_mistakes;      /* mistakes within the current stage */
    uint8_t  poop_count;          /* 0..TAMA_MAX_POOP on screen */
    uint8_t  flags;               /* TF_* */
    uint8_t  attention_kind;      /* TATT_* */
    uint8_t  medicine_doses_left; /* >0 while sick */
    /* absolute pet-epoch schedule (0 = not scheduled) */
    uint32_t next_hunger_drop;
    uint32_t next_happy_drop;
    uint32_t next_poop;
    uint32_t oldest_poop_epoch;
    uint32_t next_misbehave;
    uint32_t attention_deadline;
    uint32_t sick_death_deadline;
    uint32_t evolve_epoch;
    uint32_t rng_state;           /* xorshift32; persisted for replays */
    uint8_t  reserved[8];
} tama_state_t;

#define TAMA_BLOB_SIZE sizeof(tama_state_t)

/* ---------------- tunables (defaults; tuned via sim_life) ---------------- */
#define TAMA_MAX_HEARTS       4
#define TAMA_MAX_POOP         4
#define TAMA_DEATH_MISTAKES   5
#define TAMA_CARE_WINDOW_S    (15 * 60)
#define TAMA_SICK_DEATH_S     (6 * 3600)

/* ---------------- actions (from the UI) ---------------- */
typedef enum {
    TA_FEED_MEAL = 0,
    TA_FEED_SNACK,
    TA_LIGHT_TOGGLE,
    TA_GAME_WIN,        /* the UI runs the game; logic applies the outcome */
    TA_GAME_LOSE,
    TA_MEDICINE,
    TA_CLEAN,
    TA_SCOLD,
    TA_NEW_EGG,         /* from the death screen */
} tama_action_t;

/* ---------------- events (returned as a bitmask) ---------------- */
#define TEV_STATE_DIRTY    (1u << 0)   /* persist the blob */
#define TEV_ATTENTION_ON   (1u << 1)
#define TEV_ATTENTION_OFF  (1u << 2)
#define TEV_HEART_EMPTY    (1u << 3)
#define TEV_POOPED         (1u << 4)
#define TEV_CLEANED        (1u << 5)
#define TEV_GOT_SICK       (1u << 6)
#define TEV_CURED          (1u << 7)
#define TEV_FELL_ASLEEP    (1u << 8)
#define TEV_WOKE           (1u << 9)
#define TEV_DAY_ROLLOVER   (1u << 10)
#define TEV_EVOLVED        (1u << 11)
#define TEV_DIED           (1u << 12)
#define TEV_CARE_MISTAKE   (1u << 13)
#define TEV_ATE            (1u << 14)
#define TEV_REFUSED        (1u << 15)  /* meal refused (full) / action invalid */
#define TEV_SCOLDED        (1u << 16)
#define TEV_HATCHED        (1u << 17)

/* ---------------- API ---------------- */

/* Start a fresh egg at `now`. Seed != 0 (e.g. esp_random()). */
void tama_new_egg(tama_state_t *s, uint32_t now, uint32_t seed);

/* Advance the pet to `now`. Call at >= 1 Hz with monotonically nondecreasing
 * `now`; safe to call with a large jump (fires everything due, in order).
 * Returns a TEV_* bitmask of everything that happened. */
uint32_t tama_tick(tama_state_t *s, uint32_t now);

/* Apply a user action at `now`. Returns a TEV_* bitmask. */
uint32_t tama_action(tama_state_t *s, tama_action_t a, uint32_t now);

/* The user changed the wall clock by `delta_s` (new - old); `now` is the
 * NEW pet-epoch (post-shift). Shifts every scheduled epoch so pending
 * intervals are preserved, then re-derives the sleep anchor against the
 * new wall time so no phantom night is replayed — while keeping the P1
 * time-cheats: setting the clock into the night puts the pet to sleep,
 * setting an asleep pet's clock to morning wakes it. */
void tama_clock_rebase(tama_state_t *s, int32_t delta_s, uint32_t now);

/* Serialization: memcpy-with-validation. Returns false on bad magic/version
 * (caller starts a fresh egg). */
bool tama_serialize(const tama_state_t *s, uint8_t *buf, size_t cap, size_t *len);
bool tama_deserialize(tama_state_t *s, const uint8_t *buf, size_t len);

/* ---------------- introspection helpers (UI/status pages) ---------------- */
static inline int  tama_hour_of_day(uint32_t now) { return (int)((now / 3600u) % 24u); }
static inline bool tama_is_sick(const tama_state_t *s)    { return (s->flags & TF_SICK) != 0; }
static inline bool tama_is_asleep(const tama_state_t *s)  { return (s->flags & TF_ASLEEP) != 0; }
static inline bool tama_lights_off(const tama_state_t *s) { return (s->flags & TF_LIGHTS_OFF) != 0; }
static inline bool tama_attention(const tama_state_t *s)  { return (s->flags & TF_ATTENTION) != 0; }

/* Stage parameters (exposed for the UI and for sim_life tuning) */
typedef struct {
    uint16_t hunger_interval_s10;  /* seconds/10 per heart drop (fits u16) */
    uint16_t happy_interval_s10;
    uint8_t  sleep_hour, wake_hour;
    uint32_t stage_duration_s;     /* 0 = terminal (adult: lifespan handled separately) */
    uint16_t base_weight, min_weight;
} tama_stage_params_t;

const tama_stage_params_t *tama_stage_params(tama_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* TAMA_LOGIC_H */
