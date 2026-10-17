/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Round 9 (flash / RAM, no behaviour change): every arithmetic the library changed, against its old code kept here
 * verbatim (old_*), on random and edge inputs:
 *  - ultra_udiv64_ (no libgcc 64-bit division) against the compiler's 64-bit division;
 *  - the TIME / start-offset pipeline (stamp, stamp_slots, solve_header, start_error, finish: 64-bit -> 32-bit and the
 *    division helper) on random frames, timings, starts and hints: the same slots, header and TIME;
 *  - recent_start / phase_start (a period's whole multiples), pwm_for_hz, the RMT tick conversions, frame_max_us and
 *    the SPI encoder.
 * Round 11 (CPU, no behaviour change): finish_ counting the 1 bits once and skipping unchanged SETs (the pipeline test
 * above), set_fill (a SET without its CRC at queue time) + the seal = the header's ultra_slot_set; the SPI encoder's
 * nibble table (above) and the equal-period shortcut (no 1-bit count when '0' and '1' take as long: finish_ with
 * equal-period timings above, ultra_wire_ns against its old code).
 * The library's sources are compiled into this file (their static functions). Usage: test_equiv [rounds]
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

static uint64_t rs = 0x2545F4914F6CDD1Dull;
static uint64_t r64(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}
static uint32_t r32(void) { return (uint32_t)(r64() >> 20); }
static uint32_t pick(uint32_t lo, uint32_t hi) { return lo + r32() % (hi - lo + 1u); }

/* ------------------------------------------------------------------------------------------ the old code, verbatim */
static void old_stamp(uint8_t *s, int64_t start_us, int64_t hdr_us, int floor, uint32_t duration)
{
    int64_t d, ms;
    if (start_us == ULTRA_NO_START) {
        return;
    }
    d = (floor && start_us < hdr_us) ? 0 : start_us - hdr_us;
    ms = d >= 0 ? (d + 500) / 1000 : -((-d + 499) / 1000);
    if (s[0] == ULTRA_SET) {
        if (ms < -32768) {
            const uint64_t lost = (uint64_t)(-32768 - ms);
            duration = lost >= duration ? 0u : duration - (uint32_t)lost;
        }
        ultra_put24(s + ULTRA_PAYLOAD + 22, duration);
        ms = ms < -32768 ? -32768 : ms > 32767 ? 32767 : ms;
        ultra_put16(s + ULTRA_PAYLOAD + 25, (uint32_t)(uint16_t)(int16_t)ms);
    } else if (s[0] == ULTRA_EFFECT || s[0] == ULTRA_PIXEL_FX) {   /* (0.2.8: PIXEL_FX's START is stamped as EFFECT's) */
        ms = ms < INT32_MIN ? INT32_MIN : ms > INT32_MAX ? INT32_MAX : ms;
        ultra_put32(s + ULTRA_PAYLOAD + 1, (uint32_t)(int32_t)ms);
    }
    ultra_slot_seal(s);
}

static void old_stamp_slots(ultra_sender_t *l, int64_t t, uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < l->n; i++) {
        const ultra_sender_board_t *b = &l->boards[i];
        old_stamp(out + ULTRA_SLOT_BYTES * i, b->f_start, t, b->f_floor, b->f_dur);
    }
}

static int64_t old_solve_header(ultra_sender_t *l, int64_t start_us, uint8_t *out, int64_t hint)
{
    const ultra_timing_t *tm = &l->timing;
    const uint32_t lead = ULTRA_SLOT_BYTES * l->n + 3u;          /* the slots and the header's fixed bytes */
    const uint64_t p0 = (uint64_t)tm->t0h_ns + tm->t0l_ns, p1 = (uint64_t)tm->t1h_ns + tm->t1l_ns;
    const uint64_t pmin = p0 < p1 ? p0 : p1, pmax = p0 < p1 ? p1 : p0;
    const uint32_t lmin = tm->t0l_ns < tm->t1l_ns ? tm->t0l_ns : tm->t1l_ns;
    const uint32_t lmax = tm->t0l_ns < tm->t1l_ns ? tm->t1l_ns : tm->t0l_ns;
    uint8_t *h = out + ULTRA_SLOT_BYTES * l->n;
    uint64_t fixed;
    int64_t lo, hi, t, best = hint, best_err = INT64_MAX, best_d = INT64_MAX;
    uint32_t ones;
    ultra_build_header_flags(h, 0, l->hdr_flags);                /* (MAGIC, VERSION / FLAGS: fixed) */
    ones = ones_in(out, lead);
    fixed = (uint64_t)ones * p1 + ((uint64_t)lead * 8u - ones) * p0;
    lo = start_us + (int64_t)((fixed + 40u * pmin - lmax) / 1000u) - 1;
    hi = start_us + (int64_t)((fixed + 40u * pmax - lmin + 999u) / 1000u) + 1;
    for (t = lo; t <= hi; t++) {
        int64_t m, err, d;
        uint32_t k;
        ultra_build_header_flags(h, (uint32_t)t, l->hdr_flags);
        k = ones_in(h + 3, 5);
        m = start_us + (int64_t)((fixed + (uint64_t)k * p1 + (40u - (uint64_t)k) * p0 - ((h[7] & 1u) ? tm->t1l_ns
                                                                                                    : tm->t0l_ns) +
                                  500u) / 1000u);
        err = m > t ? m - t : t - m;
        d = t > hint ? t - hint : hint - t;
        if (err < best_err || (err == best_err && err != 0 && d < best_d)) {   /* (exact: the earliest) */
            best_err = err;
            best_d = d;
            best = t;
        }
    }
    ultra_build_header_flags(h, (uint32_t)best, l->hdr_flags);
    return best;
}

