/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Regressions from the independent review of the sender (review round 7): every probe here failed on the library
 * before its fix (the review's extra_tests.c, adapted to the fixed contracts), plus the cases around them.
 *   1  (C++ capacity, copy / move)       test_cpp.cpp
 *   3  NULL parameters                   null_params()
 *   4  group lifecycle                   regroup()
 *   5  automatic starts and the preamble startup(), mixed_startup(); the receiver side: test_sim.c
 *   6  the command budget follows set_lights   dynamic_lights()
 *   7  retiming the preamble's last frame      delayed_preamble()
 *   8  TIME / offsets over timings        timing_sweeps()
 *   9  RMT rounding out of the spec            rmt_rounding()
 *   10 the scheduler's timing for SPI          spi_time()
 *   11 CONFIG ids 16-24 (32-bit shifts)        config_high_ids()
 *   13 partial effect parameters             partial_params()
 *   (verified by the review, kept)           wrap()
 * and from its re-check (sender 0.2.5):
 *   8  the documented TIME / start bounds over the legal timing space   timing_space()
 *   N2 SPI clocks decided at full width        spi_clocks()
 *   N1 (RMT teardown) test_rmt.c; N3 (C++ begin keeps a working line) test_cpp.cpp; N4 (one header) run_tests.py
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ultra_sender.h"
/* an item's start (kept in two 32-bit halves) */
#define ITEM_START(it) ((int64_t)(((uint64_t)(it).start_hi << 32) | (it).start_lo))
#include "ultra_sender_rc.h"

static ultra_sender_t line, line2, line3;
static ultra_sender_board_t boards[64], boards2[64], boards3[64];
static ultra_sender_item_t items[640], items2[64], items3[64];
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(64)];
static int64_t now;
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

static void setup(uint16_t n, uint8_t lights)
{
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, n, lights);
    c.clear_unused = 0;
    (void)ultra_sender_init(&line, &c, boards, items, 640);
    now = 1000000;
}

static uint32_t send_one(void)
{
    const uint32_t n = ultra_sender_next_frame(&line, now, frame);
    now = n ? line.last_time_us + 300 : ultra_sender_due_us(&line);
    return n;
}

static void preamble(void)
{
    while (line.preamble_left) {
        (void)send_one();
    }
}

static ultra_values_t value(void)
{
    ultra_values_t v;
    memset(&v, 0xFF, sizeof v);
    return v;
}

/* the first frame (from now on) carrying a SET for board 0: its start offset, ms */
static int first_set_offset(ultra_sender_t *l, int64_t *t, int *found)
{
    int i;
    *found = 0;
    for (i = 0; i < 1000; i++) {
        const uint32_t n = ultra_sender_next_frame(l, *t, frame);
        *t = n ? l->last_time_us + 300 : ultra_sender_due_us(l);
        if (n && frame[0] == ULTRA_SET) {
            *found = 1;
            return (int16_t)ultra_get16(frame + 28);
        }
    }
    return 0;
}

/* 5: a SET queued at start-up (the preamble still to send) starts after its first copy arrives */
static void startup(void)
{
    ultra_values_t v = value();
    int off, found;
    setup(50, 1);
    (void)ultra_sender_set(&line, 0, 0, &v, 1000, 0, now);
    off = first_set_offset(&line, &now, &found);
    CHECK(found && off >= 0, "5: a SET queued during the 50-board preamble: its first copy %d ms before the start", off);
    /* a resync: the same */
    (void)ultra_sender_set(&line, 0, 0, &v, 1000, 0, now);
    while (ultra_sender_busy(&line)) {
        (void)send_one();
    }
    ultra_sender_resync(&line, now);
    (void)ultra_sender_set(&line, 0, 0, &v, 2000, 0, now);
    off = first_set_offset(&line, &now, &found);
    CHECK(found && off >= 0, "5: a SET queued during a resync's preamble: first copy %d ms before the start", off);
}

