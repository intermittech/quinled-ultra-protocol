/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The sender library's local pixel effects (protocol 0.2.8, the Ultra-Digital receiver): PIXEL_FX (3 copies, a newer one
 * replacing a queued one, the START stamped against every frame's TIME, kept when only the parameters change, the
 * refresh, a resync), the pixel CONFIG 129-139 in their own slots (batched into one SEQ per page, unchanged values
 * queue nothing, the refresh), the refusals and the pool, and that boards without them see nothing of it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra_sender.h"

static int failures, checks;
#define CHECK(c, ...) \
    do { \
        checks++; \
        if (!(c)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define NB 3u
#define NI 64u
static ultra_sender_t L;
static ultra_sender_board_t B[NB];
static ultra_sender_item_t IT[NI];
static uint8_t F[ULTRA_SENDER_FRAME_BYTES(NB)];
static int64_t now;

static void mk(uint16_t n, uint8_t lights, uint16_t items)
{
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, n, lights);
    c.seed = 4242;
    CHECK(ultra_sender_init(&L, &c, B, IT, items) == ULTRA_OK, "init");
    now = 1000000;
}

static uint32_t fr(void)
{
    const uint32_t n = ultra_sender_next_frame(&L, now, F);
    if (n != 0u) {
        now = ultra_sender_last_time_us(&L) + 300;
    } else {
        memset(F, 0xEE, sizeof F);
        now += 1000;
    }
    return n;
}

static void pre(void)
{
    uint32_t i;
    for (i = 0; i < ultra_preamble_frames(L.n); i++) {
        (void)fr();
    }
}

static const uint8_t *slot(uint16_t b)
{
    return F + ULTRA_SLOT_BYTES * b;
}

/* the absolute start a PIXEL_FX / EFFECT slot of the last frame carries (against its TIME), us */
static int64_t slot_start_us(uint16_t b)
{
    uint32_t t = 0;
    (void)ultra_parse_header(F + ULTRA_SLOT_BYTES * L.n, &t);
    return ultra_sender_last_time_us(&L) + (int64_t)(int32_t)ultra_get32(slot(b) + ULTRA_PX_START) * 1000;
}

static ultra_pixel_fx_t sample(void)
{
    ultra_pixel_fx_t f;
    memset(&f, 0, sizeof f);
    f.effect = 7;
    f.speed = 100;
    f.intensity = 150;
    f.palette = ULTRA_PX_PAL_DEFAULT;
    f.c[0][0] = 255;
    f.brightness = 200;
    f.transition_ms = 1000;
    return f;
}

static void test_fx(void)
{
    ultra_pixel_fx_t f = sample();
    uint8_t seqs[3];
    int64_t anchor, a2;
    uint32_t i, refresh = 0;

    mk(3, 5, NI);                                /* (every light index in use: nothing is cleared) */
    pre();
    CHECK(ultra_sender_pixel_fx(&L, 1, 0, &f, now) == ULTRA_OK, "pixel_fx");
    anchor = ultra_sender_pixel_fx_start(&L, 1, 0);
    CHECK(anchor != ULTRA_NO_START && anchor >= now, "a new effect starts ahead (%lld, now %lld)", (long long)anchor, (long long)now);
    for (i = 0; i < 3u; i++) {
        CHECK(fr() != 0u && slot(1)[0] == ULTRA_PIXEL_FX && (slot(1)[1] >> 4) == 0 && ultra_slot_check(slot(1)) == 0,
              "copy %u", (unsigned)i);
        seqs[i] = slot(1)[2];
        CHECK(llabs((long long)(slot_start_us(1) - anchor)) <= 500, "START against TIME: %lld us off", (long long)(slot_start_us(1) - anchor));
        CHECK(slot(1)[ULTRA_PX_EFFECT] == 7 && slot(1)[ULTRA_PX_BRIGHTNESS] == 200, "the parameters");
        CHECK(slot(0)[0] == ULTRA_NOP && slot(2)[0] == ULTRA_NOP, "other boards: nothing");
    }
    CHECK(seqs[0] == seqs[1] && seqs[1] == seqs[2], "one SEQ in the 3 copies");
    CHECK(!ultra_sender_pending(&L), "sent");

    /* a change of the parameters: the same START, a new SEQ, a newer command replacing the queued one */
    f.speed = 50;
    CHECK(ultra_sender_pixel_fx_at(&L, 1, 0, &f, anchor) == ULTRA_OK, "parameter change");
    f.speed = 60;
    CHECK(ultra_sender_pixel_fx_at(&L, 1, 0, &f, anchor) == ULTRA_OK, "again");
    CHECK(ultra_sender_pending(&L) == 1, "replaced, not queued twice");
    CHECK(fr() != 0u && slot(1)[0] == ULTRA_PIXEL_FX && slot(1)[2] == (uint8_t)(seqs[0] + 2) && slot(1)[ULTRA_PX_SPEED] == 60,
          "the latest, SEQ +2");
    CHECK(llabs((long long)(slot_start_us(1) - anchor)) <= 500, "the anchor kept");
    CHECK(ultra_sender_pixel_fx_start(&L, 1, 0) == anchor, "start() reads it back");
    /* a new effect: a new START (the group start for now) */
    f.effect = 9;
    CHECK(ultra_sender_pixel_fx(&L, 1, 0, &f, now) == ULTRA_OK, "new effect");
    a2 = ultra_sender_pixel_fx_start(&L, 1, 0);
    CHECK(a2 >= anchor, "the group start for now (shared with commands queued close together)");
    while (ultra_sender_busy(&L)) {
        (void)fr();
    }

    /* the refresh at rest: the PIXEL_FX comes round again, same SEQ and START, other boards NOP / their own */
    for (i = 0; i < 300u && refresh < 3u; i++) {
        L.last_start_us = now - 10000000;        /* (past the refresh interval) */
        if (fr() != 0u && slot(1)[0] == ULTRA_PIXEL_FX) {
            refresh++;
            CHECK(llabs((long long)(slot_start_us(1) - a2)) <= 500, "refresh: the START");
            CHECK(slot(1)[ULTRA_PX_EFFECT] == 9, "refresh: the latest effect");
        }
    }
    CHECK(refresh >= 3u, "the refresh repeats it (%u)", (unsigned)refresh);

    /* a resync: queued again, the START kept (the effect continues in phase) */
    ultra_sender_resync(&L, now);
    pre();
    for (i = 0; i < 400u && ultra_sender_pending(&L) != 0; i++) {
        (void)fr();
    }
    CHECK(ultra_sender_pixel_fx_start(&L, 1, 0) == a2, "the resync kept the START");
    printf("ok   PIXEL_FX: copies, START, parameter change, new effect, refresh, resync\n");
}

static void test_segments_and_refusals(void)
{
    ultra_pixel_fx_t f = sample();
    uint32_t i, seen = 0;
    mk(2, 5, NI);
    pre();
    CHECK(ultra_sender_pixel_fx(&L, 0, 0, &f, now) == ULTRA_OK && ultra_sender_pixel_fx(&L, 0, 3, &f, now) == ULTRA_OK,
          "segments 0 and 3");
    CHECK(ultra_sender_pixel_fx(&L, 0, 4, &f, now) == ULTRA_ERR_ARG, "segment 4: the receiver ignores LIGHT 4-14");
    CHECK(ultra_sender_pixel_fx(&L, 5, 0, &f, now) == ULTRA_ERR_ARG, "no such board");
    f.flags = 0x80;
    CHECK(ultra_sender_pixel_fx(&L, 0, 1, &f, now) == ULTRA_ERR_ARG, "the reserved flag bit refused");
    CHECK(ultra_sender_pixel_fx(&L, 0, 1, NULL, now) == ULTRA_ERR_ARG, "NULL");
    f.flags = ULTRA_PXF_LOCK | ULTRA_PXF_STORE;
    CHECK(ultra_sender_pixel_fx(&L, 0, 1, &f, now) == ULTRA_OK, "LOCK, STORE and the white mode are fine");
    for (i = 0; i < 12u; i++) {
        (void)fr();
        if (slot(0)[0] == ULTRA_PIXEL_FX) {
            seen |= 1u << (slot(0)[1] >> 4);
        }
    }
    CHECK(seen == ((1u << 0) | (1u << 1) | (1u << 3)), "three segments went out (mask %x)", (unsigned)seen);
    /* RESTART is an action: the command's copies carry it (the receiver starts the effect over), the refresh and a resync's
     * replay do not (or every refresh would start it over) */
    mk(1, 5, NI);
    pre();
    f = sample();
    f.flags = ULTRA_PXF_RESTART | ULTRA_PXF_LOCK;
    CHECK(ultra_sender_pixel_fx(&L, 0, 0, &f, now) == ULTRA_OK, "RESTART is accepted");
    {
        uint32_t k = 0, with = 0, without = 0;
        uint8_t seq0 = 0, same_seq = 1;
        for (i = 0; i < 400u && k < 9u; i++) {
            L.last_start_us = now - 10000000;
            (void)fr();
            if (slot(0)[0] == ULTRA_PIXEL_FX) {
                if (k == 0u) {
                    seq0 = slot(0)[2];
                }
                same_seq = (uint8_t)(same_seq && slot(0)[2] == seq0);
                if (slot(0)[ULTRA_PX_FLAGS] & ULTRA_PXF_RESTART) {
                    with++;
                    CHECK(k < 3u, "only the command's copies carry RESTART (copy %u)", (unsigned)k);
                } else {
                    without++;
                }
                CHECK((slot(0)[ULTRA_PX_FLAGS] & ULTRA_PXF_LOCK) != 0u, "its other flags stay");
                CHECK(ultra_slot_check(slot(0)) == 0, "sealed");
                k++;
            }
        }
        CHECK(with == 3u && without >= 3u && same_seq, "3 copies with RESTART, the refresh without, one SEQ (%u / %u)", (unsigned)with,
              (unsigned)without);
        ultra_sender_resync(&L, now);
        for (i = 0; i < 200u; i++) {
            (void)fr();
            if (slot(0)[0] == ULTRA_PIXEL_FX) {
                CHECK(!(slot(0)[ULTRA_PX_FLAGS] & ULTRA_PXF_RESTART), "a resync's replay does not restart the effect");
            }
        }
    }
    /* a board with fewer lights in use refuses a segment beyond them */
    mk(1, 1, NI);
    pre();
    f = sample();
    CHECK(ultra_sender_pixel_fx(&L, 0, 1, &f, now) == ULTRA_ERR_ARG, "segment 1 on a board with 1 light");
    /* the pool: a segment's PIXEL_FX takes one item */
    mk(1, 5, 1);
    pre();
    CHECK(ultra_sender_pixel_fx(&L, 0, 0, &f, now) == ULTRA_OK && ultra_sender_items_used(&L) == 1, "one item");
    CHECK(ultra_sender_pixel_fx(&L, 0, 1, &f, now) == ULTRA_ERR_FULL, "the pool is full");
    CHECK(ultra_sender_pixel_config(&L, 0, ULTRA_CFG_PX_PIXELS, 60) == ULTRA_ERR_FULL, "... for the CONFIG too");
    printf("ok   segments, refusals, the pool\n");
}

static void test_config(void)
{
    uint32_t i, ids_seen = 0, cfg_slots = 0;
    uint8_t seq_a = 0, seq_b = 0;
    mk(2, 5, NI);
    pre();
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_CHIP, ULTRA_PXC_WS2812B_V5) == ULTRA_OK, "chip");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_PIXELS, 300) == ULTRA_OK, "pixels");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_POLICY, ULTRA_PXP_POWER_ON) == ULTRA_OK, "policy (page 2)");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_ORDER, 0) == ULTRA_OK, "order 0 = the chip's own");
    CHECK(ultra_sender_pending(&L) == 0, "nothing is queued before the next frame (flushed with it)");
    for (i = 0; i < 6u; i++) {
        (void)fr();
        if (slot(1)[0] == ULTRA_CONFIG && slot(1)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST) {
            uint32_t j;
            cfg_slots++;
            CHECK(ultra_slot_check(slot(1)) == 0, "CRC");
            for (j = 0; j < 9u; j++) {
                const uint8_t id = slot(1)[ULTRA_PAYLOAD + 3u * j];
                if (id != 0u) {
                    ids_seen |= 1u << (id - ULTRA_CFG_PX_FIRST);
                }
            }
            if (slot(1)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_FIRST) {
                seq_a = slot(1)[2];
                CHECK(ultra_get16(slot(1) + ULTRA_PAYLOAD + 1) == ULTRA_PXC_WS2812B_V5 &&
                          slot(1)[ULTRA_PAYLOAD + 3] == ULTRA_CFG_PX_ORDER && slot(1)[ULTRA_PAYLOAD + 6] == ULTRA_CFG_PX_PIXELS &&
                          ultra_get16(slot(1) + ULTRA_PAYLOAD + 7) == 300 && slot(1)[ULTRA_PAYLOAD + 9] == 0,
                      "page 0: 129, 130, 131 in id order, then unused pairs");
            } else {
                seq_b = slot(1)[2];
                CHECK(slot(1)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_POLICY && ultra_get16(slot(1) + ULTRA_PAYLOAD + 1) == 1, "page 1: 138");
            }
        }
        CHECK(slot(0)[0] != ULTRA_CONFIG, "board 0 has none");
    }
    CHECK(ids_seen == ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 9)), "ids 129, 130, 131, 138 (%x)", (unsigned)ids_seen);
    CHECK(cfg_slots == 6u, "two pages x 3 copies (%u)", (unsigned)cfg_slots);
    CHECK(seq_a != seq_b, "each page its SEQ (%u %u)", seq_a, seq_b);
    while (ultra_sender_busy(&L)) {
        (void)fr();
    }
    /* the same values again: nothing; one changed: its page only */
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_PIXELS, 300) == ULTRA_OK, "same value");
    (void)fr();
    CHECK(!ultra_sender_pending(&L), "nothing queued for an unchanged value");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_PIXELS, 301) == ULTRA_OK, "changed");
    (void)fr();
    CHECK(slot(1)[0] == ULTRA_CONFIG && ultra_get16(slot(1) + ULTRA_PAYLOAD + 7) == 301 && ultra_sender_pending(&L) == 1,
          "page 0 again with the new count");
    CHECK(slot(1)[2] != seq_a, "a new SEQ");
    while (ultra_sender_busy(&L)) {
        (void)fr();
    }
    /* the refresh repeats both pages */
    {
        uint32_t p0 = 0, p1 = 0;
        for (i = 0; i < 400u; i++) {
            L.last_start_us = now - 10000000;
            if (fr() != 0u && slot(1)[0] == ULTRA_CONFIG && slot(1)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST) {
                if (slot(1)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_FIRST) {
                    p0++;
                } else {
                    p1++;
                }
            }
            CHECK(!(slot(0)[0] == ULTRA_CONFIG && slot(0)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST), "board 0 never");
        }
        CHECK(p0 >= 2u && p1 >= 2u, "the refresh repeats the pixel CONFIG (%u, %u)", (unsigned)p0, (unsigned)p1);
    }
    /* a resync queues them again */
    ultra_sender_resync(&L, now);
    pre();
    {
        uint32_t seen = 0;
        for (i = 0; i < 40u && ultra_sender_pending(&L); i++) {
            (void)fr();
            seen += slot(1)[0] == ULTRA_CONFIG && slot(1)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST;
        }
        CHECK(seen >= 6u, "resync: both pages x 3 copies (%u)", (unsigned)seen);
    }
    /* the values the receiver would refuse */
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_CHIP, ULTRA_PXC_COUNT) == ULTRA_ERR_ARG, "chip beyond the presets");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_POLICY, 8) == ULTRA_ERR_ARG, "policy bits");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_LOSS_TIMEOUT, 3) == ULTRA_ERR_ARG, "timeout below 0.5 s");
    CHECK(ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_LOSS_TIMEOUT, 0) == ULTRA_OK, "0 = 3 s");
    CHECK(ultra_sender_pixel_config(&L, 1, 128, 1) == ULTRA_ERR_ARG && ultra_sender_pixel_config(&L, 1, 140, 1) == ULTRA_ERR_ARG,
          "not a pixel id");
    CHECK(ultra_sender_pixel_config(&L, 7, ULTRA_CFG_PX_PIXELS, 1) == ULTRA_ERR_ARG, "no such board");
    printf("ok   pixel CONFIG: pages, SEQ per page, unchanged = nothing, refresh, resync, refusals\n");
}