static int64_t old_start_error(const ultra_sender_t *l, const uint8_t *out, int64_t e)
{
    int64_t worst = 0;
    uint32_t i;
    for (i = 0; i < l->n; i++) {
        const ultra_sender_board_t *b = &l->boards[i];
        const uint8_t *sl = out + ULTRA_SLOT_BYTES * i;
        int64_t off, d;
        if (b->f_start == ULTRA_NO_START) {
            continue;
        }
        if (sl[0] == ULTRA_SET) {
            off = (int16_t)ultra_get16(sl + ULTRA_PAYLOAD + 25);
            if (off == -32768 || off == 32767) {
                continue;
            }
        } else if (sl[0] == ULTRA_EFFECT || sl[0] == ULTRA_PIXEL_FX) {
            off = (int32_t)ultra_get32(sl + ULTRA_PAYLOAD + 1);
        } else {
            continue;
        }
        d = e + off * 1000 - ((b->f_floor && b->f_start < e) ? e : b->f_start);
        d = d < 0 ? -d : d;
        worst = d > worst ? d : worst;
    }
    return worst;
}

static int64_t old_finish(ultra_sender_t *l, int64_t start_us, uint8_t *out)
{
    const uint32_t bytes = ultra_frame_bytes(l->n);
    const ultra_timing_t *tm = &l->timing;
    const uint64_t avg = ((uint64_t)tm->t0h_ns + tm->t0l_ns + tm->t1h_ns + tm->t1l_ns) / 2u;
    int64_t t = start_us + (int64_t)((uint64_t)bytes * 8u * avg / 1000u), sol = t, best_t = t, best_err = INT64_MAX;
    uint32_t pass;
    for (pass = 0; pass < STAMP_PASSES; pass++) {
        int64_t err;
        old_stamp_slots(l, t, out);
        sol = old_solve_header(l, start_us, out, t);
        if (sol == t) {
            best_t = t;                          /* the offsets are against the TIME the frame measures */
            best_err = -1;
            break;
        }
        err = old_start_error(l, out, sol);
        if (err < best_err) {
            best_err = err;
            best_t = t;
        }
        t = sol;
    }
    if (best_err >= 0) {
        old_stamp_slots(l, best_t, out);         /* (never settled: the best pass again) */
        sol = old_solve_header(l, start_us, out, best_t);
    }
    l->last_time_us = sol;
    return sol;
}

static int64_t old_recent_start(int64_t start_us, uint32_t aux, int64_t now_us)
{
    int64_t p;
    p = (int64_t)aux * 1000;
    return start_us + (now_us - start_us) / p * p;
}

static int64_t old_phase_start(int64_t start_us, uint32_t period_ms)
{
    int64_t p;
    if (period_ms == 0u || start_us < 0) {
        return start_us;
    }
    p = (int64_t)period_ms * 1000;
    return start_us - start_us % p;
}

static void old_pwm_for_hz(uint32_t hz, uint16_t *preset, uint16_t *period)
{
    uint32_t i;
    uint64_t best = UINT64_MAX;
    *preset = 8;
    *period = 0;
    if (hz == 0u) {
        return;
    }
    for (i = 0; i < 15u; i++) {
        /* |f - hz| / hz with f = clock / steps, as |clock - hz x steps| / (hz x steps), compared in fixed point */
        const uint64_t hs = (uint64_t)hz * pwm_steps[i];
        const uint64_t diff = hs > PWM_CLOCK_HZ ? hs - PWM_CLOCK_HZ : PWM_CLOCK_HZ - hs;
        const uint64_t err = diff * 1000000u / hs;   /* ppm */
        if (err < best) {
            best = err;
            *preset = (uint16_t)i;
        }
    }
    if (best > 5000u) {
        uint64_t steps = ((uint64_t)PWM_CLOCK_HZ + hz / 2u) / hz;
        *period = (uint16_t)(steps < 1440u ? 1440u : steps > 65535u ? 65535u : steps);
    }
}