/* 5: two lines of a group, 50 and 2 boards, both starting up: one start, after both preambles */
static void mixed_startup(void)
{
    ultra_sender_config_t c;
    ultra_sender_group_t g;
    ultra_values_t v = value();
    int64_t ta = 1000000, tb = 1000000;
    int oa, ob, fa, fb;
    ultra_sender_config_init(&c, 50, 1);
    c.clear_unused = 0;
    (void)ultra_sender_init(&line, &c, boards, items, 640);
    ultra_sender_config_init(&c, 2, 1);
    c.clear_unused = 0;
    (void)ultra_sender_init(&line2, &c, boards2, items2, 64);
    ultra_sender_group_init(&g);
    ultra_sender_join_group(&line, &g);
    ultra_sender_join_group(&line2, &g);
    (void)ultra_sender_set(&line2, 0, 0, &v, 1000, 0, ta);
    (void)ultra_sender_set(&line, 0, 0, &v, 1000, 0, ta + 2000);
    CHECK(ITEM_START(line.items[0]) == ITEM_START(line2.items[0]),
          "5: two lines starting up share one start (%lld, %lld)", (long long)ITEM_START(line.items[0]),
          (long long)ITEM_START(line2.items[0]));
    oa = first_set_offset(&line, &ta, &fa);
    ob = first_set_offset(&line2, &tb, &fb);
    CHECK(fa && fb && oa >= 0 && ob >= 0, "5: both lines' first copies arrive before it (%d, %d ms ahead)", oa, ob);
    ultra_sender_deinit(&line);
    ultra_sender_deinit(&line2);
}

/* 6: raising a board's lights raises the automatic command budget: one change to its 5 lights, one start */
static void dynamic_lights(void)
{
    ultra_values_t v = value();
    int64_t starts[5];
    unsigned i;
    setup(50, 1);
    preamble();
    (void)ultra_sender_set_lights(&line, 0, 5);
    for (i = 0; i < 5u; i++) {
        (void)ultra_sender_set(&line, 0, (uint8_t)i, &v, 1000, 0, now);
        starts[i] = ITEM_START(items[i]);
    }
    CHECK(starts[0] == starts[4], "6: set_lights 1 -> 5, 5 SETs at once: one start (%lld .. %lld)",
          (long long)starts[0], (long long)starts[4]);
    /* and back: an explicit budget stays */
    (void)ultra_sender_set_group_commands(&line, 9);
    (void)ultra_sender_set_lights(&line, 0, 1);
    CHECK(line.group_commands == 9u, "6: an explicit budget stays when the lights change (%u)", line.group_commands);
    (void)ultra_sender_set_group_commands(&line, 0);
    CHECK(line.group_commands == 1u, "6: back to automatic: the most lights on a board (%u)", line.group_commands);
}

/* 7: the preamble's last frame retimed 2 s later: the return channel waits from then on, and for headers */
static void delayed_preamble(void)
{
    ultra_rc_request_t r;
    int64_t actual;
    uint32_t n, i;
    setup(1, 1);
    while (line.preamble_left > 1u) {
        (void)send_one();
    }
    (void)ultra_sender_next_frame(&line, now, frame);   /* the last preamble frame: couldn't go out */
    now += 2000000;
    (void)ultra_sender_retime(&line, now, frame);
    actual = line.last_time_us;
    ultra_rc_request_init(&r, 0);
    (void)ultra_rc_request_add(&r, 0, ULTRA_PAGE_STATUS, NULL, 0);
    n = ultra_sender_rc_frame(&line, &r, actual + 300, frame);
    CHECK(n == 0u, "7: no RW frame 300 us after the retimed end of the preamble (%u bytes)", (unsigned)n);
    n = ultra_sender_rc_frame(&line, &r, actual + 1500000, frame);
    CHECK(n == 0u, "7: nor 1.5 s later without headers in between (%u bytes)", (unsigned)n);
    now = actual + 300;
    for (i = 0; i < 6u; i++) {
        now += 200000;
        (void)ultra_sender_next_frame(&line, now, frame);
        now = line.last_time_us + 300;
    }
    n = ultra_sender_rc_frame(&line, &r, now, frame);
    CHECK(n != 0u, "7: after a second of refresh frames: the RW frame (%u bytes)", (unsigned)n);
}

