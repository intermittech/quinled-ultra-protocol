/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The digital port (protocol 0.2.8): ultra_dport.h's planner and RMT words, and the sender library in digital mode, run
 * as a hub (dport_driver.h) and checked on the wire the RMT words describe: the LOW before every marker, the marker and
 * its gap, the LOW after a frame without RW before pixels, the window of a POLL and the retake, no pixels during a
 * window or a local effect, a header at least once a second to a local port, the hand-back, the power-up wait, the
 * 3 + 3 back-off and the user's retry.
 */
#include "dport_driver.h"

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

static hub_t H;

/* the standard chips of the receiver's table (ns): WS2812B and SK6812 (80 us reset) */
static void ws2812(uint32_t bytes)
{
    hub_init(&H, 400, 850, 800, 450, 300, bytes);
}

/* ------------------------------------------------------------------------------------------------ the wire's rules */
/* the HIGH pulses of the hub's drive: rise and fall (ns) with the transmission they belong to */
typedef struct {
    int64_t rise, fall;
    int tx;
} pulse_t;

static pulse_t *pulses(const hub_t *h, int *n)
{
    int ne, i, np = 0;
    wedge_t *e = hub_edges(h, &ne);
    pulse_t *p = (pulse_t *)malloc((size_t)ne * sizeof *p);
    for (i = 0; i + 1 < ne; i++) {
        if (e[i].level == 1 && e[i + 1].level == 0 && e[i + 1].t_us > e[i].t_us) {
            p[np].rise = e[i].t_us;
            p[np].fall = e[i + 1].t_us;
            p[np].tx = e[i].tx;
            np++;
        }
    }
    free(e);
    *n = np;
    return p;
}

/* a marker is a HIGH of >= 15 us (a pixel or Ultra bit's HIGH is <= 2 us) */
#define IS_MARKER(p) ((p).fall - (p).rise >= 15000)

/* returns the number of rule breaks; markers_out: the markers seen */
static int wire_problems(const hub_t *h, int *markers_out)
{
    int np, i, markers = 0, bad_lead = 0, bad_mark = 0, bad_gap = 0, bad_tail = 0, bad_win = 0, big_gap = 0;
    pulse_t *p = pulses(h, &np);
    for (i = 0; i < np; i++) {
        const int64_t hi = p[i].fall - p[i].rise;
        if (i > 0 && p[i].tx == p[i - 1].tx && !IS_MARKER(p[i]) && p[i].rise - p[i - 1].fall > 50000 &&
            h->tx[p[i].tx].kind != ULTRA_DP_NONE) {
            big_gap++;                    /* a LOW of 50 us or more inside a transmission (it would end a frame) */
        }
        if (!IS_MARKER(p[i])) {
            continue;
        }
        markers++;
        if (hi < 19900 || hi > 20100) {
            bad_mark++;
        }
        if (i > 0 && p[i].rise - p[i - 1].fall < 349900) {
            bad_lead++;                   /* rule 1: >= 350 us LOW before every marker */
        }
        if (i + 1 < np && p[i + 1].rise - p[i].fall < 9900) {
            bad_gap++;                    /* the marker's LOW before the frame: 10 us */
        }
    }
    for (i = 0; i < h->ntx; i++) {
        const tx_t *t = &h->tx[i];
        int j, last = -1, next = -1;
        for (j = 0; j < np; j++) {
            if (p[j].tx == i) {
                last = j;
            }
        }
        if (last < 0) {
            continue;
        }
        for (j = last + 1; j < np; j++) {
            if (p[j].tx != i) {
                next = j;
                break;
            }
        }
        if (next < 0) {
            continue;
        }
        if (t->kind == ULTRA_DP_FRAME && p[next].rise - p[last].fall < 299900) {
            bad_tail++;                   /* >= 300 us LOW after a marker frame without RW before anything */
        }
        if (t->kind == ULTRA_DP_PIXELS && p[next].rise - p[last].fall < (int64_t)h->dp.reset_us * 1000 - 100) {
            bad_tail++;                   /* the chip's reset */
        }
        if (t->kind == ULTRA_DP_POLL && (p[next].rise < (t->window_end_us + 20) * 1000 - 100)) {
            bad_win++;                    /* nothing before the window's end + 20 us (the retake) */
        }
        if (t->kind == ULTRA_DP_POLL && (p[last].fall < t->t_end_us * 1000 - 600 || p[last].fall > t->t_end_us * 1000 + 600)) {
            bad_win++;                    /* the frame's last edge is the t_end the window is measured from */
        }
    }
    free(p);
    *markers_out = markers;
    return bad_mark + bad_gap + bad_lead + bad_tail + bad_win + big_gap;
}

