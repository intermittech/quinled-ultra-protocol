/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * A reference hub for a digital port, for the tests (test_dport.c): the sender library on one board in digital mode,
 * the planner (ultra_dport.h), pixel frames from a generator, polls on a timer, and an output that goes through the
 * real ultra_dp_rmt_encode() into RMT words at 80 MHz, which the checks decode back into levels and durations. It is
 * what a WLED / MoonLight / ESPHome hub does with these pieces, in a loop of its own with a clock it can jump.
 */
#ifndef DPORT_DRIVER_H
#define DPORT_DRIVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra_dport.h"
#include "ultra_sender.h"
#include "ultra_sender_rc.h"

#define RES_HZ     80000000u
#define MAX_TX     8192
#define MAX_PIX    900                   /* bytes per pixel frame */

typedef struct {
    ultra_dp_kind_t kind;
    int64_t  start_us;                   /* the first symbol (the lead) */
    int64_t  end_us;                     /* the last symbol's end */
    int64_t  t_end_us;                   /* marker frames: the Ultra frame's last falling edge */
    int64_t  window_end_us;              /* POLL */
    uint32_t lead_us, tail_us;
    uint8_t  beacon, slot_type;
    uint32_t nwords;
    uint32_t *words;                     /* the RMT words as the encoder gave them */
    uint32_t nbytes;                     /* the bytes it carried (a frame's, or the pixel frame's) */
    uint8_t  bytes[MAX_PIX];
} tx_t;

typedef struct {
    ultra_sender_t line;
    ultra_sender_board_t board[1];
    ultra_sender_item_t items[32];
    ultra_dp_t dp;
    ultra_rmt_t ultra;                   /* the frames' bit words */
    ultra_dp_rmt_t px;                   /* a pixel chip's bit words */
    uint32_t pix_bytes;
    uint8_t pixbuf[MAX_PIX];
    int64_t now_us;
    int64_t pixel_period_us;             /* a pixel frame is ready every period (0: none) */
    int64_t pixel_ready_us;
    int pixels_on;
    int64_t poll_period_us;              /* a poll is due every period (0: none) */
    int64_t poll_due_us;
    int polling;
    int answer;                          /* the receiver answers polls (1) or not (0) */
    int64_t loop_us;                     /* the caller's loop period (a window's end is noticed this late at most) */
    tx_t tx[MAX_TX];
    int ntx;
    uint64_t rng;
    uint32_t npix;                       /* pixel frames sent */
} hub_t;

static uint32_t hub_rand(hub_t *h)
{
    h->rng ^= h->rng << 13;
    h->rng ^= h->rng >> 7;
    h->rng ^= h->rng << 17;
    return (uint32_t)(h->rng >> 20);
}

static uint32_t ticks_of(double ns)
{
    return (uint32_t)(ns * RES_HZ / 1e9 + 0.5);
}

/* a chip: T0H / T0L / T1H / T1L ns, the reset us; pixel frames of `bytes` bytes */
static void hub_init(hub_t *h, double t0h, double t0l, double t1h, double t1l, uint32_t reset_us, uint32_t bytes)
{
    ultra_sender_config_t c;
    ultra_timing_t t = ULTRA_TIMING_RECOMMENDED;
    memset(h, 0, sizeof *h);
    h->rng = 0x9E3779B97F4A7C15ull;
    ultra_sender_config_init(&c, 1, 5);
    c.seed = 99;
    c.timing = t;
    if (ultra_sender_init(&h->line, &c, h->board, h->items, 32) != ULTRA_OK || ultra_sender_digital(&h->line, 1) != ULTRA_OK ||
        ultra_rmt_init(&h->ultra, &t, RES_HZ) != 0) {
        printf("hub_init failed\n");
        exit(2);
    }
    ultra_dp_init(&h->dp, reset_us);
    h->px.bit0 = ULTRA_RMT_WORD(ticks_of(t0h), 1, ticks_of(t0l), 0);
    h->px.bit1 = ULTRA_RMT_WORD(ticks_of(t1h), 1, ticks_of(t1l), 0);
    h->pix_bytes = bytes;
    for (uint32_t i = 0; i < bytes && i < MAX_PIX; i++) {
        h->pixbuf[i] = (uint8_t)hub_rand(h);
    }
    h->loop_us = 100;
    h->answer = 1;
    h->now_us = 1000000;
}