/* 13 + 3: effect parameters: exactly the effect's bytes, never NULL with a length */
static void partial_params(void)
{
    const uint8_t p[22] = { 1 };
    setup(1, 1);
    preamble();
    CHECK(ultra_sender_effect(&line, 0, 0, ULTRA_FX_BREATHE, p, 1, now) == ULTRA_ERR_ARG,
          "13: a breathe with 1 of its 7 parameter bytes is refused");
    CHECK(ultra_sender_effect(&line, 0, 0, ULTRA_FX_BREATHE, p, 8, now) == ULTRA_ERR_ARG,
          "13: ... with 8 too");
    CHECK(ultra_sender_scene_effect(&line, 0, 0, ULTRA_FX_CANDLE, p, 3) == ULTRA_ERR_ARG,
          "13: a power-on candle with 3 of its 4 bytes is refused");
    CHECK(ultra_sender_effect(&line, 0, 0, ULTRA_FX_NONE, NULL, 0, now) == ULTRA_OK, "13: NONE takes none");
    CHECK(ultra_sender_items_used(&line) == 1u, "13: the refused ones took no item (%u)", ultra_sender_items_used(&line));
}

static void null_params(void)
{
    int64_t g0;
    setup(1, 1);
    preamble();
    g0 = line.group->start;
    CHECK(ultra_sender_effect(&line, 0, 0, ULTRA_FX_BREATHE, NULL, 7, now) == ULTRA_ERR_ARG,
          "3: effect(NULL, 7) is refused, not a crash");
    CHECK(ultra_sender_effect(&line, 0, 0, ULTRA_FX_PULSE, NULL, 13, now) == ULTRA_ERR_ARG, "3: pulse(NULL, 13) too");
    CHECK(ultra_sender_effect_at(&line, 0, 0, ULTRA_FX_COLOR_LOOP, NULL, 5, now) == ULTRA_ERR_ARG,
          "3: effect_at(NULL, 5) too");
    CHECK(ultra_sender_scene_effect(&line, 0, 0, ULTRA_FX_BREATHE, NULL, 7) == ULTRA_ERR_ARG, "3: scene_effect too");
    CHECK(ultra_fx_period_ms(ULTRA_FX_BREATHE, NULL, 7, NULL, 0) == 4000u, "3: the period helper takes NULL as none");
    CHECK(line.group->start == g0, "3: a refused command leaves the group's start alone");
}

/* 4: a line in a group, initialized again and joined again: no cycle; deinit / leave; teardown of several lines */
static uint32_t members(const ultra_sender_group_t *g)
{
    const ultra_sender_t *x;
    uint32_t k = 0;
    for (x = g->lines; x != 0 && k < 100u; x = x->group_next) {
        k++;
    }
    return k;
}

static void regroup(void)
{
    ultra_sender_group_t g;
    ultra_sender_config_t c;
    ultra_values_t v = value();
    setup(1, 1);
    ultra_sender_group_init(&g);
    ultra_sender_join_group(&line, &g);
    ultra_sender_config_init(&c, 1, 1);
    (void)ultra_sender_init(&line, &c, boards, items, 640);   /* (without leaving: the documented misuse) */
    ultra_sender_join_group(&line, &g);
    CHECK(line.group_next != &line && members(&g) == 1u, "4: initialized again and rejoined: no self-link (%u member)",
          members(&g));
    CHECK(ultra_sender_set(&line, 0, 0, &v, 0, 0, now) == ULTRA_OK, "4: a start time after that returns");
    /* three lines; one leaves, one is deinitialized, joined again */
    ultra_sender_config_init(&c, 2, 1);
    (void)ultra_sender_init(&line2, &c, boards2, items2, 64);
    (void)ultra_sender_init(&line3, &c, boards3, items3, 64);
    ultra_sender_join_group(&line2, &g);
    ultra_sender_join_group(&line3, &g);
    CHECK(members(&g) == 3u, "4: three lines in the group");
    ultra_sender_leave_group(&line2);
    CHECK(members(&g) == 2u && line2.group == &line2.own_group, "4: one leaves: two left");
    ultra_sender_deinit(&line3);
    CHECK(members(&g) == 1u && ultra_sender_next_frame(&line3, now, frame) == 0u &&
          ultra_sender_set(&line3, 0, 0, &v, 0, 0, now) == ULTRA_ERR_ARG,
          "4: one deinitialized: out of the group, no frames, no commands");
    (void)ultra_sender_init(&line3, &c, boards3, items3, 64);
    ultra_sender_join_group(&line3, &g);
    ultra_sender_join_group(&line3, &g);
    ultra_sender_join_group(&line2, &g);
    CHECK(members(&g) == 3u, "4: joined again (twice): three, no duplicates (%u)", members(&g));
    ultra_sender_deinit(&line);
    ultra_sender_deinit(&line2);
    ultra_sender_deinit(&line3);
    CHECK(members(&g) == 0u, "4: all deinitialized: the group is empty");
}