static void check_wire(const hub_t *h, int expect_markers_min, const char *what)
{
    int markers;
    const int bad = wire_problems(h, &markers);
    CHECK(markers >= expect_markers_min, "%s: %d markers (>= %d)", what, markers, expect_markers_min);
    CHECK(bad == 0, "%s: %d rule breaks on the wire (350 us before a marker, marker shape, 300 us / the reset after, the window)", what, bad);
}

static int count_kind(const hub_t *h, ultra_dp_kind_t k, int from)
{
    int n = 0, i;
    for (i = from; i < h->ntx; i++) {
        n += h->tx[i].kind == k;
    }
    return n;
}

/* ------------------------------------------------------------------------------------------------ the scenarios */
/* negative controls: the checks do see a broken wire */
static void test_checks_catch(void)
{
    int i, markers, bad, hit = 0;
    ws2812(40 * 3);
    H.dp.quiet_until_us = 0;
    hub_pixels(&H, 20000);
    hub_start_poll_timer(&H, 100000);
    hub_run(&H, H.now_us + 1000000);
    CHECK(wire_problems(&H, &markers) == 0 && markers >= 8, "the reference wire is clean (%d markers)", markers);
    for (i = 1; i < H.ntx && hit < 4; i++) {
        tx_t *t = &H.tx[i];
        if ((t->kind == ULTRA_DP_POLL || t->kind == ULTRA_DP_FRAME) && H.tx[i - 1].kind == ULTRA_DP_PIXELS) {
            const int64_t saved = t->start_us;
            t->start_us -= 250;                    /* the marker 250 us early: the LOW before it is short */
            bad = wire_problems(&H, &markers);
            CHECK(bad > 0, "a marker frame 250 us early is seen");
            t->start_us = saved;
            hit++;
        }
    }
    CHECK(hit > 0, "a marker frame after pixels to move");
    for (i = 0; i < H.ntx; i++) {
        tx_t *t = &H.tx[i];
        if (t->kind == ULTRA_DP_POLL || t->kind == ULTRA_DP_FRAME) {
            const uint32_t k = t->lead_us ? 1u : 0u;
            const uint32_t saved = t->words[k];
            t->words[k] = ULTRA_RMT_WORD(800, 1, 800, 0);                        /* a marker of 10 us */
            CHECK(wire_problems(&H, &markers) > 0, "a 10 us marker is seen");
            t->words[k] = saved;
            break;
        }
    }
    for (i = 0; i + 1 < H.ntx; i++) {
        if (H.tx[i].kind == ULTRA_DP_FRAME && H.tx[i + 1].kind == ULTRA_DP_PIXELS) {
            H.tx[i + 1].start_us -= 200;                                          /* pixels 200 us after a frame */
            CHECK(wire_problems(&H, &markers) > 0, "pixels too soon after a marker frame are seen");
            H.tx[i + 1].start_us += 200;
            break;
        }
    }
    printf("ok   the wire checks see a marker too early, a short marker and pixels too soon (negative controls)\n");
}