/* the words of a transmission through the encoder in chunks of random size (a real RMT refills in pieces) */
static uint32_t *encode_words(hub_t *h, const ultra_dp_rmt_t *s, const uint8_t *bytes, uint32_t n, uint32_t *count)
{
    const uint32_t total = ultra_dp_rmt_count(s, n);
    uint32_t *w = (uint32_t *)malloc((total + 8u) * 4u), pos = 0;
    int done = 0;
    while (!done) {
        const uint32_t chunk = 1u + hub_rand(h) % 70u;
        pos += ultra_dp_rmt_encode(s, bytes, n, pos, w + pos, chunk, &done);
    }
    *count = pos;
    if (pos != total) {
        printf("encoder: %u symbols, %u counted\n", pos, total);
        exit(2);
    }
    return w;
}

static void hub_start_poll_timer(hub_t *h, int64_t period_us)
{
    h->poll_period_us = period_us;
    h->poll_due_us = h->now_us + period_us;
    h->polling = 1;
}

static void hub_pixels(hub_t *h, int64_t period_us)
{
    h->pixel_period_us = period_us;
    h->pixel_ready_us = h->now_us;
    h->pixels_on = 1;
}

/* one pass of the hub's loop at h->now_us: start a transmission if the planner allows one. Returns 1 if it did. */
static int hub_step(hub_t *h)
{
    ultra_dp_tx_t t;
    unsigned want = 0;
    uint8_t buf[ULTRA_SENDER_FRAME_BYTES(1)];
    int64_t start, frame_start, t_end = 0;
    uint32_t len = 0, mark_us = 0;
    tx_t *r;
    ultra_dp_rmt_t s;
    if (ultra_sender_busy(&h->line)) {
        want |= ULTRA_DP_W_CMD;
    }
    if (h->polling && h->now_us >= h->poll_due_us) {
        want |= ULTRA_DP_W_POLL;
    }
    if (h->pixels_on && h->now_us >= h->pixel_ready_us) {
        want |= ULTRA_DP_W_PIXELS;
    }
    if (!ultra_dp_next(&h->dp, h->now_us, want, &t)) {
        return 0;
    }
    if (h->ntx >= MAX_TX) {
        printf("too many transmissions\n");
        exit(2);
    }
    r = &h->tx[h->ntx];
    memset(r, 0, sizeof *r);
    start = h->now_us;
    r->kind = t.kind;
    r->lead_us = t.lead_low_us;
    r->tail_us = t.tail_low_us;
    r->beacon = t.beacon;
    memset(&s, 0, sizeof s);
    s.lead = ultra_dp_rmt_low(t.lead_low_us, RES_HZ);
    if (t.kind == ULTRA_DP_PIXELS) {
        s.bit0 = h->px.bit0;
        s.bit1 = h->px.bit1;
        s.tail = ultra_dp_rmt_low(t.tail_low_us, RES_HZ);
        r->words = encode_words(h, &s, h->pixbuf, h->pix_bytes, &r->nwords);
        r->nbytes = h->pix_bytes;
        memcpy(r->bytes, h->pixbuf, h->pix_bytes);
        r->end_us = start;                /* below: from the words */
    } else {
        mark_us = ULTRA_DP_MARKER_US + ULTRA_DP_GAP_HUB_US;
        frame_start = start + t.lead_low_us + mark_us;
        if (t.kind == ULTRA_DP_POLL) {
            ultra_rc_request_t rq;
            ultra_rc_request_init(&rq, ULTRA_RW_RATE_1M);
            ultra_rc_request_add(&rq, 0, ULTRA_PAGE_STATUS, 0, 0);
            len = ultra_sender_rc_frame(&h->line, &rq, frame_start, buf);
            h->poll_due_us += h->poll_period_us;
        } else {
            len = ultra_sender_next_frame(&h->line, frame_start, buf);
        }
        if (len == 0u) {
            if (t.kind == ULTRA_DP_POLL) {
                h->poll_due_us = h->now_us + 1000;     /* refused: asked again later */
            }
            return 0;                     /* nothing to send after all (the refresh isn't due) */
        }
        t_end = frame_start + (int64_t)((ultra_wire_ns(&h->line.timing, buf, len) + 500u) / 1000u);
        r->slot_type = buf[0];
        s.marker = ultra_dp_rmt_marker(RES_HZ);
        s.bit0 = h->ultra.bit0;
        s.bit1 = h->ultra.bit1;
        s.tail = ultra_dp_rmt_low(t.tail_low_us, RES_HZ);
        r->words = encode_words(h, &s, buf, len, &r->nwords);
        r->nbytes = len;
        memcpy(r->bytes, buf, len);
        r->t_end_us = t_end;
    }
    /* the duration from the words */
    {
        double us = 0;
        uint32_t i;
        for (i = 0; i < r->nwords; i++) {
            const uint32_t w = r->words[i];
            us += (double)(w & 0x7FFFu) / 80.0 + (double)((w >> 16) & 0x7FFFu) / 80.0;
        }
        r->start_us = start;
        r->end_us = start + (int64_t)(us + 0.5);
    }
    ultra_dp_started(&h->dp, &t, start, (uint32_t)(r->end_us - start), t_end, ULTRA_RW_RATE_1M);
    if (t.kind == ULTRA_DP_POLL) {
        r->window_end_us = h->dp.free_us;
        ultra_dp_poll_result(&h->dp, h->dp.free_us, h->answer);
    }
    if (t.kind == ULTRA_DP_PIXELS) {
        h->pixel_ready_us += h->pixel_period_us;
        if (h->pixel_ready_us < start) {
            h->pixel_ready_us = start;     /* late: no catching up */
        }
        h->npix++;                         /* (every frame different: a counter in its first bytes) */
        h->pixbuf[0] = (uint8_t)(h->npix ^ 0x55u);
        h->pixbuf[1] = (uint8_t)(h->npix >> 8);
    }
    h->ntx++;
    return 1;
}