/* 9: an RMT resolution that rounds a valid timing out of the spec is refused; the effective timing is reported */
static void rmt_rounding(void)
{
    ultra_timing_t t = ULTRA_TIMING_RECOMMENDED;
    const ultra_timing_t rec = ULTRA_TIMING_RECOMMENDED;
    ultra_rmt_t r;
    int rc;
    t.t0h_ns = ULTRA_T0H_MIN_NS;
    rc = ultra_rmt_init(&r, &t, 10000000u);
    CHECK(rc != 0, "9: 220 ns at 10 MHz (200 ns) is refused (rc %d)", rc);
    rc = ultra_rmt_init(&r, &rec, 40000000u);
    CHECK(rc == 0 && memcmp(&r.effective, &rec, sizeof rec) == 0, "9: the recommended timing at 40 MHz is exact");
    rc = ultra_rmt_init(&r, &rec, 8000000u);       /* 125 ns ticks: 250 / 1000 / 750 / 625 */
    CHECK(rc == 0 && r.effective.t0l_ns == 1000u && r.effective.t1h_ns == 750u && r.effective.t1l_ns == 625u,
          "9: at 8 MHz: the effective timing is what the ticks send (%u / %u / %u / %u)", r.effective.t0h_ns,
          r.effective.t0l_ns, r.effective.t1h_ns, r.effective.t1l_ns);
}

/* 8: TIME and the start offsets over timings: 64 boards, each its own start, 1000 frames. The start a board makes
 * (the frame's real end + the offset) against the start asked for, and TIME against the real end. */
static void sweep(const ultra_timing_t *tm, int64_t *time_max, int64_t *start_max, uint32_t *settled)
{
    ultra_sender_config_t c;
    ultra_values_t v = value();
    unsigned k, b;
    ultra_sender_config_init(&c, 64, 1);
    c.clear_unused = 0;
    c.repeats = 1;
    c.timing = *tm;
    (void)ultra_sender_init(&line, &c, boards, items, 640);
    now = 1000000;
    preamble();
    *time_max = *start_max = 0;
    *settled = 0;
    for (k = 0; k < 1000u; k++) {
        int64_t t = now, actual, err, worst = 0;
        int64_t target[64];
        uint32_t len;
        v.m = (uint16_t)(k * 73u);
        for (b = 0; b < 64u; b++) {
            target[b] = t + 60000 + (int64_t)((k * 7919u + b * 104729u) % 20000u);
            (void)ultra_sender_set_at(&line, (uint16_t)b, 0, &v, 1000, 0, target[b]);
        }
        len = send_one();
        actual = t + (int64_t)((ultra_wire_ns(&line.timing, frame, len) + 500u) / 1000u);
        err = line.last_time_us - actual;
        err = err < 0 ? -err : err;
        *time_max = err > *time_max ? err : *time_max;
        for (b = 0; b < 64u; b++) {
            int64_t se = actual + (int16_t)ultra_get16(frame + 32 * b + 28) * 1000LL - target[b];
            se = se < 0 ? -se : se;
            worst = se > worst ? se : worst;
        }
        *start_max = worst > *start_max ? worst : *start_max;
        *settled += err == 0 && worst <= 500;
        now = t + 250000;
    }
}

static void timing_sweeps(void)
{
    const ultra_timing_t rec = ULTRA_TIMING_RECOMMENDED;
    const ultra_timing_t rmt8 = { 250, 1000, 750, 625, 300000 };          /* RMT at 8 MHz */
    const ultra_timing_t extreme = { 220, 450, 2000, 10000, 300000 };     /* legal: '1' 12 us, '0' 0.67 us */
    int64_t tmax, smax;
    uint32_t ok;
    sweep(&rec, &tmax, &smax, &ok);
    /* (a frame where one offset's rounding flip moves its end by ~1 us across that offset's boundary has no exact
     * answer: 1 us is the bound there) */
    CHECK(tmax <= 1 && smax <= 501, "8: recommended timing, 64 boards, 1000 frames: TIME within %lld us of the end, "
          "every start within %lld us (nearest ms); %u frames exact", (long long)tmax, (long long)smax, ok);
    sweep(&rmt8, &tmax, &smax, &ok);
    CHECK(tmax <= 1 && smax <= 502, "8: RMT at 8 MHz ('0' / '1' 125 ns apart): TIME within %lld us, starts within %lld us; "
          "%u frames exact", (long long)tmax, (long long)smax, ok);
    sweep(&extreme, &tmax, &smax, &ok);
    CHECK(tmax <= 50 && smax <= 1500, "8: the legal extreme ('1' 12 us, '0' 0.67 us): TIME within %lld us, starts "
          "within %lld us (the review's probe: 3141); %u of 1000 frames exact", (long long)tmax, (long long)smax, ok);
}