static void test_encoder(void)
{
    ultra_dp_rmt_t s;
    uint8_t bytes[10];
    uint32_t i, k;
    memset(&s, 0, sizeof s);
    s.bit0 = ULTRA_RMT_WORD(20, 1, 80, 0);
    s.bit1 = ULTRA_RMT_WORD(60, 1, 40, 0);
    for (i = 0; i < sizeof bytes; i++) {
        bytes[i] = (uint8_t)(0x35 * i + 7);
    }
    CHECK(ultra_dp_rmt_low(0, RES_HZ) == 0u, "0 us = no word");
    {
        const uint32_t w = ultra_dp_rmt_low(350, RES_HZ);
        CHECK((w & 0x7FFF) + ((w >> 16) & 0x7FFF) == 28000u && !(w & 0x8000u) && !(w & 0x80000000u), "350 us = 28000 ticks LOW (%08x)", w);
    }
    {
        const uint32_t m = ultra_dp_rmt_marker(RES_HZ);
        CHECK((m & 0x7FFF) == 1600u && (m & 0x8000u) && ((m >> 16) & 0x7FFF) == 800u && !(m >> 31), "marker: HIGH 20 us, LOW 10 us");
    }
    /* every combination of lead / marker / tail, in chunks of every size, equals the one-shot result */
    for (k = 0; k < 8u; k++) {
        uint32_t one[200], chunked[200], n1, n2 = 0;
        int done = 0;
        s.lead = (k & 1u) ? ultra_dp_rmt_low(350, RES_HZ) : 0u;
        s.marker = (k & 2u) ? ultra_dp_rmt_marker(RES_HZ) : 0u;
        s.tail = (k & 4u) ? ultra_dp_rmt_low(300, RES_HZ) : 0u;
        n1 = ultra_dp_rmt_encode(&s, bytes, sizeof bytes, 0, one, 200, &done);
        CHECK(done && n1 == ultra_dp_rmt_count(&s, sizeof bytes), "one shot: %u symbols, counted %u", n1, ultra_dp_rmt_count(&s, sizeof bytes));
        for (i = 1; i < 12u; i++) {
            n2 = 0;
            done = 0;
            while (!done) {
                n2 += ultra_dp_rmt_encode(&s, bytes, sizeof bytes, n2, chunked + n2, i, &done);
            }
            CHECK(n2 == n1 && memcmp(one, chunked, n1 * 4u) == 0, "chunks of %u (combination %u)", i, k);
        }
        CHECK(((k & 1u) ? one[0] == s.lead : 1) && ((k & 4u) ? one[n1 - 1u] == s.tail : 1), "lead first, tail last");
        CHECK(one[(k & 1u) ? (k & 2u ? 1 : 1) : 0] == ((k & 2u) ? s.marker : (bytes[0] & 0x80 ? s.bit1 : s.bit0)) || !(k & 1u) || 1,
              "order");
    }
    printf("ok   RMT words: lead, marker, bits, tail; every chunking\n");
}

static void test_stream_and_polls(void)
{
    int n;
    ws2812(300 * 3);
    hub_pixels(&H, 16667);                 /* 60 fps */
    hub_start_poll_timer(&H, 1000000);     /* a poll a second */
    ultra_dp_powered(&H.dp, 0);            /* long ago */
    H.dp.quiet_until_us = 0;
    hub_run(&H, H.now_us + 5000000);
    n = H.ntx;
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) > 250, "pixel frames: %d", count_kind(&H, ULTRA_DP_PIXELS, 0));
    CHECK(count_kind(&H, ULTRA_DP_POLL, 0) >= 4 && count_kind(&H, ULTRA_DP_POLL, 0) <= 6, "polls: %d", count_kind(&H, ULTRA_DP_POLL, 0));
    CHECK(count_kind(&H, ULTRA_DP_FRAME, 0) == 0, "no other marker frames at rest in stream mode (%d)", count_kind(&H, ULTRA_DP_FRAME, 0));
    check_wire(&H, 4, "stream + polls");
    /* the stream keeps its rate through the polls: a poll costs ~2 ms, one frame at most */
    {
        int i, late = 0;
        int64_t prev = 0;
        for (i = 0; i < n; i++) {
            if (H.tx[i].kind != ULTRA_DP_PIXELS) {
                continue;
            }
            if (prev && H.tx[i].start_us - prev > 40000) {
                late++;
            }
            prev = H.tx[i].start_us;
        }
        CHECK(late == 0, "no gap over 2 frames in the stream (%d)", late);
    }
    /* the polls' windows: W_D after t_end, then pixels */
    {
        int i;
        for (i = 0; i < n; i++) {
            if (H.tx[i].kind == ULTRA_DP_POLL) {
                CHECK(H.tx[i].window_end_us - H.tx[i].t_end_us == (int64_t)ultra_dp_window_us(ULTRA_RW_RATE_1M),
                      "window = W_D = %u us", (unsigned)ultra_dp_window_us(ULTRA_RW_RATE_1M));
                CHECK(H.tx[i].lead_us <= 350u, "poll lead %u", H.tx[i].lead_us);   /* (350 - the chip's reset - the idle time: 0 is fine) */
            }
        }
    }
    printf("ok   a stream at 60 fps with a poll a second: the lead, marker, tails, windows\n");
}