/* a board without pixel items sends exactly what it sent before: the same frames with and without a pixel board next to it */
static void test_other_boards_unchanged(void)
{
    static uint8_t trace[2][200][sizeof F];
    static uint32_t len[2][200];
    uint32_t run, i;
    for (run = 0; run < 2u; run++) {
        ultra_light_def_t d;
        ultra_values_t v;
        ultra_pixel_fx_t f = sample();
        mk(2, 2, NI);
        ultra_light_def_init(&d);
        d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
        (void)ultra_sender_light_def(&L, 0, 0, &d);
        memset(&v, 0, sizeof v);
        v.m = 30000;
        for (i = 0; i < 5u; i++) {
            v.a[i] = v.b[i] = 65535;
        }
        (void)ultra_sender_set(&L, 0, 0, &v, 500, 0, now);
        if (run == 1u) {                         /* board 1 is a pixel receiver; board 0 is as before */
            (void)ultra_sender_set_lights(&L, 1, ULTRA_LIGHTS);
            (void)ultra_sender_pixel_config(&L, 1, ULTRA_CFG_PX_PIXELS, 100);
            (void)ultra_sender_pixel_fx(&L, 1, 0, &f, now);
        }
        for (i = 0; i < 200u; i++) {
            len[run][i] = fr();
            memcpy(trace[run][i], F, sizeof F);
        }
    }
    for (i = 0; i < 200u; i++) {
        CHECK(len[0][i] == len[1][i] || len[0][i] == 0u || len[1][i] == 0u, "frame %u length", (unsigned)i);
        if (len[0][i] != 0u && len[1][i] != 0u) {
            /* board 0's slot is the same in both; (TIME and the other board's slot differ) */
            CHECK(memcmp(trace[0][i], trace[1][i], ULTRA_SLOT_BYTES) == 0, "frame %u: board 0's slot", (unsigned)i);
        }
    }
    printf("ok   a board next to a pixel receiver sends what it sent without\n");
}

int main(void)
{
    test_fx();
    test_segments_and_refusals();
    test_config();
    test_other_boards_unchanged();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL PIXEL TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