/* re-check 8: the bounds README and the wiki give, over the legal timing space (64 boards, 1000 frames each):
 *  - '0' and '1' periods within 150 ns of each other: TIME within 2 us of the frame's end, every start within 502 us;
 *  - any legal timing: TIME within 100 us, every start within 2 ms.
 * The 16 corners of the legal box and seeded random timings (an exploration of 3,000: worst 2 / 501 us near-equal,
 * 57 / 1462 us anywhere) */
static uint32_t ts_rng = 20261015u;
static uint32_t ts_rnd(uint32_t lo, uint32_t hi)
{
    ts_rng = ts_rng * 1664525u + 1013904223u;
    return lo + (ts_rng >> 8) % (hi - lo + 1u);
}

static void timing_space(void)
{
    static const uint16_t lim[4][2] = { { ULTRA_T0H_MIN_NS, ULTRA_T0H_MAX_NS }, { ULTRA_TL_MIN_NS, ULTRA_TL_MAX_NS },
                                        { ULTRA_T1H_MIN_NS, ULTRA_T1H_MAX_NS }, { ULTRA_TL_MIN_NS, ULTRA_TL_MAX_NS } };
    int64_t near_t = 0, near_s = 0, any_t = 0, any_s = 0, tmax, smax;
    uint32_t i, ok, n_near = 0, n_any = 0;
    for (i = 0; i < 16u + 60u + 100u; i++) {
        ultra_timing_t t;
        long d;
        t.reset_ns = 300000u;
        if (i < 16u) {                           /* the corners */
            t.t0h_ns = lim[0][i & 1u];
            t.t0l_ns = lim[1][(i >> 1) & 1u];
            t.t1h_ns = lim[2][(i >> 2) & 1u];
            t.t1l_ns = lim[3][(i >> 3) & 1u];
        } else {
            do {                                 /* (drawn again until it is a legal timing) */
                t.t0h_ns = (uint16_t)ts_rnd(ULTRA_T0H_MIN_NS, ULTRA_T0H_MAX_NS);
                t.t1h_ns = (uint16_t)ts_rnd(ULTRA_T1H_MIN_NS, ULTRA_T1H_MAX_NS);
                t.t0l_ns = (uint16_t)ts_rnd(ULTRA_TL_MIN_NS, ULTRA_TL_MAX_NS);
                t.t1l_ns = (uint16_t)ts_rnd(ULTRA_TL_MIN_NS, ULTRA_TL_MAX_NS);
                if (i < 16u + 60u) {             /* '1' period within 150 ns of the '0' period */
                    const long t1l = (long)t.t0h_ns + t.t0l_ns - t.t1h_ns + (long)ts_rnd(0, 300) - 150;
                    t.t1l_ns = (uint16_t)(t1l < 0 ? 0 : t1l > 65535 ? 65535 : t1l);
                }
            } while (ultra_timing_check(&t) != 0);
        }
        sweep(&t, &tmax, &smax, &ok);
        d = (long)(t.t1h_ns + t.t1l_ns) - (long)(t.t0h_ns + t.t0l_ns);
        if (d >= -150 && d <= 150) {
            n_near++;
            near_t = tmax > near_t ? tmax : near_t;
            near_s = smax > near_s ? smax : near_s;
        }
        n_any++;
        any_t = tmax > any_t ? tmax : any_t;
        any_s = smax > any_s ? smax : any_s;
    }
    CHECK(n_near >= 60u && near_t <= 2 && near_s <= 502, "8: '0' / '1' periods within 150 ns (%u timings): TIME within "
          "%lld us (<= 2), starts within %lld us (<= 502)", n_near, (long long)near_t, (long long)near_s);
    CHECK(any_t <= 100 && any_s <= 2000, "8: any legal timing (%u: the 16 corners, random ones): TIME within %lld us "
          "(<= 100), starts within %lld us (<= 2000)", n_any, (long long)any_t, (long long)any_s);
}