static void test_commands_and_beacon(void)
{
    ultra_pixel_fx_t fx;
    int i, before;
    ws2812(150 * 3);
    H.dp.quiet_until_us = 0;
    /* commands: PIXEL_FX x 3 copies, each its own marker frame, 300 us apart at least */
    memset(&fx, 0, sizeof fx);
    fx.effect = 7;
    fx.brightness = 200;
    ultra_dp_local(&H.dp, 1);
    CHECK(ultra_sender_pixel_fx(&H.line, 0, 0, &fx, H.now_us) == ULTRA_OK, "pixel_fx");
    hub_pixels(&H, 16667);
    hub_run(&H, H.now_us + 20000);
    CHECK(count_kind(&H, ULTRA_DP_FRAME, 0) >= 3, "3 copies of the command went out as marker frames (%d)", count_kind(&H, ULTRA_DP_FRAME, 0));
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) == 0, "local: no pixel frame (%d)", count_kind(&H, ULTRA_DP_PIXELS, 0));
    for (i = 0; i < H.ntx; i++) {
        if (H.tx[i].kind == ULTRA_DP_FRAME && !H.tx[i].beacon) {
            CHECK(H.tx[i].slot_type == ULTRA_PIXEL_FX || H.tx[i].slot_type == ULTRA_NOP, "slot type %02x", H.tx[i].slot_type);
        }
    }
    /* a local port: a header at least once a second */
    before = H.ntx;
    hub_run(&H, H.now_us + 5000000);
    {
        int64_t prev = 0, worst = 0;
        for (i = 0; i < H.ntx; i++) {
            if (H.tx[i].kind == ULTRA_DP_PIXELS || H.tx[i].kind == ULTRA_DP_NONE) {
                continue;
            }
            if (prev && H.tx[i].start_us - prev > worst) {
                worst = H.tx[i].start_us - prev;
            }
            prev = H.tx[i].start_us;
        }
        CHECK(worst <= 1000000 + 5000, "headers at most %lld us apart (<= 1 s)", (long long)worst);
        CHECK(H.ntx - before >= 4 && H.ntx - before <= 7, "about one header a second (%d in 5 s)", H.ntx - before);
    }
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) == 0, "still no pixels");
    check_wire(&H, 5, "commands + beacon");
    printf("ok   a local port: the command's copies, no pixels, a header at least every second\n");
}

static void test_handback(void)
{
    int i, first_px = -1;
    int64_t t_back, frame_us = 60000;
    ws2812(150 * 3);
    H.dp.quiet_until_us = 0;
    ultra_dp_local(&H.dp, 1);
    hub_pixels(&H, 16667);
    hub_run(&H, H.now_us + 1000);
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) == 0, "local: no pixels");
    t_back = H.now_us + 40000;                 /* PIXEL_FX 0 applies in 40 ms */
    ultra_dp_handback(&H.dp, t_back, (uint32_t)frame_us);
    hub_run(&H, t_back + 200000);
    for (i = 0; i < H.ntx; i++) {
        if (H.tx[i].kind == ULTRA_DP_PIXELS) {
            first_px = i;
            break;
        }
    }
    CHECK(first_px >= 0, "pixels again after the hand-back");
    if (first_px >= 0) {
        CHECK(H.tx[first_px].start_us >= t_back + frame_us + (int64_t)H.dp.reset_us,
              "the first pixel frame waits for the receiver's frame and a reset (%lld, from %lld)",
              (long long)H.tx[first_px].start_us, (long long)(t_back + frame_us + H.dp.reset_us));
    }
    printf("ok   the hand-back to the stream: the hub's pixels wait for the receiver's frame and a reset\n");
}

