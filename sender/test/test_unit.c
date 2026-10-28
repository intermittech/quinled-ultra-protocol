/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The sender library's host rules, frame by frame (wiki: Host behaviour, Timing and clock): the preamble, 3 copies,
 * first copies first, a newer command replacing a queued one, the refresh at rest, NOP, every 16th slot a refresh,
 * TIME from the frame's bits, start offsets, group starts, CONFIG slots, clearing unused lights (and their refresh
 * rotation, round 11), power-on, resync, streams, long-running starts, the pool, argument checks and the bit encoders;
 * and what review round 7 asked for: the queue order's and the stream history's wrap, the return channel's answer
 * decoding, the encoders' output decoded on its own against TIME, stored-level queue semantics, the refusals.
 */
#include <stdio.h>
#include <string.h>

#include "ultra_sender.h"
#include "ultra_sender_rc.h"

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

#define NB 64u
#define NI 1024u
static ultra_sender_t L, L2;
static ultra_sender_board_t B[NB], B2[NB];
static ultra_sender_item_t IT[NI], IT2[NI];
static uint8_t F[ULTRA_SENDER_FRAME_BYTES(NB)];
static int64_t now;
static const ultra_timing_t TM = ULTRA_TIMING_RECOMMENDED;
static const uint16_t ONE5[5] = { 65535, 65535, 65535, 65535, 65535 };

static void mk(uint16_t n, uint8_t lights, uint8_t clear, uint16_t items)
{
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, n, lights);
    c.clear_unused = clear;
    c.seed = 777;
    CHECK(ultra_sender_init(&L, &c, B, IT, items) == ULTRA_OK, "init");
    now = 1000000;
}