/* 10: the scheduler's timing for the SPI encoder: TIME on the SPI frame's end */
static void spi_time(void)
{
    static const uint32_t hz[3] = { 2700000u, 4000000u, 4400000u };
    ultra_timing_t t;
    uint32_t i;
    CHECK(ultra_spi_timing(4000000u, &t) == 0 && t.t0h_ns == 250u && t.t0l_ns == 1000u && t.t1h_ns == 750u &&
          t.t1l_ns == 500u, "10: SPI at 4 MHz: 250 / 1000 / 750 / 500 ns");
    CHECK(ultra_spi_timing(2600000u, &t) != 0 && ultra_spi_timing(4500000u, &t) != 0,
          "10: SPI clocks outside 2.7-4.4 MHz are refused");
    {
        volatile uint32_t fast = 4400000u, nominal = 4000000u;
        CHECK(ULTRA_SPI_RESET_BYTES(fast) * 8u * 1000000ull / fast >= 300u && ULTRA_SPI_RESET_BYTES(nominal) == 150u,
              "10: the reset bytes give >= 300 us at every clock");
    }
    for (i = 0; i < 3u; i++) {
        ultra_sender_config_t c;
        int64_t start, end;
        uint32_t n, bit, sub, last_fall = 0;
        static uint8_t spi[ULTRA_SPI_BYTES(ULTRA_SENDER_FRAME_BYTES(50))];
        ultra_sender_config_init(&c, 50, 1);
        (void)ultra_spi_timing(hz[i], &c.timing);
        (void)ultra_sender_init(&line, &c, boards, items, 640);
        start = 1000000;
        n = ultra_sender_next_frame(&line, start, frame);
        sub = (uint32_t)ultra_spi_encode(frame, n, spi);
        /* the SPI stream itself: its last falling edge, in SPI bit times */
        for (bit = 0; bit < sub * 8u; bit++) {
            if (bit > 0u && ((spi[(bit - 1u) >> 3] >> (7u - ((bit - 1u) & 7u))) & 1u) &&
                !((spi[bit >> 3] >> (7u - (bit & 7u))) & 1u)) {
                last_fall = bit;
            }
        }
        end = start + (int64_t)(((uint64_t)last_fall * 1000000000u / hz[i] + 500u) / 1000u);
        /* (the timing is in whole ns: 370.4 ns at 2.7 MHz is 370, 0.1 % short) */
        CHECK(line.last_time_us - end >= -1 - (end - start) / 1000 && line.last_time_us - end <= 1 + (end - start) / 1000,
              "10: SPI at %.1f MHz, 50 boards: TIME %lld us from the SPI stream's end (%lld us long)", hz[i] / 1e6,
              (long long)(line.last_time_us - end), (long long)(end - start));
    }
}

/* re-check N2: every SPI clock decided at full width (no 16-bit wrap: 15.2 kHz gave 65789 ns -> 253 and passed):
 * accepted exactly where the whole-ns patterns (1 / 4, 3 / 2 sub-bits) are inside the spec; refused: *t untouched */
static double spi_ns(uint32_t subbits, uint32_t hz)
{
    return floor((double)subbits * 1e9 / (double)hz + 0.5);
}

static int spi_in_spec(uint32_t hz)
{
    const double t0h = spi_ns(1, hz), t0l = spi_ns(4, hz), t1h = spi_ns(3, hz), t1l = spi_ns(2, hz);
    return t0h >= ULTRA_T0H_MIN_NS && t0h <= ULTRA_T0H_MAX_NS && t1h >= ULTRA_T1H_MIN_NS && t1h <= ULTRA_T1H_MAX_NS &&
           t0l >= ULTRA_TL_MIN_NS && t0l <= ULTRA_TL_MAX_NS && t1l >= ULTRA_TL_MIN_NS && t1l <= ULTRA_TL_MAX_NS;
}