static uint32_t old_ticks(uint32_t ns, uint32_t hz)
{
    return (uint32_t)(((uint64_t)ns * hz + 500000000u) / 1000000000u);
}

static uint64_t old_tick_ns(uint32_t n, uint32_t hz)
{
    return ((uint64_t)n * 1000000000u + hz / 2u) / hz;
}

static uint32_t old_frame_max_us(const ultra_timing_t *t, uint32_t bytes)
{
    const uint32_t p0 = (uint32_t)t->t0h_ns + t->t0l_ns, p1 = (uint32_t)t->t1h_ns + t->t1l_ns;
    const uint32_t worst = p0 > p1 ? p0 : p1;
    return (uint32_t)((uint64_t)bytes * 8u * worst / 1000u) + t->reset_ns / 1000u;
}

static uint64_t old_wire_ns(const ultra_timing_t *t, const uint8_t *bytes, uint32_t n)
{
    uint32_t i, ones = 0;
    uint64_t bits, all;
    if (n == 0u) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint32_t v;
        for (v = bytes[i]; v != 0u; v &= v - 1u) {
            ones++;
        }
    }
    bits = (uint64_t)n * 8u;
    all = (uint64_t)ones * ((uint32_t)t->t1h_ns + t->t1l_ns) + (bits - ones) * ((uint32_t)t->t0h_ns + t->t0l_ns);
    return all - ((bytes[n - 1u] & 1u) ? t->t1l_ns : t->t0l_ns);   /* the last bit ends with its falling edge */
}

static uint32_t old_spi_encode(const uint8_t *bytes, uint32_t nbytes, uint8_t *out)
{
    uint32_t i, b;
    for (i = 0; i < nbytes; i++) {
        uint64_t w = 0;                                  /* 8 bits x 5 SPI bits = 40 bits */
        for (b = 0; b < 8u; b++) {
            w = (w << 5) | (((bytes[i] >> (7u - b)) & 1u) ? 0x1Cu : 0x10u);   /* 11100 / 10000 */
        }
        for (b = 0; b < 5u; b++) {
            out[5u * i + b] = (uint8_t)(w >> (32u - 8u * b));
        }
    }
    return nbytes * 5u;
}

/* ------------------------------------------------------------------------------------------ the tests */
static uint64_t edge64(void)
{
    static const uint64_t e[] = { 0, 1, 2, 999, 1000, 1001, 0xFFFFu, 0x10000u, 0xFFFFFFFFu, 0x100000000ull,
                                  0x1FFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull };
    const uint32_t k = r32() % 8u;
    return k < 2u ? e[r32() % (sizeof e / sizeof e[0])] : k < 4u ? r64() >> (r32() % 64u) : r64();
}

static void test_udiv(uint32_t rounds)
{
    uint32_t i, bad = 0;
    for (i = 0; i < rounds * 40u; i++) {
        const uint64_t x = edge64();
        uint64_t d = edge64();
        const uint32_t k = r32() % 4u;
        d = k == 0u ? (uint64_t)pick(1, 0xFFFF) : k == 1u ? (uint64_t)(r32() | 1u) : d == 0u ? 1u : d;
        bad += ultra_udiv64_(x, d) != x / d;
    }
    CHECK(bad == 0u, "ultra_udiv64_ = the 64-bit division: %u inputs (16-bit, 32-bit, 64-bit divisors, edges), %u "
          "different", rounds * 40u, bad);
}

/* the stamping pipeline: random lines (timing, boards, slots, starts), the old and the new finish on copies */
static ultra_sender_t lo_, ln_;
static ultra_sender_board_t bo_[64], bn_[64];
static uint8_t fo_[ULTRA_SENDER_FRAME_BYTES(64)], fn_[ULTRA_SENDER_FRAME_BYTES(64)];

