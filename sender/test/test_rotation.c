/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Round 11, the refresh rotation (shared with ESPHome's scheduler): the clearing slots of a board's unused lights
 * (an empty LIGHT_DEF, SCENE and SCENE_EFFECT each) are part of its first 3 complete refresh rounds after init, a
 * resync or a change of its light count, then of every 4th round (rounds 0, 1, 2, 6, 10, ...). The used lights' items
 * are in every round.
 *
 * The shared scenario, round by round through the scheduler's own refresh list (take_refresh):
 *  - init: 3 boards with 1 / 3 / 5 lights, clear_unused, a LIGHT_DEF + SET per light; 20 rounds per board;
 *  - set_lights: board 0 to 2 lights (light 1 gets a LIGHT_DEF + SET); 10 more rounds per board (boards 1 and 2 go on
 *    counting, board 0 starts at round 0);
 *  - resync: 10 more rounds per board, all from round 0.
 * Each reset comes mid-round (one refresh slot of the next round taken just before it): the reset starts the refresh
 * list anew, so round 0 is a complete round.
 * One line per board and round: "<phase> b<board> r<round> clear <lights> | used <TYPE light ...>". clear lists the
 * unused lights whose 3 clearing slots the round had ("-" none); used lists the round's LIGHT_DEF and SET slots of the
 * used lights (sorted). ESPHome's host test writes the same lines for the same scenario: the two must be equal.
 *
 * The lines are checked against the rule here and against the golden file (test/rotation_trace.txt):
 *   test_rotation <golden>            check
 *   test_rotation <golden> --write    write it anew (a deliberate rotation change)
 * The library's sources are compiled into this file (take_refresh is static).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra_sender.c"
#include "ultra_sender_bits.c"

static int failures, checks;
#define CHECK(c, ...) \
    do { \
        checks++; \
        if (!(c)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        } else { \
            printf("ok   "); \
        } \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } while (0)

#define NB 3u
static ultra_sender_t L;
static ultra_sender_board_t B[NB];
static ultra_sender_item_t IT[64];
static const int64_t NOW = 1000000;
static char trace[16384];
static size_t trace_len;
static uint32_t round_no[NB];
static int rule_bad;

static void add(const char *fmt, const char *phase, uint32_t b, uint32_t r, const char *clear, const char *used)
{
    trace_len += (size_t)snprintf(trace + trace_len, sizeof trace - trace_len, fmt, phase, b, r, clear, used);
}

static void light(uint16_t board, uint8_t li)
{
    ultra_light_def_t d;
    ultra_values_t v;
    uint32_t i;
    ultra_light_def_init(&d);
    d.roles[li] = ULTRA_ROLE_BRIGHTNESS;
    v.m = 1000;
    for (i = 0; i < 5u; i++) {
        v.a[i] = v.b[i] = 65535;
    }
    CHECK(ultra_sender_light_def(&L, board, li, &d) == ULTRA_OK && ultra_sender_set(&L, board, li, &v, 0, 0, NOW) ==
          ULTRA_OK, "board %u light %u: a LIGHT_DEF and a SET", board, li);
}

/* one round of the board's refresh list: the slots until the position wraps (a slot taken after a wrap inside the
 * same call, past skipped positions, belongs to the next round: it is held back for it) */
static uint8_t held[NB][ULTRA_SLOT_BYTES];
static int have_held[NB];

static void one_round(const char *phase, uint32_t bi)
{
    ultra_sender_board_t *b = &B[bi];
    uint8_t s[ULTRA_SLOT_BYTES];
    uint32_t clear[ULTRA_LIGHTS] = { 0 }, n = 0, k, cleared = 0;
    uint8_t used[64];
    char cl[64], us[256];
    size_t cp = 0, up = 0;
    const uint32_t r = round_no[bi]++;
    int done = 0, guard = 0;
    for (; !done && guard < 1000; guard++) {
        const uint8_t before = b->clr_round;
        if (have_held[bi]) {
            memcpy(s, held[bi], sizeof s);
            have_held[bi] = 0;
        } else {
            if (!take_refresh(&L, b, s, NOW)) {
                break;
            }
            if (b->rr == 0u) {
                done = 1;                        /* the round's last position */
            } else if (b->clr_round != before) {
                memcpy(held[bi], s, sizeof s);   /* a wrap before this slot: the next round's first */
                have_held[bi] = 1;
                break;
            }
        }
        {
            const uint8_t li = (uint8_t)(s[1] >> 4);
            if (li < ULTRA_LIGHTS && li >= b->lights) {
                clear[li] |= s[0] == ULTRA_LIGHT_DEF ? 1u : s[0] == ULTRA_SCENE ? 2u : s[0] == ULTRA_SCENE_EFFECT ? 4u : 8u;
            } else if (li < ULTRA_LIGHTS && (s[0] == ULTRA_LIGHT_DEF || s[0] == ULTRA_SET) && n < sizeof used) {
                used[n++] = (uint8_t)((s[0] == ULTRA_SET ? 0x80u : 0u) | li);
            }
        }
    }
    /* clear: the lights with all three clearing slots; anything else is an error */
    for (k = 0; k < ULTRA_LIGHTS; k++) {
        if (clear[k] == 7u) {
            cleared++;
            cp += (size_t)snprintf(cl + cp, sizeof cl - cp, "%s%u", cp ? "," : "", k);
        } else if (clear[k] != 0u) {
            rule_bad++;
            printf("FAIL %s b%u r%u: light %u has clearing slots 0x%X (not all three)\n", phase, bi, r, k, clear[k]);
        }
    }
    if (cp == 0u) {
        snprintf(cl, sizeof cl, "-");
    }
    {   /* the rule: rounds 0, 1, 2, then every 4th (6, 10, ...), when the board has unused lights */
        const int due = (r < 3u || (r >= 6u && (r - 6u) % 4u == 0u)) && L.clear_unused && b->lights < ULTRA_LIGHTS;
        if (due ? cleared != ULTRA_LIGHTS - b->lights : cleared != 0u) {
            rule_bad++;
            printf("FAIL %s b%u r%u: clear %s (due: %d)\n", phase, bi, r, cl, due);
        }
    }
    /* used: sorted, DEF before SET per light */
    {
        uint32_t i, j;
        for (i = 1; i < n; i++) {
            for (j = i; j > 0u && ((used[j] & 15u) * 2u + (used[j] >> 7)) < ((used[j - 1] & 15u) * 2u + (used[j - 1] >> 7));
                 j--) {
                const uint8_t t = used[j];
                used[j] = used[j - 1];
                used[j - 1] = t;
            }
        }
        for (i = 0; i < n; i++) {
            up += (size_t)snprintf(us + up, sizeof us - up, " %s%u", (used[i] & 0x80u) ? "SET" : "DEF", used[i] & 15u);
        }
        if (n == 0u) {
            snprintf(us, sizeof us, " -");
        }
        if (n != 2u * b->lights) {
            rule_bad++;
            printf("FAIL %s b%u r%u: used%s (not every light's LIGHT_DEF and SET)\n", phase, bi, r, us);
        }
    }
    add("%s b%u r%u clear %s | used%s\n", phase, bi, r, cl, us);
}

/* one refresh slot of the board's next round, dropped (a reset mid-round) */
static void one_slot(uint32_t bi)
{
    uint8_t s[ULTRA_SLOT_BYTES];
    if (have_held[bi]) {
        have_held[bi] = 0;
    } else {
        CHECK(take_refresh(&L, &B[bi], s, NOW) && B[bi].rr != 0u, "board %u: a slot into round %u", bi, round_no[bi]);
    }
}

static void rounds(const char *phase, uint32_t count)
{
    uint32_t bi, i;
    for (bi = 0; bi < NB; bi++) {
        for (i = 0; i < count; i++) {
            one_round(phase, bi);
        }
    }
}

/* positions per SET of a 1-light board in the steady rotation (rounds 3-6 repeat), and as before (clears every round) */
static void set_rate(void)
{
    const uint32_t items = 2u, clears = (ULTRA_LIGHTS - 1u) * 3u;
    const uint32_t before = items + clears, after_x4 = 4u * items + clears;
    printf("     a 1-light board (LIGHT_DEF + SET, no CONFIG): a SET every %u refresh slots before, every %u.%02u now\n",
           before, after_x4 / 4u, after_x4 % 4u * 25u);
}

int main(int argc, char **argv)
{
    ultra_sender_config_t c;
    const char *golden = argc > 1 ? argv[1] : NULL;
    const int write = argc > 2 && strcmp(argv[2], "--write") == 0;
    uint32_t bi;
    ultra_sender_config_init(&c, NB, 5);
    c.clear_unused = 1;
    c.seed = 11;
    CHECK(ultra_sender_init(&L, &c, B, IT, 64) == ULTRA_OK, "init: 3 boards, clear_unused");
    CHECK(ultra_sender_set_lights(&L, 0, 1) == ULTRA_OK && ultra_sender_set_lights(&L, 1, 3) == ULTRA_OK,
          "board 0: 1 light, board 1: 3, board 2: 5");
    light(0, 0);
    light(1, 0);
    light(1, 1);
    light(1, 2);
    for (bi = 0; bi < 5u; bi++) {
        light(2, (uint8_t)bi);
    }
    rounds("init", 20);
    one_slot(0);
    CHECK(ultra_sender_set_lights(&L, 0, 2) == ULTRA_OK, "set_lights: board 0 to 2 lights");
    light(0, 1);
    round_no[0] = 0;
    have_held[0] = 0;
    rounds("set_lights", 10);
    for (bi = 0; bi < NB; bi++) {
        one_slot(bi);
    }
    ultra_sender_resync(&L, NOW);
    for (bi = 0; bi < NB; bi++) {
        round_no[bi] = 0;
        have_held[bi] = 0;
    }
    rounds("resync", 10);
    CHECK(rule_bad == 0, "the rule in every round: clearing slots in rounds 0, 1, 2, 6, 10, ... after init / set_lights "
          "/ resync (all 3 per unused light), the used lights' LIGHT_DEF + SET in every round (%u lines)",
          (unsigned)(NB * 40u));
    set_rate();
    if (golden != NULL) {
        FILE *f = fopen(golden, write ? "wb" : "rb");
        static char want[sizeof trace];
        size_t n = 0;
        if (f != NULL) {
            if (write) {
                n = fwrite(trace, 1, trace_len, f);
            } else {
                n = fread(want, 1, sizeof want - 1u, f);
            }
            fclose(f);
        }
        if (write) {
            CHECK(n == trace_len, "golden written: %s (%u bytes)", golden, (unsigned)trace_len);
        } else {
            CHECK(f != NULL && n == trace_len && memcmp(want, trace, n) == 0, "the trace = the golden %s (%u bytes)",
                  golden, (unsigned)trace_len);
            if (!(f != NULL && n == trace_len && memcmp(want, trace, n) == 0)) {
                fwrite(trace, 1, trace_len, stdout);
            }
        }
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL ROTATION TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
