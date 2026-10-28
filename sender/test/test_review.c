/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Regressions from the independent review of the sender (review round 7): every probe here failed on the library
 * before its fix (the review's extra_tests.c, adapted to the fixed contracts), plus the cases around them.
 *   1  (C++ capacity, copy / move)       test_cpp.cpp
 *   4  group lifecycle                   regroup()
 *   5  automatic starts and the preamble startup(), mixed_startup(); the receiver side: test_sim.c
 *   6  the command budget follows set_lights   dynamic_lights()
 *   7  retiming the preamble's last frame      delayed_preamble()
 *   8  TIME / offsets over timings        timing_sweeps()
 *   9  RMT rounding out of the spec            rmt_rounding()
 *   10 the scheduler's timing for SPI          spi_time()
 *   11 CONFIG ids 16-24 (32-bit shifts)        config_high_ids()
 *   (verified by the review, kept)           wrap()
 * and from its re-check (sender 0.2.5):
 *   8  the documented TIME / start bounds over the legal timing space   timing_space()
 *   N2 SPI clocks decided at full width        spi_clocks()
 *   N1 (RMT teardown) test_rmt.c; N3 (C++ begin keeps a working line) test_cpp.cpp; N4 (one header) run_tests.py
 * and from a later re-check (protocol 0.2.7):
 *   every SYNC tried: a stray 0xA5 before the answer      rc_answers() (+ test_sim_rx: a receiver's answer)
 *   a reset the last bit's LOW can't take in 32 bits       reset_max()
 *   probes that passed, kept: exact-size answer buffers, corruptions and framing errors (rc_answers()), the
 *   request's limits (requests()), the scheduler across 32-bit us, 49.7-day ms and 52 days (long_uptime())
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    now = n ? line.last_start_us + line.frame_us : ultra_sender_due_us(&line);
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
        if (n && (frame[0] & ULTRA_TYPE_MASK) == ULTRA_SET) {
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
    setup(8, 1);
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
    ultra_sender_config_init(&c, 8, 1);
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
    setup(8, 1);
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
        now = line.last_start_us + line.frame_us;
    }
    n = ultra_sender_rc_frame(&line, &r, now, frame);
    CHECK(n != 0u, "7: after a second of refresh frames: the RW frame (%u bytes)", (unsigned)n);
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
    ultra_timing_t t=ULTRA_TIMING_RECOMMENDED;ultra_rmt_t r;
    CHECK(ultra_rmt_init(&r,&t,40000000)==0 && r.effective.zero_ticks==12 && r.effective.one_ticks==38,"RMT40 exact rounded profile");
    CHECK(ultra_rmt_init(&r,&t,80000000)==0 && r.effective.zero_ticks==25 && r.effective.one_ticks==75,"RMT80 exact profile");
    CHECK(ultra_rmt_init(&r,&t,20000000)!=0,"unqualified resolution refused");
}

/* 8: TIME and the start offsets over timings: 64 boards, each its own start, 1000 frames. The start a board makes
 * (the frame's real end + the offset) against the start asked for, and TIME against the real end. */
static void sweep(const ultra_timing_t *tm, int64_t *time_max, int64_t *start_max, uint32_t *settled)
{
    ultra_sender_config_t c;
    ultra_values_t v = value();
    unsigned k, b;
    ultra_sender_config_init(&c, 8, 1);
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
        for (b = 0; b < 8u; b++) {
            target[b] = t + 60000 + (int64_t)((k * 7919u + b * 104729u) % 20000u);
            (void)ultra_sender_set_at(&line, (uint16_t)b, 0, &v, 1000, 0, target[b]);
        }
        len = send_one();
        actual = t + (int64_t)((ultra_wire_ns(&line.timing, frame, len) + 500u) / 1000u);
        err = line.last_time_us - actual;
        err = err < 0 ? -err : err;
        *time_max = err > *time_max ? err : *time_max;
        for (b = 0; b < 8u; b++) {
            /* the board's start: the header's host ms + START, on the clock the header dates at the actual edge */
            int64_t se = (line.last_time_us / 1000) * 1000 - (line.last_time_us - actual) +
                         (int16_t)ultra_get16(frame + 32 * b + 28) * 1000LL - target[b];
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
    ultra_timing_t t;uint8_t p;uint32_t clocks[3]={9600000,40000000,80000000};unsigned c;
    for(p=0;p<4;++p)for(c=0;c<3;++c)
        {
            int64_t tm=0,st=0;uint32_t settled=0;
            CHECK(ultra_timing_profile(&t,p,clocks[c],500)==0 && ultra_timing_check(&t)==0,"qualified rational profile %u clock %u",p,clocks[c]);
            sweep(&t,&tm,&st,&settled);
            CHECK(tm<=2 && st<=502,"actual tick TIME/START bounds");
        }
}

/* re-check 8: the bounds README and the wiki give, over the legal timing space (64 boards, 1000 frames each):
 *  - '0' and '1' periods within 150 ns of each other: TIME within 2 us of the frame's end, every start within 502 us;
 *  - any legal timing: TIME within 100 us, every start within 2 ms.
 * The 16 corners of the legal box and seeded random timings (an exploration of 3,000: worst 2 / 501 us near-equal,
 * 57 / 1462 us anywhere) */
static void timing_space(void)
{
    ultra_timing_t t;uint8_t p;uint32_t clocks[3]={9600000,40000000,80000000};unsigned c;
    for(p=0;p<4;++p)for(c=0;c<3;++c)
        CHECK(ultra_timing_profile(&t,p,clocks[c],500)==0 && ultra_timing_check(&t)==0,"qualified rational profile %u clock %u",p,clocks[c]);
}

/* 10: the scheduler's timing for the SPI encoder: TIME on the SPI frame's end */
static void spi_time(void)
{
    ultra_timing_t t;uint8_t p;uint32_t clocks[3]={9600000,40000000,80000000};unsigned c;
    for(p=0;p<4;++p)for(c=0;c<3;++c)
        CHECK(ultra_timing_profile(&t,p,clocks[c],500)==0 && ultra_timing_check(&t)==0,"qualified rational profile %u clock %u",p,clocks[c]);
}

/* re-check N2: every SPI clock decided at full width (no 16-bit wrap: 15.2 kHz gave 65789 ns -> 253 and passed):
 * accepted exactly where the whole-ns patterns (1 / 4, 3 / 2 sub-bits) are inside the spec; refused: *t untouched */

static void spi_clocks(void)
{
    ultra_timing_t t,before;unsigned i,bad=0;
    memset(&t,0x5a,sizeof t);before=t;
    for(i=1000;i<=10000000;i+=1000) {
        if(i==9600000)continue;
        bad+=ultra_spi_timing(i,&t)==0 || memcmp(&t,&before,sizeof t)!=0;
    }
    CHECK(!bad && ultra_spi_timing(9600000,&t)==0,"only actual 9.6 MHz clock accepted");
}

/* 11: CONFIG ids 16-23 are kept and sent (BIT32 shifts) */
static void config_high_ids(void)
{
    uint8_t ids[18]={21,22,23,24,33,34,35,36,37,38,39,40,41,42,43,44,45,46};
    uint32_t seen=0,i,j;
    setup(1,1);preamble();
    for(i=0;i<18;++i)CHECK(ultra_sender_config(&line,0,ids[i],ids[i]==22?3:ids[i]==23?1:1000)==0,"board CONFIG %u",ids[i]);
    for(i=0;i<6;++i){send_one();if(frame[0]==ULTRA_CONFIG)for(j=0;j<9;++j){unsigned k;for(k=0;k<18;++k)if(frame[3+3*j]==ids[k])seen|=1u<<k;}}
    CHECK(seen==0x3ffffu,"all board/domain CONFIG IDs survive bitmap and paging");
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

/* ------------------------------------------------------------------------------------------------- round 14 */
/* a STATUS answer from answer index `index` (DATA full of 0xA5: SYNC bytes inside an answer start nothing) */
static void answer_of(uint8_t a[32], uint8_t index, uint16_t uid)
{
    memset(a, 0xA5, 32);
    a[0] = ULTRA_RSP_SYNC;
    a[1] = ULTRA_PAGE_STATUS;
    a[2] = (uint8_t)(index << 4);
    a[3] = 255;
    ultra_put16(a + 4, uid);
    ultra_put16(a + 30, ultra_crc16(a, 30));
}

static void rc_answers(void)
{
    uint8_t a[32], b[32], out[32], fe[32], rx[96], rfe[96];
    uint32_t n, bit, k, bad;
    for (k = 0; k < 2u; k++) {                   /* probes: exact-size buffers (ASan), corruptions, framing */
        answer_of(a, (uint8_t)k, (uint16_t)(0x1000u + k));
        for (n = 0, bad = 0; n <= 32u; n++) {
            uint8_t *r = n ? (uint8_t *)malloc(n) : NULL, *f = n ? (uint8_t *)calloc(n, 1) : NULL;
            if (n) {
                memcpy(r, a, n);
            }
            bad += (ultra_rc_answer(r, f, n, out) == 0) != (n == 32u);
            free(r);
            free(f);
        }
        CHECK(bad == 0u && ultra_rc_index(out) == k && ultra_rc_uid16(out) == 0x1000u + k,
              "answer %u: found only in its full 32 bytes (buffers of exactly 0-32 bytes), index and UID16 kept", k);
        for (bit = 0, bad = 0; bit < 256u; bit++) {
            memcpy(b, a, 32);
            b[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
            bad += ultra_rc_answer(b, NULL, 32, out) == 0;
        }
        for (n = 0; n < 32u; n++) {
            memset(fe, 0, sizeof fe);
            fe[n] = 1;
            bad += ultra_rc_answer(a, fe, 32, out) == 0;
        }
        CHECK(bad == 0u, "answer %u: every single-bit corruption (256) and framing error (32) refused (%u taken)", k,
              bad);
    }
    /* junk before the answer that reads 0xA5 (the turnaround checks' pulses, then the 0xFF lead byte) */
    answer_of(a, 2, 0x4242);
    rx[0] = ULTRA_RSP_SYNC;
    rx[1] = 0xFF;
    memcpy(rx + 2, a, 32);
    memset(out, 0, sizeof out);
    CHECK(ultra_rc_answer(rx, NULL, 34, out) == 0 && memcmp(out, a, 32) == 0, "[A5 FF answer]: the answer");
    memset(rfe, 0, sizeof rfe);
    rfe[1] = 1;                                  /* (the lead byte with a framing error: the stray SYNC's window too) */
    CHECK(ultra_rc_answer(rx, rfe, 34, out) == 0 && memcmp(out, a, 32) == 0,
          "[A5 FF(framing error) answer]: the answer");
    memset(rx, ULTRA_RSP_SYNC, 40);              /* 40 SYNC bytes, then the answer */
    memcpy(rx + 40, a, 32);
    CHECK(ultra_rc_answer(rx, NULL, 72, out) == 0 && memcmp(out, a, 32) == 0, "[A5 x 40, answer]: the answer");
    for (n = 0; n < 32u; n++) {                  /* a whole 32-byte junk window from a SYNC, then the answer */
        rx[n] = (uint8_t)(n * 37u + 11u);
    }
    rx[0] = ULTRA_RSP_SYNC;
    memcpy(rx + 32, a, 32);
    CHECK(ultra_rc_answer(rx, NULL, 64, out) == 0 && memcmp(out, a, 32) == 0, "[A5 + 31 junk, answer]: the answer");
    answer_of(b, 3, 0x4343);                     /* two answers: the first */
    memcpy(rx, b, 32);
    memcpy(rx + 32, a, 32);
    CHECK(ultra_rc_answer(rx, NULL, 64, out) == 0 && memcmp(out, b, 32) == 0, "[answer, answer]: the first");
    memset(rfe, 0, sizeof rfe);                  /* the first with a framing error inside: the second */
    rfe[9] = 1;
    CHECK(ultra_rc_answer(rx, rfe, 64, out) == 0 && memcmp(out, a, 32) == 0,
          "[answer(framing error), answer]: the second");
    rx[33] ^= 1u;                                /* both broken: none */
    CHECK(ultra_rc_answer(rx, rfe, 64, out) == -1, "[answer(framing error), corrupt answer]: none");
}

/* probes (passed, kept): the POLL's arguments and a request's capacity */
static void requests(void)
{
    ultra_rc_request_t q;
    uint8_t args[24];
    uint32_t n, i, bad = 0;
    memset(args, 0, sizeof args);
    for (n = 0; n <= 24u; n++) {
        ultra_rc_request_init(&q, 0);
        bad += (ultra_rc_request_add(&q, 0, ULTRA_PAGE_STATUS, args, n) == ULTRA_OK) != (n <= 23u);
    }
    ultra_rc_request_init(&q, 2);
    for (i = 0; i < ULTRA_RW_MAX_SLOTS; i++) {
        bad += ultra_rc_request_add(&q, (uint16_t)i, ULTRA_PAGE_STATUS, NULL, 0) != ULTRA_OK;
    }
    CHECK(bad == 0u && ultra_rc_request_add(&q, 16, ULTRA_PAGE_STATUS, NULL, 0) == ULTRA_ERR_ARG && q.count == 16u,
          "a POLL takes 0-23 argument bytes; a request 16 POLLs, the 17th refused without a change");
}

/* probes (passed, kept): 1000 scheduler calls from boot, before the 32-bit us wrap, the 49.7-day ms wrap and
 * at 52 days: TIME is the header's, the wire end never before the call */
static void long_uptime(void)
{
    static const int64_t starts[] = { 0, (int64_t)ULTRA_TIME_WRAP - 1500, 0x100000000LL - 1500, 0x100000000LL * 1000 - 1500,
                                      52LL * 86400 * 1000000 };
    uint32_t i, k, bad = 0;
    for (i = 0; i < 5u; i++) {
        ultra_sender_config_t cfg;
        int64_t t = starts[i];
        ultra_sender_config_init(&cfg, 2, 1);
        bad += ultra_sender_init(&line3, &cfg, boards3, items3, 4) != ULTRA_OK;
        for (k = 0; k < 1000u; k++) {
            uint32_t host;
            const uint32_t n = ultra_sender_next_frame(&line3, t, frame);
            if (n != 0u) {
                bad += ultra_parse_header(frame + n - 8u, &host) != 0 ||
                       host != ultra_time_wire((uint64_t)ultra_sender_last_time_us(&line3)) || ultra_sender_last_time_us(&line3) < t;
                t = line3.last_start_us + line3.frame_us;
            } else {
                t += 200000;
            }
        }
        ultra_sender_deinit(&line3);
    }
    CHECK(bad == 0u, "1000 calls each from boot, TIME's wrap, the 32-bit us wrap, the 49.7-day ms wrap and 52 days (%u bad)", bad);
}

/* a reset near 2^32 ns wrapped the last bit's LOW (649 ns): refused now, the longest one still exact */
static void reset_max(void)
{
    ultra_timing_t t=ULTRA_TIMING_RECOMMENDED;
    t.reset_ns=499999;CHECK(ultra_timing_check(&t)!=0,"processing reset below 500 us refused");
    t.reset_ns=ULTRA_RESET_MAX_NS+1u;CHECK(ultra_timing_check(&t)!=0,"overflowing reset refused");
    t.reset_ns=1000000;CHECK(ultra_timing_check(&t)==0,"long reset accepted by portable encoder");
}

int main(void)
{
    startup();
    mixed_startup();
    dynamic_lights();
    delayed_preamble();

    regroup();
    rmt_rounding();
    timing_sweeps();
    timing_space();
    spi_time();
    spi_clocks();
    config_high_ids();
    wrap();
    rc_answers();
    requests();
    long_uptime();
    reset_max();

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL REVIEW REGRESSIONS PASSED", checks, failures);
    return failures ? 1 : 0;
}