static void test_finish(uint32_t rounds)
{
    uint32_t i, b, bad = 0, unsettled = 0;
    for (i = 0; i < rounds; i++) {
        ultra_sender_config_t c;
        const uint16_t n = (uint16_t)pick(1, 64);
        const int64_t start = (int64_t)(r64() >> pick(2, 40)) - (int64_t)((r32() & 1u) ? 0u : r32());
        int64_t to, tn;
        ultra_sender_config_init(&c, n, 1);
        if (r32() % 3u == 0u) {                  /* equal periods (SPI, a constant bit period) */
            do {
                c.timing.t0h_ns = (uint16_t)pick(220, 380);
                c.timing.t0l_ns = (uint16_t)pick(450, (r32() & 1u) ? 1500u : 9000u);
                c.timing.t1h_ns = (uint16_t)pick(580, 2000);
                c.timing.t1l_ns = (uint16_t)((uint32_t)c.timing.t0h_ns + c.timing.t0l_ns - c.timing.t1h_ns);
            } while (ultra_timing_check(&c.timing) != 0);
        } else if (r32() & 1u) {
            c.timing.t0h_ns = (uint16_t)pick(220, 380);
            c.timing.t0l_ns = (uint16_t)pick(450, (r32() & 1u) ? 1500u : 10000u);
            c.timing.t1h_ns = (uint16_t)pick(580, 2000);
            c.timing.t1l_ns = (uint16_t)pick(450, (r32() & 1u) ? 1500u : 10000u);
        }
        (void)ultra_sender_init(&lo_, &c, bo_, 0, 0);
        for (b = 0; b < n; b++) {
            uint8_t *s = fo_ + ULTRA_SLOT_BYTES * b;
            uint32_t j;
            const uint32_t kind = r32() % 4u;
            ultra_sender_board_t *x = &bo_[b];
            for (j = 0; j < ULTRA_SLOT_BYTES; j++) {
                s[j] = (uint8_t)r32();
            }
            s[0] = kind == 0u ? ULTRA_SET : kind == 1u ? ULTRA_EFFECT : (uint8_t)(r32() % 12u);
            x->f_start = (r32() % 5u) == 0u ? ULTRA_NO_START
                       : start + (int64_t)(int32_t)r32() / (int64_t)pick(1, 4000) +
                             ((r32() % 9u) == 0u ? (int64_t)(int32_t)r32() * 1000 : 0);
            x->f_floor = (uint8_t)(r32() & 1u);
            x->f_dur = r32() % 0x1000000u;
        }
        lo_.hdr_flags = (uint8_t)((r32() % 4u) == 0u ? ULTRA_HDR_RW : 0u);
        ln_ = lo_;
        memcpy(bn_, bo_, sizeof bo_);
        ln_.boards = bn_;
        memcpy(fn_, fo_, sizeof fo_);
        to = old_finish(&lo_, start, fo_);
        tn = ultra_sender_finish_(&ln_, start, fn_);
        bad += to != tn || memcmp(fo_, fn_, ultra_frame_bytes(n)) != 0 || lo_.last_time_us != ln_.last_time_us;
        unsettled += to - start > 0 && old_start_error(&lo_, fo_, to) > 500;
    }
    CHECK(bad == 0u, "finish (stamp, solve_header, start_error): %u random frames (1-64 boards, legal timings, starts, "
          "SET / EFFECT / other slots), %u different (%u never settled)", rounds, bad, unsettled);
}

static void test_periods(uint32_t rounds)
{
    uint32_t i, bad_r = 0, bad_p = 0;
    for (i = 0; i < rounds * 20u; i++) {
        ultra_sender_item_t it;
        const uint32_t aux = (r32() % 4u) == 0u ? pick(1, 0xFFFFFFu) : r32() | 1u;
        const int64_t start = (int64_t)(r64() >> pick(1, 40)) - (int64_t)(r64() >> pick(20, 50));
        const int64_t now = start + DAY_US + (int64_t)(r64() >> pick(2, 30));
        memset(&it, 0, sizeof it);
        it_set_start(&it, start);
        it.aux = aux;
        it.slot[0] = ULTRA_EFFECT;
        bad_r += recent_start(&it, now) != old_recent_start(start, aux, now);
        {
            const int64_t s2 = (r32() & 1u) ? (int64_t)(r64() >> pick(1, 60)) : (int64_t)(int32_t)r32();
            const uint32_t pms = (r32() % 3u) == 0u ? 0u : (r32() % 2u) ? pick(1, 100000) : r32();
            bad_p += phase_start(s2, pms) != old_phase_start(s2, pms);
        }
    }
    CHECK(bad_r == 0u && bad_p == 0u, "recent_start / phase_start: %u inputs each (periods to 2^32 ms, starts over "
          "the int64 range), %u / %u different", rounds * 20u, bad_r, bad_p);
}