static void spi_clocks(void)
{
    static const uint32_t odd[] = { 1u, 1000u, 15200u, 15259u, 30518u, 61036u, 100000u, 1000000u, 2699999u,
                                    4400001u, 20000000u, 1000000000u, 4294967295u };
    const uint32_t n_odd = (uint32_t)(sizeof odd / sizeof odd[0]);
    uint32_t i, wrong = 0, accepted = 0, lo = 0, hi = 0;
    for (i = 0; i < n_odd + 10000u; i++) {
        const uint32_t hz = i < n_odd ? odd[i] : 1000u * (i - n_odd + 1u);    /* and 1 kHz .. 10 MHz */
        ultra_timing_t t, before;
        int got, want = spi_in_spec(hz);
        memset(&t, 0x5A, sizeof t);
        before = t;
        got = ultra_spi_timing(hz, &t) == 0;
        if (got != want || (!got && memcmp(&t, &before, sizeof t) != 0) ||
            (got && (t.t0h_ns != spi_ns(1, hz) || t.t0l_ns != spi_ns(4, hz) || t.t1h_ns != spi_ns(3, hz) ||
                     t.t1l_ns != spi_ns(2, hz) || t.reset_ns != 300000u))) {
            if (wrong++ < 5u) {
                printf("     %lu Hz: %s, want %s\n", (unsigned long)hz, got ? "accepted" : "refused",
                       want ? "accepted" : "refused");
            }
        }
        if (got) {
            lo = accepted++ == 0u ? hz : lo;
            hi = hz;
        }
    }
    CHECK(wrong == 0u && accepted > 0u, "N2: SPI clocks 1 Hz-4.29 GHz: accepted exactly inside the spec (%lu-%lu Hz of "
          "the 1 kHz steps), refused ones leave the timing untouched", (unsigned long)lo, (unsigned long)hi);
    {
        ultra_timing_t t;
        CHECK(ultra_spi_timing(15200u, &t) != 0 && ultra_spi_timing(1u, &t) != 0, "N2: 15.2 kHz and 1 Hz refused");
    }
}

/* 11: CONFIG ids 16-23 are kept and sent (BIT32 shifts) */
static void config_high_ids(void)
{
    uint8_t id;
    uint32_t i, seen = 0;
    setup(1, 1);
    preamble();
    for (id = 16; id < ULTRA_CFG_COUNT; id++) {
        (void)ultra_sender_config(&line, 0, id, (uint16_t)(id == 22u ? 3u : id == 23u ? 1u : 1000u + id));
    }
    for (i = 0; i < 4u; i++) {
        (void)send_one();
        if (frame[0] == ULTRA_CONFIG) {
            uint32_t k;
            for (k = 0; k < 9u; k++) {
                seen |= frame[3 + 3 * k] >= 16u ? 1u << (frame[3 + 3 * k] - 16u) : 0u;
            }
        }
    }
    CHECK(seen == (1u << (ULTRA_CFG_COUNT - 16u)) - 1u, "11: CONFIG 16-%u all in the CONFIG slots (mask %03X)",
          (unsigned)ULTRA_CFG_COUNT - 1u, (unsigned)seen);
}

/* the review verified these; kept as regressions */
static void wrap(void)
{
    ultra_clock64_t c = { 0, 0 };
    ultra_values_t v = value();
    unsigned i;
    int64_t target;
    CHECK(ultra_clock64(&c, 0xFFFFFFF0u) == 0xFFFFFFF0LL && ultra_clock64(&c, 0x10u) == 0x100000010LL,
          "clock64 extends an observed wrap");
    setup(1, 1);
    preamble();
    now = 0x100000000LL - 1000;
    target = now + 5000;
    (void)ultra_sender_set_at(&line, 0, 0, &v, 1000, 0, target);
    for (i = 0; i < 3u; i++) {
        int64_t error;
        (void)send_one();
        error = line.last_time_us + (int16_t)ultra_get16(frame + 28) * 1000LL - target;
        CHECK(error >= -500 && error <= 500, "SET copy %u across TIME's wrap: start within %lld us", i,
              (long long)error);
    }
}

int main(void)
{
    startup();
    mixed_startup();
    dynamic_lights();
    delayed_preamble();
    partial_params();
    null_params();
    regroup();
    rmt_rounding();
    timing_sweeps();
    timing_space();
    spi_time();
    spi_clocks();
    config_high_ids();
    wrap();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL REVIEW REGRESSIONS PASSED", checks, failures);
    return failures ? 1 : 0;
}