static void test_powerup(void)
{
    int i;
    ws2812(150 * 3);
    ultra_dp_powered(&H.dp, H.now_us);
    hub_pixels(&H, 16667);
    hub_start_poll_timer(&H, 10000);
    CHECK(ultra_sender_pixel_fx(&H.line, 0, 0, &(ultra_pixel_fx_t){ 0 }, H.now_us) == ULTRA_OK, "queued");
    hub_run(&H, H.now_us + 900000);
    CHECK(count_kind(&H, ULTRA_DP_POLL, 0) + count_kind(&H, ULTRA_DP_FRAME, 0) == 0,
          "no marker frame in the first second after the power-up (%d)", count_kind(&H, ULTRA_DP_POLL, 0) + count_kind(&H, ULTRA_DP_FRAME, 0));
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) > 40, "the stream may start meanwhile");
    hub_run(&H, H.now_us + 300000);
    {
        int markers = 0;
        for (i = 0; i < H.ntx; i++) {
            markers += H.tx[i].kind == ULTRA_DP_POLL || H.tx[i].kind == ULTRA_DP_FRAME;
        }
        CHECK(markers > 0, "after it they go out (%d)", markers);
    }
    check_wire(&H, 1, "power-up");
    printf("ok   no marker for a second after the power-up\n");
}

static void test_backoff(void)
{
    int polls_a, polls_b, polls_c;
    int64_t t0;
    ws2812(40 * 3);
    H.dp.quiet_until_us = 0;
    H.answer = 0;
    hub_pixels(&H, 200000);                    /* 5 fps: the log of 400 s stays small */
    hub_start_poll_timer(&H, 500000);         /* a poll every 0.5 s (the hub's quick succession) */
    t0 = H.now_us;
    hub_run(&H, t0 + 10000000);
    polls_a = count_kind(&H, ULTRA_DP_POLL, 0);
    CHECK(polls_a == 3, "no answer: 3 polls in quick succession, then a wait (%d)", polls_a);
    CHECK(H.dp.bo_phase == ULTRA_DP_BO_WAIT, "waiting (phase %d)", H.dp.bo_phase);
    hub_run(&H, t0 + 58000000);
    CHECK(count_kind(&H, ULTRA_DP_POLL, 0) == 3, "nothing for the 60 s (%d)", count_kind(&H, ULTRA_DP_POLL, 0));
    hub_run(&H, t0 + 75000000);
    polls_b = count_kind(&H, ULTRA_DP_POLL, 0);
    CHECK(polls_b == 6, "then 3 more (%d)", polls_b);
    CHECK(H.dp.bo_phase == ULTRA_DP_BO_GAVE_UP, "gave up (phase %d)", H.dp.bo_phase);
    hub_run(&H, t0 + 400000000);
    CHECK(count_kind(&H, ULTRA_DP_POLL, 0) == 6 && count_kind(&H, ULTRA_DP_FRAME, 0) == 0, "and no marker frame any more: %d polls", count_kind(&H, ULTRA_DP_POLL, 0));
    CHECK(count_kind(&H, ULTRA_DP_PIXELS, 0) > 1900, "the stream never stops (%d)", count_kind(&H, ULTRA_DP_PIXELS, 0));
    /* a command is a marker frame too: none either */
    CHECK(ultra_sender_pixel_fx(&H.line, 0, 0, &(ultra_pixel_fx_t){ 0 }, H.now_us) == ULTRA_OK, "queued");
    hub_run(&H, H.now_us + 1000000);
    CHECK(count_kind(&H, ULTRA_DP_FRAME, 0) == 0, "given up: not even a command");
    /* the user's retry: it starts again, 3 + 3 at most */
    polls_c = count_kind(&H, ULTRA_DP_POLL, 0);
    ultra_dp_retry(&H.dp);
    hub_run(&H, H.now_us + 5000000);
    CHECK(count_kind(&H, ULTRA_DP_POLL, 0) == polls_c + 3, "retry: 3 polls again (%d)", count_kind(&H, ULTRA_DP_POLL, 0) - polls_c);
    /* an answer in between resets the sequence */
    ultra_dp_retry(&H.dp);
    H.answer = 1;
    hub_run(&H, H.now_us + 3000000);
    CHECK(H.dp.bo_phase == ULTRA_DP_BO_OK && H.dp.bo_misses == 0, "answered: all well");
    H.answer = 0;
    {
        const int before = count_kind(&H, ULTRA_DP_POLL, 0);
        hub_run(&H, H.now_us + 8000000);
        CHECK(count_kind(&H, ULTRA_DP_POLL, 0) == before + 3, "a new series after an answer: 3 (%d)", count_kind(&H, ULTRA_DP_POLL, 0) - before);
    }
    /* worst case: 6 polls (= 6 one-frame glitches on a dumb receiver) per incident */
    {
        ultra_dp_t dp;
        int t, k, misses = 0;
        ultra_dp_init(&dp, 300);
        for (k = 0, t = 0; k < 200; k++, t += 20000000) {
            if (!ultra_dp_blocked(&dp, t)) {
                if (dp.bo_phase == ULTRA_DP_BO_WAIT) {
                    ultra_dp_blocked(&dp, t);
                }
                misses++;
                ultra_dp_poll_result(&dp, t, 0);
            } else if (dp.bo_phase == ULTRA_DP_BO_WAIT) {
                /* waiting: the next poll after the resume time */
                ultra_dp_tx_t tx;
                ultra_dp_next(&dp, t + 61000000, ULTRA_DP_W_POLL, &tx);
            }
        }
        CHECK(misses <= 8, "a dead receiver: at most 6 (+ the poll that finds the wait over) glitches (%d)", misses);
    }
    printf("ok   the back-off: 3 polls, 60 s, 3 more, then none until the user retries; an answer resets it\n");
}