static void test_bits(uint32_t rounds)
{
    uint32_t i, bad = 0;
    for (i = 0; i < rounds * 40u; i++) {
        const uint32_t hz = (r32() % 3u) == 0u ? pick(1, 2000000) : r32();
        const uint32_t ns = (r32() & 1u) ? pick(0, 70000) : r32();
        const uint32_t res = (r32() & 1u) ? pick(1, 160000000) : r32() | 1u;
        uint16_t p1, q1, p2, q2;
        ultra_timing_t t;
        t.t0h_ns = (uint16_t)r32();
        t.t0l_ns = (uint16_t)r32();
        t.t1h_ns = (uint16_t)r32();
        t.t1l_ns = (uint16_t)r32();
        t.reset_ns = r32();
        ultra_pwm_for_hz(hz, &p1, &q1);
        old_pwm_for_hz(hz, &p2, &q2);
        bad += p1 != p2 || q1 != q2;
        bad += ticks(ns, res) != old_ticks(ns, res);
        bad += tick_ns(ns, res) != old_tick_ns(ns, res);
        {
            const uint32_t by = r32() % 0x1000000u;
            bad += ultra_frame_max_us(&t, by) != old_frame_max_us(&t, by);
        }
    }
    {
        uint8_t in[256], a[1280], b2[1280];
        for (i = 0; i < 256u; i++) {
            in[i] = (uint8_t)i;
        }
        bad += ultra_spi_encode(in, 256, a) != old_spi_encode(in, 256, b2) || memcmp(a, b2, sizeof a) != 0;
    }
    CHECK(bad == 0u, "pwm_for_hz, ticks / tick_ns (RMT), frame_max_us: %u inputs each; the SPI encoder: every byte; "
          "%u different", rounds * 40u, bad);
}

/* round 11: set_fill + ultra_slot_seal = ultra_slot_set (finish_ seals every SET it sends) */
static void test_set_fill(uint32_t rounds)
{
    uint32_t i, j, bad = 0;
    for (i = 0; i < rounds * 20u; i++) {
        ultra_values_t v;
        uint8_t a[ULTRA_SLOT_BYTES], b[ULTRA_SLOT_BYTES];
        const uint8_t light = (uint8_t)(r32() % 16u), seq = (uint8_t)r32(), flags = (uint8_t)r32();
        const uint32_t dur = (r32() & 1u) ? r32() : r32() % 0x1000000u;
        v.m = (uint16_t)r32();
        for (j = 0; j < 5u; j++) {
            v.a[j] = (uint16_t)r32();
            v.b[j] = (uint16_t)r32();
        }
        memset(a, 0xA5, sizeof a);
        set_fill(a, light, seq, flags, &v, dur);
        ultra_slot_seal(a);
        ultra_slot_set(b, light, seq, flags, v.m, v.a, v.b, dur, 0);
        bad += memcmp(a, b, sizeof a) != 0;
    }
    CHECK(bad == 0u, "set_fill + the seal = ultra_slot_set: %u random SETs (values, flags, durations over 24 bits), %u "
          "different", rounds * 20u, bad);
}

/* round 11: ultra_wire_ns with its equal-period shortcut against the old count (random bytes, lengths, timings) */
static void test_wire(uint32_t rounds)
{
    static uint8_t buf[2056];
    uint32_t i, j, bad = 0, equal = 0;
    for (i = 0; i < rounds; i++) {
        ultra_timing_t t;
        const uint32_t n = pick(0, sizeof buf);
        t.t0h_ns = (uint16_t)r32();
        t.t0l_ns = (uint16_t)r32();
        t.t1h_ns = (uint16_t)r32();
        t.t1l_ns = (uint16_t)r32();
        t.reset_ns = r32();
        if (r32() & 1u) {                        /* equal periods (with 16-bit wrap: the fields' own) */
            t.t1l_ns = (uint16_t)((uint32_t)t.t0h_ns + t.t0l_ns - t.t1h_ns);
        }
        equal += (uint32_t)t.t0h_ns + t.t0l_ns == (uint32_t)t.t1h_ns + t.t1l_ns;
        for (j = 0; j < n; j++) {
            buf[j] = (uint8_t)r32();
        }
        bad += ultra_wire_ns(&t, buf, n) != old_wire_ns(&t, buf, n);
    }
    CHECK(bad == 0u, "ultra_wire_ns (equal-period shortcut) = the old count: %u frames (%u with equal periods), %u "
          "different", rounds, equal, bad);
}

int main(int argc, char **argv)
{
    const uint32_t rounds = argc > 1 ? (uint32_t)atoi(argv[1]) : 20000u;
    test_udiv(rounds);
    test_finish(rounds / 4u);
    test_periods(rounds);
    test_bits(rounds);
    test_set_fill(rounds);
    test_wire(rounds / 4u);
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL EQUIVALENCE TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