/* run the hub's loop until until_us: the clock moves with the loop period (+ jitter), or jumps to the line's free time */
static void hub_run(hub_t *h, int64_t until_us)
{
    while (h->now_us < until_us) {
        if (hub_step(h)) {
            continue;
        }
        h->now_us += h->loop_us + (int64_t)(hub_rand(h) % (uint32_t)(h->loop_us + 1));
    }
}

/* ---------------------------------------------------------------------------------- the wire, from the words */
typedef struct {
    int64_t t_us;                        /* the edge */
    int level;                           /* the level from here on */
    int tx;                              /* the transmission it belongs to */
} wedge_t;

/* all edges of all transmissions (the line between them: LOW, or the released window: HIGH is not modelled, the checks
 * look at the hub's own drive only). n edges returned. */
static wedge_t *hub_edges(const hub_t *h, int *n)
{
    int cap = 0, i;
    wedge_t *e;
    int ne = 0;
    for (i = 0; i < h->ntx; i++) {
        cap += (int)h->tx[i].nwords * 4 + 4;
    }
    e = (wedge_t *)malloc((size_t)cap * sizeof *e);
    for (i = 0; i < h->ntx; i++) {
        const tx_t *t = &h->tx[i];
        double us = (double)t->start_us;
        uint32_t k;
        for (k = 0; k < t->nwords; k++) {
            const uint32_t w = t->words[k];
            const double d0 = (double)(w & 0x7FFFu) / 80.0, d1 = (double)((w >> 16) & 0x7FFFu) / 80.0;
            e[ne].t_us = (int64_t)(us * 1000.0 + 0.5);   /* ns */
            e[ne].level = (int)((w >> 15) & 1u);
            e[ne].tx = i;
            ne++;
            us += d0;
            e[ne].t_us = (int64_t)(us * 1000.0 + 0.5);
            e[ne].level = (int)((w >> 31) & 1u);
            e[ne].tx = i;
            ne++;
            us += d1;
        }
        e[ne].t_us = (int64_t)(us * 1000.0 + 0.5);
        e[ne].level = 0;
        e[ne].tx = i;
        ne++;
    }
    *n = ne;
    return e;                            /* times in ns */
}

#endif