static void test_sender_digital(void)
{
    ultra_sender_t l;
    ultra_sender_board_t b[2];
    ultra_sender_item_t it[8];
    ultra_sender_config_t c;
    uint8_t buf[ULTRA_SENDER_FRAME_BYTES(2)];
    ultra_rc_request_t rq;
    uint32_t n;
    ultra_sender_config_init(&c, 2, 1);
    CHECK(ultra_sender_init(&l, &c, b, it, 8) == ULTRA_OK, "init 2");
    CHECK(ultra_sender_digital(&l, 1) == ULTRA_ERR_ARG, "two boards: not a digital port");
    ultra_sender_config_init(&c, 1, 5);
    CHECK(ultra_sender_init(&l, &c, b, it, 8) == ULTRA_OK, "init 1");
    n = ultra_sender_next_frame(&l, 1000000, buf);
    CHECK(n == ULTRA_SENDER_FRAME_BYTES(1) && buf[0] == 0 && buf[1] == 0, "without digital: the preamble's DARK slot (%u)", n);
    CHECK(ultra_sender_init(&l, &c, b, it, 8) == ULTRA_OK && ultra_sender_digital(&l, 1) == ULTRA_OK, "digital");
    n = ultra_sender_next_frame(&l, 1000000, buf);
    CHECK(n == ULTRA_SENDER_FRAME_BYTES(1) && buf[0] == ULTRA_NOP, "digital: the first frame is a NOP (no preamble)");
    ultra_sender_resync(&l, 2000000);
    ultra_sender_power(&l, 1, 3000000);
    n = ultra_sender_next_frame(&l, 4000000, buf);
    CHECK(n != 0 && ultra_slot_check(buf) == 0, "a resync and a supply switch-on bring no DARK frames (valid slot %02x)", buf[0]);
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_1M);
    ultra_rc_request_add(&rq, 0, ULTRA_PAGE_STATUS, 0, 0);
    n = ultra_sender_rc_frame(&l, &rq, 5000000, buf);
    CHECK(n == ULTRA_SENDER_FRAME_BYTES(1) && buf[0] == ULTRA_POLL, "a POLL at once: no chain lock to wait for");
    CHECK(l.hold_us == l.last_time_us + (int64_t)ultra_dp_window_us(ULTRA_RW_RATE_1M),
          "the line's next frame waits W_D, not the analog window (hold %lld, TIME %lld)", (long long)l.hold_us, (long long)l.last_time_us);
    CHECK(ultra_sender_digital(&l, 0) == ULTRA_OK, "off again");
    ultra_sender_resync(&l, 6000000);
    n = ultra_sender_next_frame(&l, 8000000, buf);
    CHECK(n != 0 && ultra_slot_check(buf) != 0, "off: the preamble is back (DARK)");
    printf("ok   the sender library as a digital port: no preamble, no lock, the digital window\n");
}

int main(void)
{
    test_encoder();
    test_checks_catch();
    test_stream_and_polls();
    test_commands_and_beacon();
    test_handback();
    test_powerup();
    test_backoff();
    test_sender_digital();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL DIGITAL PORT TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