/* one frame if due: now moves past it and its reset gap; else 1 ms on */
static uint32_t fr(void)
{
    const uint32_t n = ultra_sender_next_frame(&L, now, F);
    if (n != 0u) {
        now = L.last_start_us + L.frame_us;
    } else {
        memset(F, 0xEE, sizeof F);               /* no frame: nothing to count */
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

static int is_dark(const uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        if (s[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static int is_nop(const uint8_t *s)
{
    uint8_t n[ULTRA_SLOT_BYTES];
    ultra_slot_nop(n);
    return memcmp(n, s, ULTRA_SLOT_BYTES) == 0;
}

static ultra_values_t val(uint16_t m)
{
    ultra_values_t v;
    v.m = m;
    memcpy(v.a, ONE5, sizeof v.a);
    memcpy(v.b, ONE5, sizeof v.b);
    return v;
}

static void def_mono(uint16_t board, uint8_t light)
{
    ultra_light_def_t d;
    ultra_light_def_init(&d);
    d.roles[light] = ULTRA_ROLE_BRIGHTNESS;
    CHECK(ultra_sender_light_def(&L, board, light, &d) == ULTRA_OK, "light_def");
}

/* the frames until nothing is queued */
static uint32_t drain(void)
{
    uint32_t n = 0;
    while (ultra_sender_busy(&L) && n < 100000u) {
        n += fr() != 0u;
    }
    return n;
}

static int hdr_ok(void)
{
    uint32_t t;
    return ultra_parse_header(F + ultra_header_offset(L.n), &t) == 0 && ultra_header_flags(F + ultra_header_offset(L.n)) == 0u;
}

/* ------------------------------------------------------------------------------------------------- tests */
static void test_preamble(void)
{
    uint16_t n;
    for (n = 1; n <= 8u; n = (uint16_t)(n * 4u)) {
        uint32_t i, dark = 0;
        mk(n, 1, 0, NI);
        def_mono(0, 0);
        for (i = 0; i < ultra_preamble_frames(n); i++) {
            CHECK(fr() == ultra_frame_bytes(n), "preamble frame length");
            dark += is_dark(slot(0)) && is_dark(slot((uint16_t)(n - 1u))) && hdr_ok();
        }
        CHECK(dark == 4u * n + 2u, "%u boards: %u DARK frames with a header, 4 N + 2 expected", n, dark);
        fr();
        CHECK(slot(0)[0] == ULTRA_LIGHT_DEF && ultra_slot_check(slot(0)) == 0,
              "%u boards: the command queued before goes out right after the preamble", n);
    }
}

static void test_copies_and_order(void)
{
    ultra_values_t v1 = val(1000), v2 = val(2000), v3 = val(3000);
    uint8_t seq[8], light[8];
    uint32_t i, k;
    mk(1, 2, 0, NI);
    pre();
    /* 3 copies, in 3 frames */
    ultra_sender_set_at(&L, 0, 0, &v1, 0, 0, now + 100000);
    for (i = 0, k = 0; i < 6u; i++) {
        fr();
        k += (slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET;
        if (i < 3u) {
            seq[i] = slot(0)[2];
        }
    }
    CHECK(k == 3u && seq[0] == seq[1] && seq[1] == seq[2], "a command goes out 3 times with one SEQ (%u)", k);
    CHECK(!ultra_sender_busy(&L), "then the queue is empty");
    /* first copies first: A, B -> A B A B A B */
    ultra_sender_set_at(&L, 0, 0, &v1, 0, 0, now + 100000);
    ultra_sender_set_at(&L, 0, 1, &v2, 0, 0, now + 100000);
    for (i = 0; i < 6u; i++) {
        fr();
        light[i] = (uint8_t)(slot(0)[1]);
    }
    CHECK(light[0] == 0 && light[1] == 1 && light[2] == 0 && light[3] == 1 && light[4] == 0 && light[5] == 1,
          "first copies first, oldest first: %u %u %u %u %u %u", light[0], light[1], light[2], light[3], light[4],
          light[5]);
    /* a newer command replaces a queued one and keeps its place */
    ultra_sender_set_at(&L, 0, 0, &v1, 0, 0, now + 100000);
    ultra_sender_set_at(&L, 0, 1, &v2, 0, 0, now + 100000);
    ultra_sender_set_at(&L, 0, 0, &v3, 0, 0, now + 100000);
    for (i = 0, k = 0; i < 6u; i++) {
        fr();
        light[i] = (uint8_t)(slot(0)[1]);
        k += (slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET && ultra_get16(slot(0) + 3) == 1000u;
    }
    CHECK(k == 0u, "the replaced command never goes out");
    CHECK(light[0] == 0 && light[1] == 1, "the replacement keeps its place (light %u first)", light[0]);
    CHECK(!ultra_sender_busy(&L), "3 copies each, then empty");
}

static void test_refresh(void)
{
    uint32_t i, defs = 0, sets = 0, nops = 0;
    ultra_values_t v = val(5000);
    uint8_t seq_set = 0;
    mk(2, 1, 0, NI);
    pre();
    def_mono(0, 0);
    ultra_sender_set(&L, 0, 0, &v, 500, 0, now);
    drain();
    /* at rest: a frame every 200 ms, the state repeated with its SEQ; board 1 has nothing: NOP */
    {
        const int64_t t0 = ultra_sender_due_us(&L);
        CHECK(ultra_sender_next_frame(&L, t0 - 1, F) == 0u, "no frame before the refresh interval");
        CHECK(ultra_sender_next_frame(&L, t0, F) != 0u, "a frame at the refresh interval");
        now = t0 + 1000;
    }
    for (i = 0; i < 20u; i++) {
        now += 200000;
        CHECK(fr() != 0u, "a refresh frame every 200 ms");
        defs += slot(0)[0] == ULTRA_LIGHT_DEF;
        if ((slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET) {
            sets++;
            CHECK(seq_set == 0u || slot(0)[2] == seq_set, "the refresh keeps the SEQ");
            seq_set = slot(0)[2];
        }
        nops += is_nop(slot(1));
    }
    CHECK(defs == 10u && sets == 10u, "round robin: LIGHT_DEF %u, SET %u of 20", defs, sets);
    CHECK(nops == 20u, "a board with nothing gets NOP (%u)", nops);
}

static void test_refresh_in_stream(void)
{
    uint32_t i, refresh = 0;
    ultra_values_t v = val(100);
    mk(1, 1, 0, NI);
    pre();
    def_mono(0, 0);
    drain();
    /* a host-side effect: a new step every frame, 64 frames */
    for (i = 0; i < 64u; i++) {
        v.m = (uint16_t)(100u + i);
        ultra_sender_stream(&L, 0, 0, &v, 20, now);
        fr();
        refresh += slot(0)[0] == ULTRA_LIGHT_DEF;
    }
    /* refresh slots at 16, 32, 48, 64: the list alternates LIGHT_DEF and the SET */
    CHECK(refresh == 2u, "every 16th slot of a busy board is a refresh (LIGHT_DEF in %u of 64)", refresh);
}

static void test_time(void)
{
    uint32_t i;
    mk(8, 5, 0, NI);
    pre();
    for (i = 0; i < 8u; i++) {
        ultra_values_t v = val((uint16_t)(i * 6000u + 77u));
        def_mono((uint16_t)i, (uint8_t)(i % 5u));
        ultra_sender_set(&L, (uint16_t)i, (uint8_t)(i % 5u), &v, 1000, ULTRA_SET_SMOOTH, now);
    }
    for (i = 0; i < 12u; i++) {
        const int64_t start = now;
        uint32_t n = fr(), t;
        const int64_t time = ultra_sender_last_time_us(&L);
        const int64_t wire = (int64_t)((ultra_wire_ns(&TM, F, n) + 500u) / 1000u);
        if (n == 0u) {
            now += 200000;
            continue;
        }
        CHECK(ultra_parse_header(F + ultra_header_offset(8), &t) == 0 && t == (uint32_t)time, "header TIME");
        CHECK(time - (start + wire) >= -2 && time - (start + wire) <= 2,
              "TIME = start + the frame's bits on the wire (%lld us off)", (long long)(time - (start + wire)));
    }
}

static void test_start_offsets(void)
{
    ultra_values_t v = val(40000);
    int64_t start;
    uint32_t i, k = 0;
    mk(1, 2, 0, NI);
    pre();
    /* one absolute start, re-stamped per frame */
    start = now + 50000;
    ultra_sender_set_at(&L, 0, 0, &v, 1000, 0, start);
    for (i = 0; i < 3u; i++) {
        int16_t off;
        int64_t want;
        now += 7000;                             /* frames 7 ms apart */
        fr();
        off = (int16_t)ultra_get16(slot(0) + ULTRA_PAYLOAD + 25);
        want = start - ultra_sender_last_time_us(&L);
        CHECK((int64_t)off * 1000 - want >= -500 && (int64_t)off * 1000 - want <= 500,
              "copy %u: start offset %d ms for %lld us", i, off, (long long)want);
        k += ultra_slot_check(slot(0)) == 0;
    }
    CHECK(k == 3u, "each copy resealed");
    /* a late copy: negative */
    ultra_sender_set_at(&L, 0, 0, &v, 1000, 0, now - 20000);
    fr();
    CHECK((int16_t)ultra_get16(slot(0) + ULTRA_PAYLOAD + 25) <= -20, "a late copy has a negative offset");
    drain();

}

static void test_long_starts(void)
{
    ultra_values_t v = val(40000);
    mk(1, 2, 0, NI);
    pre();
    /* a 60 s transition that started 40 s ago, to a board that missed it: -32768 ms, duration shortened */
    ultra_sender_set_at(&L, 0, 0, &v, 60000, 0, now - 40000000);
    fr();
    CHECK((int16_t)ultra_get16(slot(0) + ULTRA_PAYLOAD + 25) == -32768 &&
          ultra_get24(slot(0) + ULTRA_PAYLOAD + 22) >= 52700u && ultra_get24(slot(0) + ULTRA_PAYLOAD + 22) <= 52800u,
          "a SET 40 s back: -32768 ms and 60 - 7.2 s left (%u)", (unsigned)ultra_get24(slot(0) + ULTRA_PAYLOAD + 22));
    drain();

}

/* Independent stored levels survive refresh, dedup and clearing without effects. */
static void test_scene_levels(void)
{
    ultra_values_t power = val(12000), loss = val(23000);
    unsigned i, seen[2] = {0,0};
    uint8_t seq[2] = {0,0};
    mk(1, 1, 0, NI);
    pre();
    CHECK(ultra_sender_scene(&L,0,0,0,3,&power)==ULTRA_OK, "power-on level");
    CHECK(ultra_sender_scene(&L,0,0,1,5,&loss)==ULTRA_OK, "signal-loss level");
    CHECK(ultra_sender_items_used(&L)==2u, "both selectors have independent items");
    for(i=0;i<6u;++i) {
        unsigned sel;
        fr();
        CHECK(slot(0)[0]==ULTRA_SCENE, "scene command");
        sel=slot(0)[3];
        CHECK(sel<2u && ultra_slot_check(slot(0))==0, "selector and CRC");
        if(sel<2u) {
            CHECK(ultra_get16(slot(0)+5)==(uint32_t)(sel?loss.m:power.m) && slot(0)[4]==(sel?5:3), "independent levels and fades");
            if(seen[sel])CHECK(seq[sel]==slot(0)[2], "scene repeats retain SEQ");
            seq[sel]=slot(0)[2];++seen[sel];
        }
    }
    CHECK(seen[0]==3u && seen[1]==3u, "three copies per selector");
    CHECK(seq[0]!=seq[1], "both levels use distinct SEQs in the receiver's shared key");
    CHECK(ultra_sender_scene(&L,0,0,0,3,&power)==ULTRA_OK &&
          ultra_sender_scene(&L,0,0,1,5,&loss)==ULTRA_OK && !ultra_sender_busy(&L), "both selectors deduplicate independently");
    CHECK(ultra_sender_scene(&L,0,0,2,0,&power)==ULTRA_ERR_ARG &&
          ultra_sender_scene(&L,0,0,255,0,&power)==ULTRA_ERR_ARG, "reserved scene selectors refused");
    CHECK(ultra_sender_scene(&L,0,0,0,2,NULL)==ULTRA_OK, "NULL level stores dark");
    fr();
    CHECK(slot(0)[3]==0 && ultra_get16(slot(0)+5)==0 && slot(0)[2]!=(uint8_t)seq[0], "power-on dark is a new command");
    CHECK(!ultra_command_known(3) && !ultra_command_known(4) && !ultra_command_known(10), "analog opcodes unknown");
    CHECK(ultra_sender_preset_seq(&L,0,0,3,9)==ULTRA_ERR_ARG, "reserved opcode cannot allocate sender state");
    drain();
    for(i=0;i<300u;++i) {
        unsigned tries;
        power.m=(uint16_t)(12000u+i);
        CHECK(ultra_sender_scene(&L,0,0,0,3,&power)==ULTRA_OK, "changed power-on level");
        for(tries=0;tries<4u;++tries) {
            fr();
            if(slot(0)[0]==ULTRA_SCENE && slot(0)[3]==0)break;
        }
        CHECK(slot(0)[0]==ULTRA_SCENE && slot(0)[3]==0 && slot(0)[2]!=seq[1] &&
              ultra_get16(slot(0)+5)==power.m, "first changed level differs from unchanged level, including SEQ wrap");
        drain();
    }
}

static void test_group(void)
{
    ultra_sender_group_t g;
    ultra_sender_config_t c;
    int64_t a, b, d;
    mk(8, 5, 0, NI);
    ultra_sender_config_init(&c, 8, 1);
    ultra_sender_init(&L2, &c, B2, IT2, NI);
    ultra_sender_group_init(&g);
    ultra_sender_join_group(&L, &g);
    ultra_sender_join_group(&L2, &g);
    a = ultra_sender_start_time(&L, 0, now);
    b = ultra_sender_start_time(&L2, 3, now + 2000);
    d = ultra_sender_start_time(&L, 7, now + 4000);
    CHECK(a == b && b == d, "commands 4 ms apart on two lines share one start (%lld %lld %lld)", (long long)a,
          (long long)b, (long long)d);
    CHECK(a - now >= (int64_t)ultra_sender_frame_us(&L2), "the start leaves the longest line time (%lld us)",
          (long long)(a - now));
    d = ultra_sender_start_time(&L, 0, a + 1000);
    CHECK(d > a, "a command after the group's start opens a new group");
}

static void test_config(void)
{
    uint32_t i, k, cfg = 0;
    uint8_t seq0;
    mk(1, 1, 0, NI);
    pre();
    for (i = 1; i <= 10u; i++) {
        /* (valid values: PWM period 0 or >= 1440, minimum pulse 5-100, timeout 5-600, the modes 0-2) */
        const uint16_t v = i == 2u ? 2000u : i == 4u || i == 6u || i == 9u ? (uint16_t)(i * 10u) : (uint16_t)(i % 3u);
        CHECK(ultra_sender_config(&L, 0, (uint8_t)i, v) == ULTRA_OK, "config %u = %u", i, v);
    }
    for (k = 0; k < 6u; k++) {
        fr();
        if (slot(0)[0] == ULTRA_CONFIG) {
            cfg++;
        }
    }
    CHECK(cfg == 6u, "10 ids: 2 CONFIG slots x 3 copies (%u)", cfg);
    fr();
    CHECK(!ultra_sender_busy(&L), "then nothing");
    /* the slots: ids 1-9, then 10 */
    now += 300000;
    for (k = 0; k < 4u; k++) {
        fr();
        if (slot(0)[0] == ULTRA_CONFIG && slot(0)[3] == 1u) {
            CHECK(slot(0)[3 + 8 * 3] == 9u && ultra_get16(slot(0) + 4 + 8 * 3) == 90u &&
                  ultra_get16(slot(0) + 4 + 1 * 3) == 2000u, "first slot: ids 1-9");
        } else if (slot(0)[0] == ULTRA_CONFIG) {
            CHECK(slot(0)[3] == 10u && slot(0)[6] == 0u, "second slot: id 10 alone");
        }
        now += 200000;
    }
    /* unchanged: nothing; changed: both slots with new SEQs */
    ultra_sender_config(&L, 0, 3, 0);
    CHECK(!ultra_sender_busy(&L), "an unchanged value queues nothing");
    ultra_sender_config(&L, ULTRA_SENDER_ALL, 3, 1);
    CHECK(ultra_sender_busy(&L), "a changed value queues the board's CONFIG");
    fr();
    seq0 = slot(0)[2];
    fr();
    CHECK(slot(0)[0] == ULTRA_CONFIG && slot(0)[2] != seq0, "each CONFIG slot its own SEQ");
    CHECK(ultra_sender_config(&L, 0, 0, 1) == ULTRA_ERR_ARG && ultra_sender_config(&L, 0, ULTRA_SENDER_CFG_COUNT, 1) ==
          ULTRA_ERR_ARG && ultra_sender_config(&L, 1, 1, 1) == ULTRA_ERR_ARG, "undefined ids and boards refused");
    CHECK(ultra_sender_config(&L, 0, ULTRA_CFG_PWM_PRESET, 15) == ULTRA_ERR_ARG &&
          ultra_sender_config(&L, 0, ULTRA_CFG_PWM_PERIOD, 1000) == ULTRA_OK &&
          ultra_sender_config(&L, 0, ULTRA_CFG_LOSS_TIMEOUT, 4) == ULTRA_ERR_ARG &&
          ultra_sender_config(&L, 0, ULTRA_CFG_BOOT_PROTOCOL, 4) == ULTRA_ERR_ARG &&
          ultra_sender_config(&L, 0, ULTRA_CFG_PWM_PERIOD, 1440) == ULTRA_OK &&
          ultra_sender_config(&L, 0, ULTRA_CFG_BOARD_MAX, 65535) == ULTRA_ERR_ARG, "values outside an id's range refused");
    /* 0.2.5: signal loss 3 (scene), the retry policy (every value) */
    CHECK(ultra_sender_config(&L, 0, ULTRA_CFG_LOSS, 3) == ULTRA_OK &&
          ultra_sender_config(&L, 0, ULTRA_CFG_LOSS, 4) == ULTRA_ERR_ARG &&
          ultra_sender_config(&L, 0, ULTRA_CFG_RETRY, 0) == ULTRA_OK &&
          ultra_sender_config(&L, 0, ULTRA_CFG_RETRY, 0xFFFF) == ULTRA_OK &&
          ultra_config_value_ok(ULTRA_CFG_RETRY, 0x010A) && !ultra_config_value_ok(ULTRA_SENDER_CFG_COUNT, 0),
          "0.2.5: signal loss 0-3, CONFIG 24 any value, ids from ULTRA_SENDER_CFG_COUNT refused");
    {
        volatile uint32_t n1 = 1, n2 = 2, n3 = 3;          /* (no constant expressions) */
        CHECK(ULTRA_SENDER_RETRY(10u * n1, n1) == 0x010Au && ULTRA_SENDER_RETRY(n2, n3) == 0x0302u &&
              ULTRA_SENDER_RETRY(255u * n1, n1 - 1u) == 0x00FFu && ULTRA_SENDER_RETRY(256u * n1, 256u * n1) == 0u,
              "ULTRA_SENDER_RETRY: count | interval << 8 (8 bits each)");
    }
}

static void test_clear_unused(void)
{
    uint32_t i, defs = 0, scenes = 0, fx = 0;
    uint8_t seq = 0;
    uint8_t level_seq[5][2] = {{0}}, seen[5][2] = {{0}};
    mk(1, 2, 1, NI);
    pre();
    for (i = 0; i < 40u && ultra_sender_busy(&L); i++) {
        fr();
        if (slot(0)[0] == ULTRA_LIGHT_DEF) {
            defs++;
            CHECK((slot(0)[1]) >= 2u && slot(0)[3] == 0u && slot(0)[4] == 0u && slot(0)[5] == 0u,
                  "an empty LIGHT_DEF for lights 2-4");
            seq = slot(0)[2];
        }
        if(slot(0)[0]==ULTRA_SCENE && slot(0)[1]<5u && slot(0)[3]<2u) {
            uint8_t light=slot(0)[1], mode=slot(0)[3];
            if(seen[light][mode])CHECK(level_seq[light][mode]==slot(0)[2], "clear repeats retain SEQ");
            level_seq[light][mode]=slot(0)[2];seen[light][mode]=1;
        }
        scenes += slot(0)[0] == ULTRA_SCENE && slot(0)[3] == 0u;
        fx += slot(0)[0] == ULTRA_SCENE && slot(0)[3] == 1u;
    }
    CHECK(defs >= 9u && scenes >= 9u && fx >= 9u && i <= 29u, "3 unused lights cleared, 3 copies (%u %u %u in %u "
          "frames)", defs, scenes, fx, i);
    CHECK(!ultra_sender_busy(&L), "then the refresh only");
    for(i=2;i<5u;++i)CHECK(seen[i][0] && seen[i][1] && level_seq[i][0]!=level_seq[i][1],
                          "both unused levels have distinct SEQs");
    CHECK(ultra_sender_light_def(&L, 0, 2, 0) == ULTRA_ERR_ARG, "a light index not in use is refused");
    /* light 2 in use again: its first LIGHT_DEF never takes the clearing SEQ */
    ultra_sender_set_lights(&L, 0, 3);
    ultra_sender_preset_seq(&L, 0, 2, ULTRA_LIGHT_DEF, seq);
    def_mono(0, 2);
    for (i = 0; i < 20u; i++) {
        fr();
        if (slot(0)[0] == ULTRA_LIGHT_DEF && (slot(0)[1]) == 2u && slot(0)[4] != 0u) {
            CHECK(slot(0)[2] != seq, "a cleared light's LIGHT_DEF avoids the clearing SEQ");
            break;
        }
    }
    CHECK(i < 20u, "light 2's LIGHT_DEF goes out");
}

/* round 11, the refresh rotation: a 1-light board's clearing slots (4 unused lights x 3) in the first 3 refresh rounds,
 * then in every 4th: at rest its SET every 5 refresh frames (a round without them: LIGHT_DEF + SET; 4 rounds + 12
 * clearing slots = 20 frames), not every 14 (test_rotation: round by round) */
static void test_refresh_rotation(void)
{
    uint32_t i, sets = 0, clears = 0, late_sets = 0, late_clears = 0, run = 0, longest = 0;
    ultra_values_t v = val(5000);
    mk(1, 1, 1, NI);
    pre();
    def_mono(0, 0);
    ultra_sender_set(&L, 0, 0, &v, 0, 0, now);
    drain();
    for (i = 0; i < 400u; i++) {
        const uint8_t *s = slot(0);
        int clr;
        now = ultra_sender_due_us(&L);
        CHECK(fr() != 0u, "a refresh frame");
        clr = (s[1]) >= 1u && (s[1]) < ULTRA_SENDER_DEFAULT_LIGHTS;
        sets += (s[0] & ULTRA_TYPE_MASK) == ULTRA_SET;
        clears += clr;
        late_sets += i >= 200u && (s[0] & ULTRA_TYPE_MASK) == ULTRA_SET;
        late_clears += i >= 200u && clr;
        run = clr ? run + 1u : 0u;
        longest = run > longest ? run : longest;
    }
    CHECK(late_sets == 40u && late_clears == 120u, "at rest: 40 SETs and 120 clearing slots in 200 refresh frames (%u, "
          "%u)", late_sets, late_clears);
    CHECK(sets >= 75u && clears >= 230u && longest == 12u, "from the start: %u SETs in 400 frames, %u clearing slots, "
          "12 in a row (%u)", sets, clears, longest);
}

/* 0.2.6, the planned power-off notice: every board's slot, 3 copies with one SEQ per board, >= 50 ms apart, before
 * anything else and nothing after it until the wake; the cut 200 ms after the first copy (not before the last one's
 * end); a retimed copy moves both; the wake: the short preamble, then everything again */
static void notice_copies(int64_t gap_us, int64_t *first, int64_t *third_end, uint8_t seq[3], int *ok)
{
    uint32_t k, b;
    for (k = 0; k < 3u; k++) {
        const int64_t at = now;
        const uint32_t n = ultra_sender_next_frame(&L, at, F);
        *first = k == 0u ? at : *first;
        for (b = 0; b < 3u; b++) {
            const uint8_t *s = slot((uint16_t)b);
            *ok &= n == ultra_frame_bytes(3) && s[0] == ULTRA_IDENTIFY && (s[1]) == ULTRA_TARGET_BOARD &&
                   s[ULTRA_PAYLOAD] == ULTRA_IDENT_POWER_OFF && s[ULTRA_PAYLOAD + 1] == 0u && ultra_slot_check(s) == 0 &&
                   (k == 0u || s[2] == seq[b]);
            seq[b] = s[2];
        }
        *ok &= k == 2u || (ultra_sender_next_frame(&L, at + 49999, F) == 0 && ultra_sender_due_us(&L) == at + 50000);
        *third_end = ultra_sender_last_time_us(&L);
        now = at + gap_us;
    }
}

static void test_power_off(void)
{
    ultra_values_t v = val(20000);
    ultra_rc_request_t rq;
    int64_t first, third_end, cut;
    uint8_t seq[3], seq2[3];
    uint32_t i, b, dark = 0, sets = 0;
    int ok = 1;
    mk(3, 1, 0, NI);
    pre();
    for (b = 0; b < 3u; b++) {
        def_mono((uint16_t)b, 0);
    }
    for (i = 0; i < 6u; i++) {
        (void)fr();
    }
    (void)ultra_sender_identify(&L, ULTRA_SENDER_ALL, ULTRA_IDENT_ON, 0);   /* queued: gives way to the notice */
    CHECK(ultra_sender_power_off_at(&L) == INT64_MIN && ultra_sender_identify(&L, 0, ULTRA_IDENT_POWER_OFF, 0) ==
          ULTRA_ERR_ARG, "power-off: none yet (INT64_MIN); not through ultra_sender_identify");
    CHECK(ultra_sender_power_off(&L) == ULTRA_OK && ultra_sender_power_off(&L) == ULTRA_ERR_ARG &&
          ultra_sender_busy(&L) && ultra_sender_power_off_at(&L) == INT64_MAX,
          "power-off: given once (again: refused), the line busy, the cut not known before the first copy");
    notice_copies(50000, &first, &third_end, seq, &ok);
    CHECK(ok, "power-off: 3 frames of IDENTIFY 7 in every slot, one SEQ per board, the queued IDENTIFY dropped, "
          "nothing before 50 ms");
    cut = ultra_sender_power_off_at(&L);
    CHECK(cut == first + 200000 && third_end < cut, "power-off: the cut 200 ms after the first copy (%lld us), after "
          "the last one's end (%lld us)", (long long)(cut - first), (long long)(third_end - first));
    (void)ultra_sender_set(&L, 0, 0, &v, 500, 0, now);
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_1M);
    (void)ultra_rc_request_add(&rq, 0, ULTRA_PAGE_STATUS, 0, 0);
    CHECK(ultra_sender_next_frame(&L, now + 3000000, F) == 0 && !ultra_sender_busy(&L) &&
          ultra_sender_due_us(&L) == INT64_MAX && ultra_sender_rc_frame(&L, &rq, now + 3000000, F) == 0 &&
          ultra_sender_power_off_at(&L) == cut, "power-off: then nothing (a SET waits, no POLL), the cut as it was");
    /* the wake: the short preamble, then the definitions and the SET given meanwhile */
    now += 3000000;
    ultra_sender_power(&L, 0, now);
    now += 1500000;
    ultra_sender_power(&L, 1, now);
    CHECK(ultra_sender_power_off_at(&L) == INT64_MIN && ultra_sender_busy(&L), "wake: no notice, the line busy");
    for (i = 0; i < 30u; i++) {
        (void)fr();
        dark += i < 10u && is_dark(slot(0));
        sets += i >= 10u && (slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET && ultra_get16(slot(0) + ULTRA_PAYLOAD) == 20000u;
    }
    CHECK(dark == 10u && sets >= 1u, "wake: the 10-frame preamble, then the SET given while the line was off (%u)", sets);
    /* late copies: the cut waits for the last one's end; a retimed first copy moves the cut and the spacing */
    CHECK(ultra_sender_power_off(&L) == ULTRA_OK, "power-off again after the wake");
    ok = 1;
    notice_copies(130000, &first, &third_end, seq2, &ok);
    CHECK(ok && seq2[0] != seq[0] && ultra_sender_power_off_at(&L) == third_end && third_end > first + 200000,
          "power-off, copies 130 ms apart: a new SEQ, the cut at the last copy's end (%lld us after the first)",
          (long long)(third_end - first));
    ultra_sender_power(&L, 1, now);
    (void)ultra_sender_power_off(&L);
    now += 1000000;
    (void)ultra_sender_next_frame(&L, now, F);
    (void)ultra_sender_retime(&L, now + 7000, F);
    CHECK(ultra_sender_due_us(&L) == now + 57000 && ultra_sender_next_frame(&L, now + 56999, F) == 0,
          "power-off: a retimed copy moves the next one's time");
    for (i = 0; i < 2u; i++) {
        (void)ultra_sender_next_frame(&L, now + 57000 + 50000 * (int64_t)i, F);
    }
    CHECK(ultra_sender_power_off_at(&L) == now + 207000, "power-off: ... and the cut (200 ms after the retimed first)");
    /* before the preamble is out: the notice goes first */
    mk(2, 1, 0, NI);
    (void)ultra_sender_power_off(&L);
    (void)fr();
    CHECK(slot(0)[0] == ULTRA_IDENTIFY && slot(1)[ULTRA_PAYLOAD] == ULTRA_IDENT_POWER_OFF,
          "power-off during the preamble: the notice first");
}

static void test_power_and_resync(void)
{
    ultra_values_t v = val(30000);
    uint32_t i, dark = 0;
    uint8_t seq;
    int64_t t_set;
    mk(8, 1, 0, NI);
    def_mono(0, 0);
    ultra_sender_set(&L, 0, 0, &v, 2000, 0, now);
    pre();
    drain();
    seq = 0;
    /* supply switch-on after a full preamble: 10 frames */
    ultra_sender_power(&L, 0, now);
    ultra_sender_power(&L, 1, now);
    for (i = 0; i < 82u; i++) {
        fr();
        if (is_dark(slot(0))) {
            dark++;
        } else if ((slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET && seq == 0u) {
            seq = slot(0)[2];
            t_set = ultra_sender_last_time_us(&L) + (int16_t)ultra_get16(slot(0) + ULTRA_PAYLOAD + 25) * 1000;
            CHECK(t_set > now - 20000, "the replay restarts the transition (%lld us ahead)", (long long)(t_set - now));
        }
    }
    CHECK(dark == 10u, "after a supply switch-on: 10 preamble frames (%u)", dark);
    CHECK(seq != 0u, "then the state again");
    /* a resync: the full one */
    drain();
    ultra_sender_resync(&L, now);
    for (i = 0, dark = 0; i < 100u; i++) {
        fr();
        dark += is_dark(slot(0));
    }
    CHECK(dark == 34u, "a resync: 4 N + 2 = 82 preamble frames (%u)", dark);
    /* power on before any full preamble reached powered boards: the full one */
    mk(8, 1, 0, NI);
    ultra_sender_power(&L, 0, now);
    pre();
    ultra_sender_power(&L, 1, now);
    for (i = 0, dark = 0; i < 100u; i++) {
        fr();
        dark += is_dark(slot(0));
    }
    CHECK(dark == 34u, "power on, no full preamble with power yet: the full one (%u)", dark);
}

static void test_stream(void)
{
    ultra_values_t v = val(1000);
    uint32_t i, sets = 0;
    mk(1, 5, 0, NI);
    pre();
    /* sent once, never before it arrives */
    ultra_sender_stream(&L, 0, 0, &v, 20, now);
    for (i = 0; i < 4u; i++) {
        fr();
        if ((slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET) {
            sets++;
            CHECK((int16_t)ultra_get16(slot(0) + ULTRA_PAYLOAD + 25) >= 0, "a streamed SET never starts in the past");
            CHECK(ultra_get24(slot(0) + ULTRA_PAYLOAD + 22) == 20u, "its glide");
        }
        now += 50000;
    }
    CHECK(sets >= 1u && !ultra_sender_busy(&L), "a streamed SET goes out once");
    /* behind: 5 lights streaming every 10 ms on one board; each light's glide stretches to the time between its steps */
    for (i = 0; i < 40u; i++) {
        uint8_t l2;
        for (l2 = 0; l2 < 5u; l2++) {
            v.m = (uint16_t)(i * 100u + l2);
            ultra_sender_stream(&L, 0, l2, &v, 10, now);
        }
        fr();
        now += 9000;
    }
    CHECK((slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET && ultra_get24(slot(0) + ULTRA_PAYLOAD + 22) >= 40u,
          "a step that waited glides as long as the light's steps are apart (%u ms)",
          (unsigned)ultra_get24(slot(0) + ULTRA_PAYLOAD + 22));
}

static void test_dedupe_and_args(void)
{
    ultra_light_def_t d;
    ultra_values_t v = val(1);
    mk(2, 2, 0, NI);
    pre();
    def_mono(0, 0);
    drain();
    def_mono(0, 0);
    CHECK(!ultra_sender_busy(&L), "an unchanged LIGHT_DEF queues nothing");
    ultra_sender_scene(&L, 0, 0, 1, 3, &v);
    drain();
    ultra_sender_scene(&L, 0, 0, 1, 3, &v);
    CHECK(!ultra_sender_busy(&L), "an unchanged signal-loss SCENE queues nothing");
    ultra_light_def_init(&d);
    CHECK(ultra_sender_light_def(&L, 2, 0, &d) == ULTRA_ERR_ARG, "board out of range");
    CHECK(ultra_sender_light_def(&L, 0, 2, &d) == ULTRA_ERR_ARG, "light out of range");
    CHECK(ultra_sender_set(&L, 0, 0, &v, 0, 0x04, now) == ULTRA_ERR_ARG, "reserved SET flag");
    CHECK(ultra_sender_identify(&L, 0, 6, 0) == ULTRA_ERR_ARG && ultra_sender_identify(&L, 0, ULTRA_IDENT_CAL, 3) ==
          ULTRA_ERR_ARG, "reserved IDENTIFY values");
    CHECK(ultra_sender_scene(&L, 0, 0, 2, 0, &v) == ULTRA_ERR_ARG, "reserved SCENE mode");
    {
        ultra_sender_config_t c;
        ultra_sender_config_init(&c, 65, 1);
        CHECK(ultra_sender_init(&L2, &c, B2, IT2, NI) == ULTRA_ERR_ARG, "65 boards refused");
        ultra_sender_config_init(&c, 1, 1);
        c.timing.t0h_ns = 400;
        CHECK(ultra_sender_init(&L2, &c, B2, IT2, NI) == ULTRA_ERR_ARG, "timing outside the spec refused");
    }
}

static void test_identify_and_retime(void)
{
    uint32_t i, k = 0, n;
    uint8_t copy[ULTRA_SENDER_FRAME_BYTES(3)];
    mk(3, 1, 0, NI);
    pre();
    ultra_sender_identify(&L, ULTRA_SENDER_ALL, ULTRA_IDENT_DARK, 20);
    CHECK(ultra_sender_identify_pending(&L, 2), "pending");
    n = fr();
    memcpy(copy, F, n);
    /* the frame couldn't go out: again, new TIME, same slots, nothing more taken */
    now += 5000;
    ultra_sender_retime(&L, now, F);
    CHECK(memcmp(copy, F, 96) == 0 && memcmp(copy + 96, F + 96, 8) != 0, "retime: same slots, a new header");
    for (i = 0; i < 4u; i++) {
        fr();
        k += slot(2)[0] == ULTRA_IDENTIFY && slot(2)[3] == ULTRA_IDENT_DARK && slot(2)[4] == 20u;
    }
    CHECK(k == 2u && !ultra_sender_identify_pending(&L, 2), "IDENTIFY 3 copies (1 + %u), not refreshed", k);
}

static void test_encoders(void)
{
    static const uint8_t bytes[2]={0xa5,1};
    ultra_timing_t tm=ULTRA_TIMING_RECOMMENDED;
    ultra_rmt_t r; ultra_pulse_t pulses[16];
    uint8_t spi[24];uint32_t pos=0,words[17],n;int done=0;
    CHECK(ultra_encode_pulses(&tm,bytes,2,&pos,pulses,16)==16 && pos==16,"exact tick pulses");
    CHECK(pulses[0].high_ticks==9 && pulses[1].high_ticks==3 && pulses[15].low_ticks==4803,"exact HIGH/LOW and reset");
    CHECK(ultra_wire_ns(&tm,bytes,2)==19688,"last falling edge rational ns");
    CHECK(ultra_rmt_init(&r,&tm,40000000)==0 && r.bit0==ULTRA_RMT_WORD(12,1,38,0) && r.bit1==ULTRA_RMT_WORD(38,1,12,0),"40 MHz separation rounding");
    n=ultra_rmt_encode(&r,bytes,2,0,words,17,&done);
    CHECK(n==17 && done && words[16]==r.reset,"RMT includes 500 us reset");
    CHECK(ultra_spi_encode(bytes,2,spi)==24 && spi[0]==0xff && spi[1]==0x8e,"9.6 MHz 12 samples/bit");
    CHECK(ultra_rmt_init(&r,&tm,20000000)!=0,"unqualified RMT clock refused");
}

static void test_cpp_wrapper(void)
{
    /* (compiled as C: the wrapper is checked by test_cpp.cpp) */
    CHECK(ultra_frame_bytes(50) == ULTRA_SENDER_FRAME_BYTES(50), "frame bytes for 50 boards");
    CHECK(ultra_frame_max_us(&TM, ULTRA_SENDER_FRAME_BYTES(50)) / 1000u == 16u, "50-object format stress frame at 800");
}

/* ------------------------------------------------------------------------------------------------- review round 7:
 * the coverage it asked for */

/* the queue's order counter wraps: the oldest first copy still goes first */
static void test_order_wrap(void)
{
    ultra_values_t v1 = val(1111), v2 = val(2222);
    uint8_t first;
    mk(1, 2, 0, NI);
    pre();
    L.order = 0xFFFFFFFEu;
    ultra_sender_set_at(&L, 0, 1, &v1, 0, 0, now + 100000);   /* order 0xFFFFFFFE */
    L.order = 0xFFFFFFFFu;
    (void)ultra_sender_identify(&L, 0, ULTRA_IDENT_ON, 0);   /* 0xFFFFFFFF */
    ultra_sender_set_at(&L, 0, 0, &v2, 0, 0, now + 100000);   /* 0: wrapped, the newest */
    fr();
    first = (slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET ? (uint8_t)(slot(0)[1]) : 0xFFu;
    fr();
    CHECK(first == 1u && slot(0)[0] == ULTRA_IDENTIFY, "the queue order wraps: oldest first (light %u, then IDENTIFY)",
          first);
}

/* a stream after 72 minutes of nothing: its 32-bit history aliases; the glide stays a glide */
static void test_stream_alias(void)
{
    ultra_values_t v = val(100);
    uint32_t i, glide = 0;
    mk(1, 2, 0, NI);
    pre();
    ultra_sender_stream(&L, 0, 0, &v, 20, now);
    fr();
    now += 0x100000000LL + 5000;                 /* 71.6 min + 5 ms: last_us aliases to 5 ms ago */
    for (i = 0; i < 3u; i++) {
        ultra_sender_stream(&L, 0, 1, &v, 20, now);
        ultra_sender_stream(&L, 0, 0, &v, 20, now);   /* behind light 1's */
        ultra_sender_stream(&L, 0, 0, &v, 20, now);
    }
    for (i = 0; i < 6u; i++) {
        fr();
        if ((slot(0)[0] & ULTRA_TYPE_MASK) == ULTRA_SET && (slot(0)[1]) == 0u) {
            glide = ultra_get24(slot(0) + ULTRA_PAYLOAD + 22);
        }
    }
    CHECK(glide >= 20u && glide <= 1000u, "a stream after 72 min idle: glide %u ms (20 .. 1000)", glide);
}

/* ultra_rc_answer(): the hub's decoding (Return channel, "The hub (guidance)") */
static void answer(uint8_t *a, uint8_t page, uint8_t index, uint8_t seq, uint8_t data0)
{
    uint32_t i;
    memset(a, 0, 32);
    a[0] = ULTRA_RSP_SYNC;
    a[1] = page;
    a[2] = (uint8_t)(index << 4);
    a[3] = seq;
    a[4] = 0x12;
    a[5] = 0x34;
    for (i = 0; i < 24u; i++) {
        a[6 + i] = (uint8_t)(data0 + i);
    }
    ultra_put16(a + 30, ultra_crc16(a, 30));
}

/* 0.2.6 LIFE_SYNC: the POLL's arguments (built by hand from the header's ULTRA_LS_* offsets) in the slot the header's
 * own ultra_slot_poll builds; the answer's accessors */
static void test_life_sync(void)
{
    static const uint8_t life[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint8_t args[23], want[32], a[32];
    ultra_rc_request_t rq;
    uint32_t i, n;
    mk(2, 1, 0, NI);
    pre();
    for (i = 0; i < 1500u; i++) {
        (void)fr();                              /* a second and a half of headers: the chain may poll */
    }
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_500K);
    CHECK(ultra_rc_request_life_sync(&rq, 1, 0xBEEF, NULL, 0, 0) == ULTRA_ERR_ARG && rq.count == 0u,
          "life sync: no ledger bytes: refused");
    CHECK(ultra_rc_request_life_sync(&rq, 1, 0xBEEF, life, 0x01020304u, 0x5A) == ULTRA_OK &&
          ultra_rc_request_life_sync(&rq, 0, 0xBEEF, life, 0, 0) == ULTRA_ERR_ARG && rq.count == 1u,
          "life sync: asked; boards in rising order");
    (void)ultra_sender_preset_seq(&L, 1, ULTRA_TARGET_BOARD, ULTRA_POLL, 0x70);
    n = ultra_sender_rc_frame(&L, &rq, now, F);
    args[0] = 0xBE;
    args[1] = 0xEF;
    memcpy(args + 2, life, 16);
    args[18] = 1;
    args[19] = 2;
    args[20] = 3;
    args[21] = 4;
    args[22] = 0x5A;
    ultra_slot_poll(want, 0x70, 0, 1, ULTRA_PAGE_LIFE_SYNC, ULTRA_RW_RATE_500K, args, sizeof args);
    CHECK(n == ultra_frame_bytes(2) && memcmp(slot(1), want, 32) == 0 && ultra_poll_valid(slot(1)) == 0 &&
          slot(0)[ULTRA_POLL_INDEX] == ULTRA_POLL_RELAY_ONLY,
          "life sync: the POLL as the header builds it (page 0x0E, UID16, LIFETIME 0..15, Wh, RECORDS byte)");
    memset(a, 0, sizeof a);
    a[0] = ULTRA_RSP_SYNC;
    a[1] = ULTRA_PAGE_LIFE_SYNC;
    a[ULTRA_RSP_DATA + 16] = 0x00;
    a[ULTRA_RSP_DATA + 17] = 0x01;
    a[ULTRA_RSP_DATA + 18] = 0x86;
    a[ULTRA_RSP_DATA + 19] = 0xA0;               /* 100,000 Wh */
    a[ULTRA_RSP_DATA + 20] = ULTRA_LS_R_CAPPED;
    a[ULTRA_RSP_DATA + 21] = ULTRA_LS_F_POWERED | ULTRA_LS_F_WH;
    a[ULTRA_RSP_DATA + 22] = 0x12;
    a[ULTRA_RSP_DATA + 23] = 0x34;
    CHECK(ultra_rc_ls_total_wh(a) == 100000u && ultra_rc_ls_result(a) == ULTRA_LS_R_CAPPED &&
          ultra_rc_ls_applied(a) == (ULTRA_LS_F_POWERED | ULTRA_LS_F_WH) && ultra_rc_ls_records(a) == 0x1234u,
          "life sync answer: Wh (DATA 16, u32), result (20), applied (21), RECORDS (22, u16)");
}

static void test_rc_answer(void)
{
    uint8_t a[32], rx[64], ferr[64], got[32];
    /* junk from the turnaround checks, the 0xFF lead byte, the answer */
    answer(a, ULTRA_PAGE_STATUS, 1, 0x42, 0x10);
    memset(ferr, 0, sizeof ferr);
    rx[0] = 0x00;
    rx[1] = 0x3C;
    rx[2] = 0xFF;
    memcpy(rx + 3, a, 32);
    CHECK(ultra_rc_answer(rx, ferr, 35, got) == 0 && memcmp(got, a, 32) == 0 && ultra_rc_index(got) == 1u &&
          ultra_rc_seq(got) == 0x42u && ultra_rc_page(got) == ULTRA_PAGE_STATUS && ultra_rc_uid16(got) == 0x1234u,
          "rc answer: junk, lead byte, answer: found; index, SEQ, page, UID16");
    CHECK(ultra_rc_answer(rx, NULL, 35, got) == 0, "rc answer: without framing flags");
    /* a junk 0xA5 with a framing error before the real SYNC */
    rx[1] = ULTRA_RSP_SYNC;
    ferr[1] = 1;
    CHECK(ultra_rc_answer(rx, ferr, 35, got) == 0 && memcmp(got, a, 32) == 0,
          "rc answer: a 0xA5 with a framing error isn't a SYNC");
    ferr[1] = 0;
    rx[1] = 0x3C;
    /* 0xA5 inside DATA: taken as data (the whole answer after the first SYNC) */
    answer(a, ULTRA_PAGE_POWER, 2, 7, 0xA0);     /* DATA holds 0xA5 */
    memcpy(rx + 3, a, 32);
    CHECK(ultra_rc_answer(rx, ferr, 35, got) == 0 && memcmp(got, a, 32) == 0, "rc answer: 0xA5 inside DATA");
    /* truncated, a framing error inside, a corrupt CRC, no SYNC */
    CHECK(ultra_rc_answer(rx, ferr, 34, got) != 0, "rc answer: truncated (31 bytes after SYNC): none");
    ferr[3 + 17] = 1;
    CHECK(ultra_rc_answer(rx, ferr, 35, got) != 0, "rc answer: a framing error inside: dropped");
    ferr[3 + 17] = 0;
    rx[3 + 10] ^= 0x01u;
    CHECK(ultra_rc_answer(rx, ferr, 35, got) != 0, "rc answer: a flipped bit (CRC): dropped");
    memset(rx, 0x3C, sizeof rx);
    CHECK(ultra_rc_answer(rx, ferr, 64, got) != 0, "rc answer: no SYNC: none");
    CHECK(ultra_rc_slot_start_us(ULTRA_RW_RATE_1M, 0) == 1000u && ultra_rc_slot_start_us(ULTRA_RW_RATE_1M, 2) == 2000u &&
          ultra_rc_slot_start_us(ULTRA_RW_RATE_250K, 1) == 2505u, "rc slots: widened pitch");
    /* the getters: an IDENTITY part 1 answer's features (0.2.7: six big-endian u32 words), a STATUS answer's reason
     * nibble (0.2.5, beside the limit bits) */
    memset(a, 0, sizeof a);
    a[1] = ULTRA_PAGE_IDENTITY;
    a[ULTRA_RSP_DATA + ULTRA_IDF_FEATURES + 1] = (uint8_t)((ULTRA_FEAT_RETRY | ULTRA_FEAT_STANDBY) >> 16);
    a[ULTRA_RSP_DATA + ULTRA_IDF_FEATURES + 3] = (uint8_t)ULTRA_FEAT_SCENE;
    a[ULTRA_RSP_DATA + ULTRA_IDF_FEATURES + 4 * 5] = 0x80u;     /* bit 191: word 5's most significant */
    a[ULTRA_RSP_DATA + ULTRA_IDF_FEATURES + 4 * 1 + 3] = 0x01u; /* bit 32: word 1's least significant */
    CHECK(ultra_rc_features(a) == (ULTRA_FEAT_RETRY | ULTRA_FEAT_STANDBY | ULTRA_FEAT_SCENE) &&
          (ultra_rc_features(a) & ULTRA_FEAT_STANDBY) != 0u, "rc: IDENTITY part 1, word 0 (big-endian u32), STANDBY");
    CHECK(ultra_rc_feature(a, 0) && ultra_rc_feature(a, 19) && ultra_rc_feature(a, 21) && !ultra_rc_feature(a, 22) &&
          ultra_rc_feature(a, 32) && !ultra_rc_feature(a, 33) && ultra_rc_feature(a, 191) && !ultra_rc_feature(a, 190) &&
          !ultra_rc_feature(a, 192), "rc: IDENTITY part 1, bits 0-191 (ultra_feat_has)");
    a[1] = ULTRA_PAGE_STATUS;
    a[ULTRA_RSP_DATA + ULTRA_ST_STATE] = (uint8_t)((ULTRA_ST_WHY_RETRIES_OUT << ULTRA_ST_WHY_SHIFT) | ULTRA_ST_LIMITING);
    CHECK(ultra_rc_status_why(a) == ULTRA_ST_WHY_RETRIES_OUT, "rc: STATUS reason nibble (0.2.5)");
    a[ULTRA_RSP_DATA + ULTRA_ST_STATE] = ULTRA_ST_TRIPPED | ULTRA_ST_CH_LIMITING;
    CHECK(ultra_rc_status_why(a) == ULTRA_ST_WHY_NONE, "rc: STATUS from a board before 0.2.5: reason 0");
}

/* the encoders' output, decoded on its own (not with ultra_wire_ns): the frame's bits back, and its end where the
 * scheduler's TIME says */
static void decode_check(const char *what, const ultra_timing_t *tm, int64_t tol_us)
{
    static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(8)], back[ULTRA_SENDER_FRAME_BYTES(8)];
    static ultra_pulse_t pulses[ULTRA_SENDER_FRAME_BYTES(8) * 8u];
    ultra_sender_config_t c;
    ultra_values_t v = val(12345);
    uint32_t pos = 0, n, k, len, b;
    uint64_t t_ns = 0;
    int64_t end;
    ultra_sender_config_init(&c, 8, 1);
    c.timing = *tm;
    c.seed = 5;
    ultra_sender_init(&L, &c, B, IT, NI);
    now = 1000000;
    pre();
    for (b = 0; b < 8u; b++) {
        def_mono((uint16_t)b, 0);
        ultra_sender_set_at(&L, (uint16_t)b, 0, &v, 300, 0, now + 30000 + b * 777);
    }
    len = ultra_sender_next_frame(&L, now, frame);
    n = ultra_encode_pulses(tm, frame, len, &pos, pulses, len * 8u);
    memset(back, 0, sizeof back);
    for (k = 0; k < n; k++) {
        const uint32_t thr = ((uint32_t)tm->zero_ticks + tm->one_ticks) / 2u;     /* the board decides on the high time */
        back[k >> 3] |= (uint8_t)((pulses[k].high_ticks > thr ? 1u : 0u) << (7u - (k & 7u)));
        t_ns += pulses[k].high_ticks + (k + 1u < n ? pulses[k].low_ticks : 0u);  /* to the last falling edge */
    }
    end = now + (int64_t)((t_ns * 1000000u + tm->clock_hz/2u) / tm->clock_hz);
    CHECK(n == len * 8u && memcmp(back, frame, len) == 0 && pulses[n - 1].low_ticks >= (uint64_t)tm->reset_ns*tm->clock_hz/1000000000u &&
          ultra_sender_last_time_us(&L) - end >= -tol_us && ultra_sender_last_time_us(&L) - end <= tol_us,
          "%s: pulses decode to the frame, the reset after the last bit, TIME %lld us from the pulses' end", what,
          (long long)(ultra_sender_last_time_us(&L) - end));
}

static void test_independent_timing(void)
{
    ultra_timing_t t;uint8_t p;
    for(p=0;p<4;++p){(void)ultra_timing_profile(&t,p,9600000,500);decode_check("tick pulses",&t,2);}
}

/* undefined values are refused before anything changes */
static void test_refusals(void)
{
    ultra_light_def_t d;
    ultra_light_opt_t o;
    ultra_values_t v = val(1);
    int64_t g0;
    mk(1, 1, 0, 1);
    pre();
    ultra_light_def_init(&d);
    d.roles[2] = 8;
    CHECK(ultra_sender_light_def(&L, 0, 0, &d) == ULTRA_ERR_ARG, "LIGHT_DEF role 8 (reserved): refused");
    d.roles[2] = 0;
    d.flags = 0x04;
    CHECK(ultra_sender_light_def(&L, 0, 0, &d) == ULTRA_ERR_ARG, "LIGHT_DEF flag 0x04 (reserved): refused");
    memset(&o, 0, sizeof o);
    o.flags = 0x80;
    CHECK(ultra_sender_light_opt(&L, 0, 0, &o) == ULTRA_ERR_ARG, "LIGHT_OPT flag 0x80 (reserved): refused");
    CHECK(ultra_sender_switch_to_ultra(&L, now, 4) == ULTRA_ERR_ARG && !ultra_sender_busy(&L),
          "switch_to_ultra(boot 4): refused, nothing queued");
    CHECK(ultra_sender_items_used(&L) == 0u, "refused commands take no item");
    /* a full pool: the group's start stays */
    def_mono(0, 0);
    g0 = L.group->start;
    CHECK(ultra_sender_set(&L, 0, 0, &v, 0, 0, now) == ULTRA_ERR_FULL && L.group->start == g0,
          "a full pool: ULTRA_ERR_FULL, the group's start unchanged");
}

int main(void)
{
    test_preamble();
    test_copies_and_order();
    test_refresh();
    test_refresh_in_stream();
    test_time();
    test_start_offsets();
    test_long_starts();
    test_scene_levels();
    test_group();
    test_config();
    test_clear_unused();
    test_refresh_rotation();
    test_power_and_resync();
    test_power_off();
    test_stream();

    test_dedupe_and_args();
    test_identify_and_retime();
    test_encoders();
    test_cpp_wrapper();
    test_order_wrap();
    test_stream_alias();
    test_rc_answer();
    test_life_sync();
    test_independent_timing();

    test_refusals();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL UNIT TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
