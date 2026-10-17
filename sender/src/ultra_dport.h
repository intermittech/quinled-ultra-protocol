/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: a DIGITAL PORT (protocol 0.2.8; the QuinLED Diff-Power Ultra-Digital receiver). Header-only,
 * C99, no heap, no dependencies but ultra_protocol.h and ultra_sender_bits.h. The ESPHome component dig2analog_ultra
 * keeps a byte-identical copy (tests check it).
 *
 * A pixel receiver sits in the strip's data path and is polled over the same wire: every Ultra frame (a command, a
 * POLL, a NOP beacon) goes out BEHIND A MARKER in a latch gap the hub stretches, one slot (N = 1) + the header, and the
 * strip never sees it (the receiver zeroes everything from the marker to the hub's retake). This file decides WHEN a
 * transmission may start and what it looks like on the wire; the caller builds the bytes and drives the output.
 *
 *   pixels ... last edge | LOW >= 350 us | MARK HIGH 20 us | LOW 10 us | Ultra frame (320 bits) | LOW >= 300 us | pixels
 *   an RW frame (a POLL; t_end = its last falling edge): the hub lets go of the line by t_end + 150 us (the receiver
 *   answers once it has seen the line released >= 20 us, by t_end + 900 us), listens until t_end + W_D, retakes the
 *   line (LOW), and sends pixels >= 20 us later.
 *
 * The rules (docs/ultradigital-firmware.md section 5b, "Hub rules"; the protocol header ULTRA_DP_*):
 *  1. >= 350 us LOW after the last pixel edge before every marker; >= 300 us LOW after every marker frame without RW
 *     before pixels; pixels >= 20 us after a retake, and a marker >= 350 us LOW after it. The LOW is part of the
 *     transmission (the lead and the tail: one hardware-timed symbol each), so the output's timing doesn't depend on
 *     the caller's loop.
 *  2. Never pixels during a window.
 *  3. No marker for ULTRA_DP_POWERUP_MS after the port is powered or its stream starts.
 *  4. The back-off: a receiver that stops answering gets ULTRA_DP_TRIES polls in quick succession, ULTRA_DP_TRIES more
 *     ULTRA_DP_RETRY_S later, then no marker frame at all until the user intervenes (ultra_dp_retry). One answer in
 *     between resets it. At most 2 x TRIES one-frame glitches from a dead or misconfigured receiver.
 *  5. A receiver running a local effect (PIXEL_FX) gets no pixels, and a header (a marker frame: a command, a poll or a
 *     NOP) at least every ULTRA_DP_BEACON_MS: its effect clock comes from the header TIME.
 *  6. The switch local -> stream (PIXEL_FX 0): the receiver finishes its frame first; the hub's pixels wait for it
 *     (ultra_dp_handback).
 * Not thread-safe: one task.
 */
#ifndef ULTRA_DPORT_H
#define ULTRA_DPORT_H

#include <stdint.h>
#include "ultra_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ULTRA_DP_BEACON_MS   1000u    /* a local port gets a header at least this often */
#define ULTRA_DP_MARKER_US   20u      /* the marker's HIGH (15-40 us allowed), then LOW ULTRA_DP_GAP_HUB_US */
#define ULTRA_DP_GAP_HUB_US  10u

/* what the caller has to send (a bit mask) */
#define ULTRA_DP_W_CMD    1u          /* a command / refresh frame is queued (the scheduler's) */
#define ULTRA_DP_W_POLL   2u          /* an RW frame (a POLL) is due */
#define ULTRA_DP_W_PIXELS 4u          /* a pixel frame is ready */

typedef enum {
    ULTRA_DP_NONE = 0,                /* nothing may start now */
    ULTRA_DP_PIXELS,                  /* a pixel frame: lead LOW, the bits, tail (the chip's reset) */
    ULTRA_DP_FRAME,                   /* a marker frame without RW: lead LOW, marker, the bits, tail LOW 300 us */
    ULTRA_DP_POLL                     /* a marker frame with RW: lead LOW, marker, the bits; then the window */
} ultra_dp_kind_t;

typedef struct {
    ultra_dp_kind_t kind;
    uint32_t lead_low_us;             /* LOW before the marker / the first pixel bit (0: none) */
    uint32_t tail_low_us;             /* LOW after the last bit (0 after a POLL: the caller lets go of the line) */
    uint8_t  beacon;                  /* FRAME: sent as the header a local port is due (build a refresh / NOP frame) */
} ultra_dp_tx_t;

/* the back-off */
enum { ULTRA_DP_BO_OK = 0, ULTRA_DP_BO_FIRST, ULTRA_DP_BO_WAIT, ULTRA_DP_BO_SECOND, ULTRA_DP_BO_GAVE_UP };

typedef struct {
    uint32_t reset_us;                /* the chip's reset: the tail of a pixel frame */
    int64_t  free_us;                 /* the next transmission may start from here (the last one's end, a window's end) */
    uint32_t tail_us;                 /* the LOW the last transmission ended with */
    int64_t  quiet_until_us;          /* no marker frame before */
    int64_t  pixels_until_us;         /* no pixel frame before (the hand-back) */
    int64_t  last_marker_us;          /* the last marker frame's start (0: none yet) */
    int64_t  bo_resume_us;            /* BO_WAIT: the second series starts here */
    uint32_t polls, answered, missed; /* counters */
    uint8_t  after_window;            /* the line was let go of: the next transmission retakes it */
    uint8_t  local;                   /* a local effect runs */
    uint8_t  bo_phase, bo_misses;
    uint8_t  started;                 /* a marker frame was sent since power-up */
} ultra_dp_t;

static inline void ultra_dp_init(ultra_dp_t *dp, uint32_t pixel_reset_us)
{
    uint8_t *p = (uint8_t *)dp;
    uint32_t i;
    for (i = 0; i < (uint32_t)sizeof *dp; i++) {
        p[i] = 0;
    }
    dp->reset_us = pixel_reset_us;
}

/* rule 3: the port was powered (or its stream just started): no marker for ULTRA_DP_POWERUP_MS */
static inline void ultra_dp_powered(ultra_dp_t *dp, int64_t now_us)
{
    dp->quiet_until_us = now_us + (int64_t)ULTRA_DP_POWERUP_MS * 1000;
    dp->started = 0;
}

/* rule 5 / 6: a local effect runs (on) or not */
static inline void ultra_dp_local(ultra_dp_t *dp, int on)
{
    dp->local = (uint8_t)(on != 0);
}

/* the hub asked the receiver for the stream again (PIXEL_FX 0, applied at start_us on the host clock): the receiver
 * finishes its frame (local_frame_us on the wire) and a reset first; the hub's pixels wait for that */
static inline void ultra_dp_handback(ultra_dp_t *dp, int64_t start_us, uint32_t local_frame_us)
{
    int64_t t = start_us + (int64_t)local_frame_us + (int64_t)dp->reset_us;
    dp->local = 0;
    if (t > dp->pixels_until_us) {
        dp->pixels_until_us = t;
    }
}

/* rule 4: the user intervened (a button, a configuration change, a port power cycle the user commanded) */
static inline void ultra_dp_retry(ultra_dp_t *dp)
{
    dp->bo_phase = ULTRA_DP_BO_OK;
    dp->bo_misses = 0;
}

/* no marker frame may go out now (rule 4): the wait between the series, or given up */
static inline int ultra_dp_blocked(const ultra_dp_t *dp, int64_t now_us)
{
    return dp->bo_phase == ULTRA_DP_BO_GAVE_UP || (dp->bo_phase == ULTRA_DP_BO_WAIT && now_us < dp->bo_resume_us);
}

/* A POLL's window is over: answered (a valid answer to it came) or not. */
static inline void ultra_dp_poll_result(ultra_dp_t *dp, int64_t now_us, int answered)
{
    dp->polls++;
    if (answered) {
        dp->answered++;
        dp->bo_phase = ULTRA_DP_BO_OK;
        dp->bo_misses = 0;
        return;
    }
    dp->missed++;
    if (dp->bo_phase == ULTRA_DP_BO_OK) {
        dp->bo_phase = ULTRA_DP_BO_FIRST;
        dp->bo_misses = 0;
    }
    if (dp->bo_phase == ULTRA_DP_BO_FIRST || dp->bo_phase == ULTRA_DP_BO_SECOND) {
        if (++dp->bo_misses >= ULTRA_DP_TRIES) {
            dp->bo_misses = 0;
            if (dp->bo_phase == ULTRA_DP_BO_FIRST) {
                dp->bo_phase = ULTRA_DP_BO_WAIT;
                dp->bo_resume_us = now_us + (int64_t)ULTRA_DP_RETRY_S * 1000000;
            } else {
                dp->bo_phase = ULTRA_DP_BO_GAVE_UP;
            }
        }
    }
}

/* What may start at now_us. want: ULTRA_DP_W_*. NONE: nothing (the line is busy, a window is open, or only a marker frame
 * was wanted and rule 3 or 4 forbids it). Priority: a command, a poll, the header a local port is due, then pixels. */
static inline int ultra_dp_next(ultra_dp_t *dp, int64_t now_us, unsigned want, ultra_dp_tx_t *tx)
{
    int marker_ok;
    int64_t low;
    tx->kind = ULTRA_DP_NONE;
    tx->lead_low_us = tx->tail_low_us = 0;
    tx->beacon = 0;
    if (now_us < dp->free_us) {
        return 0;
    }
    if (dp->bo_phase == ULTRA_DP_BO_WAIT && now_us >= dp->bo_resume_us) {
        dp->bo_phase = ULTRA_DP_BO_SECOND;
        dp->bo_misses = 0;
    }
    marker_ok = !ultra_dp_blocked(dp, now_us) && now_us >= dp->quiet_until_us;
    /* the LOW the line has been at since the last transmission: its tail and the idle time after it; after a window
     * the hub retakes the line as it starts: nothing yet */
    low = dp->after_window ? 0 : (int64_t)dp->tail_us + (now_us - dp->free_us);
    if (marker_ok && ((want & ULTRA_DP_W_CMD) || (want & ULTRA_DP_W_POLL) ||
                      (dp->local && (dp->last_marker_us == 0 || now_us - dp->last_marker_us >= (int64_t)ULTRA_DP_BEACON_MS * 1000)))) {
        int64_t lead = low >= (int64_t)ULTRA_DP_QUIET_US ? 0 : (int64_t)ULTRA_DP_QUIET_US - low;
        if (dp->after_window && lead < (int64_t)ULTRA_DP_RETAKE_US) {
            lead = ULTRA_DP_RETAKE_US;
        }
        tx->lead_low_us = (uint32_t)lead;
        if (want & ULTRA_DP_W_CMD) {
            tx->kind = ULTRA_DP_FRAME;
        } else if (want & ULTRA_DP_W_POLL) {
            tx->kind = ULTRA_DP_POLL;
        } else {
            tx->kind = ULTRA_DP_FRAME;
            tx->beacon = 1;
        }
        tx->tail_low_us = tx->kind == ULTRA_DP_POLL ? 2u : ULTRA_DP_AFTER_US;   /* a POLL: 2 us LOW, then the caller lets go */
        return 1;
    }
    if ((want & ULTRA_DP_W_PIXELS) && !dp->local && now_us >= dp->pixels_until_us) {
        tx->kind = ULTRA_DP_PIXELS;
        tx->lead_low_us = dp->after_window ? ULTRA_DP_RETAKE_US : 0u;
        tx->tail_low_us = dp->reset_us;
        return 1;
    }
    return 0;
}

/* The transmission tx starts at start_us (the first symbol of its lead). wire_us: all of it on the wire (lead, marker,
 * gap, the bits, tail). A POLL: t_end_us = the frame's last falling edge, rate = ULTRA_RW_RATE_*: the window runs to
 * t_end + ultra_dp_window_us(rate); the next transmission retakes the line. */
static inline void ultra_dp_started(ultra_dp_t *dp, const ultra_dp_tx_t *tx, int64_t start_us, uint32_t wire_us,
                                    int64_t t_end_us, uint8_t rate)
{
    if (tx->kind == ULTRA_DP_POLL) {
        dp->free_us = t_end_us + (int64_t)ultra_dp_window_us(rate);
        dp->tail_us = 0;
        dp->after_window = 1;
    } else {
        dp->free_us = start_us + (int64_t)wire_us;
        dp->tail_us = tx->tail_low_us;
        dp->after_window = 0;
    }
    if (tx->kind != ULTRA_DP_PIXELS) {
        dp->last_marker_us = start_us > 0 ? start_us : 1;
        dp->started = 1;
    }
}

/* ------------------------------------------------------------------------------------------- ESP32 RMT words */
/* The RMT words of one transmission (ESP32 layout, as ultra_sender_bits.h): [lead LOW] [marker] [the bits] [tail LOW].
 * A word of 0 is absent. The caller fills bit0 / bit1 (a frame's, or a chip's), the others by the helpers below per
 * transmission. */
typedef struct {
    uint32_t lead, marker, bit0, bit1, tail;
} ultra_dp_rmt_t;

/* LOW for us microseconds as one word (two halves); 0 for 0 us. A duration over 2 x 32767 ticks is clipped. */
static inline uint32_t ultra_dp_rmt_low(uint32_t us, uint32_t resolution_hz)
{
    uint32_t ticks = (uint32_t)(((uint64_t)us * resolution_hz + 500000u) / 1000000u), a, b;
    if (ticks == 0u) {
        return 0u;
    }
    if (ticks > 2u * 0x7FFFu) {
        ticks = 2u * 0x7FFFu;
    }
    a = ticks / 2u;
    b = ticks - a;
    return (uint32_t)((a & 0x7FFFu) | ((b & 0x7FFFu) << 16));   /* levels 0, 0 */
}

/* the marker: HIGH ULTRA_DP_MARKER_US, LOW ULTRA_DP_GAP_HUB_US */
static inline uint32_t ultra_dp_rmt_marker(uint32_t resolution_hz)
{
    uint32_t h = (uint32_t)(((uint64_t)ULTRA_DP_MARKER_US * resolution_hz + 500000u) / 1000000u);
    uint32_t l = (uint32_t)(((uint64_t)ULTRA_DP_GAP_HUB_US * resolution_hz + 500000u) / 1000000u);
    return (uint32_t)((h & 0x7FFFu) | (1u << 15) | ((l & 0x7FFFu) << 16));
}

/* the number of symbols of a transmission of nbytes */
static inline uint32_t ultra_dp_rmt_count(const ultra_dp_rmt_t *s, uint32_t nbytes)
{
    return (s->lead != 0u) + (s->marker != 0u) + nbytes * 8u + (s->tail != 0u);
}

/* Symbols from symbol `pos` on (pos = symbols already written), up to max; returns how many; *done once the tail
 * (or the last bit when there is none) is out. The shape of ESP-IDF's rmt_simple_encoder callback. */
static inline uint32_t ultra_dp_rmt_encode(const ultra_dp_rmt_t *s, const uint8_t *bytes, uint32_t nbytes, uint32_t pos,
                                           uint32_t *out, uint32_t max, int *done)
{
    const uint32_t total = ultra_dp_rmt_count(s, nbytes);
    const uint32_t head = (s->lead != 0u) + (s->marker != 0u);
    uint32_t n = 0;
    while (n < max && pos < total) {
        uint32_t w;
        if (pos < head) {
            w = (pos == 0u && s->lead != 0u) ? s->lead : s->marker;
        } else if (pos < head + nbytes * 8u) {
            const uint32_t bit = pos - head;
            w = (bytes[bit >> 3] & (0x80u >> (bit & 7u))) ? s->bit1 : s->bit0;
        } else {
            w = s->tail;
        }
        out[n++] = w;
        pos++;
    }
    *done = pos >= total;
    return n;
}

#ifdef __cplusplus
}
#endif

#endif
