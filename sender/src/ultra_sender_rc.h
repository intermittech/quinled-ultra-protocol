/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Wire-3 return-channel frames and response extraction. Requests serialize
 * part/bank context per receiver/SEQ. A matched answer never echoes that bank.
 * Chain scheduling uses ultra_rw_next_us(), including processing LOW. Marker
 * ports also wait for measured header training before polling.
 */
#ifndef ULTRA_SENDER_RC_H
#define ULTRA_SENDER_RC_H

#include "ultra_sender.h"

/* the LOW a transport holds after an RW frame's last falling edge before it lets go of the line: the protocol's lower bound (ULTRA_RW_RELEASE_MIN_US) + 20 us for
 * the release timer's latency; the upper bound ULTRA_RW_RELEASE_US (150 us) stays 50 us away */
#define ULTRA_RC_RELEASE_HOLD_US (ULTRA_RW_RELEASE_MIN_US + 20u)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t board;                    /* the board's position on the line */
    uint8_t  page;                     /* ULTRA_PAGE_* */
    uint8_t  nargs;
    uint8_t  args[23];                 /* the page's arguments, from POLL slot byte 7 on */
} ultra_rc_ask_t;

typedef struct {
    uint8_t rate;                      /* ULTRA_RW_RATE_* */
    uint8_t count;                     /* answers asked: R */
    ultra_rc_ask_t ask[ULTRA_RW_MAX_SLOTS];
} ultra_rc_request_t;

void ultra_rc_request_init(ultra_rc_request_t *r, uint8_t rate);
/* ask `board` for `page`: boards in rising order, each once; answer index = the order of the calls. The page's
 * arguments (POLL slot byte 7 on) as the header gives them per page (POWER, SUPPLY, CAL_TEST, ACKS; DIAG: one
 * byte, the part: 0 the running board, 1 the fault memory and the calibration trend; LIFETIME none). A page the board
 * doesn't have is answered with page ULTRA_PAGE_UNSUPPORTED. */
int  ultra_rc_request_add(ultra_rc_request_t *r, uint16_t board, uint8_t page, const uint8_t *args, uint32_t nargs);
/* ask `board` for page LIFE_SYNC (ULTRA_FEAT_LIFE_SYNC): the hub's ledger of the board's lifetime counters for
 * it to merge. uid16: the board's (its answers'); lifetime: LIFETIME page bytes 0..15 as the ledger has them (powered,
 * lit, power-ups, trips, dips; the same layout); total_wh: its lifetime energy, Wh; records_lo: the low 8 bits of the
 * board's RECORDS (LIFETIME 21) the ledger was reconciled against. The board merges once per power-up, only with its
 * own UID16 and RECORDS byte, each counter raised by at most its step, never lowered (the header's ULTRA_LS_*). Hub:
 * once per board power-up, when the ledger is ahead of the board's LIFETIME; then the power-off notice if the line is
 * to be switched off (no POLL until the cut: ultra_sender_rc_frame refuses). The ledger is the hub's own accounting,
 * never an echo of the board's reads: the sessions' seconds and mWh in fine totals sent as whole units, POWER_UPS =
 * the ledger + the power-on just performed, a board read raising a time / energy total only by its lead beyond one
 * unit (the wiki: Return channel, "The hub's ledger"). ULTRA_ERR_ARG as ultra_rc_request_add. */
int  ultra_rc_request_life_sync(ultra_rc_request_t *r, uint16_t board, uint16_t uid16, const uint8_t lifetime[16],
                                uint32_t total_wh, uint8_t records_lo);

/* The RW frame for request r into buf, its first bit at now_us; returns its length, or 0 when the line can't poll
 * now (preamble, switch sequence, < ULTRA_RW_LOCK_MS after the preamble, a power-off notice given and the line not
 * woken since, an empty or invalid request). Send it like
 * any frame, release the line, receive the answers; the line's next frame waits for the window. */
uint32_t ultra_sender_rc_frame(ultra_sender_t *l, const ultra_rc_request_t *r, int64_t now_us, uint8_t *buf);
/* While false, send ordinary training headers at the configured refresh rate. */
int ultra_sender_rc_ready(const ultra_sender_t *l,int64_t now_us);
/* the start of answer `index`'s response slot after the frame's TIME, us */
uint32_t ultra_rc_slot_start_us(uint8_t rate, uint8_t index);

/* One response slot's received bytes (rx, n; ferr: a framing error per byte, or NULL): the answer is the first 32
 * bytes from a SYNC that pass the CRC-16 without a framing error; each SYNC is tried in turn (the turnaround checks'
 * pulses arrive as junk bytes, which may read 0xA5 too, then the 0xFF lead byte). 0 = a valid answer copied to
 * answer, -1 = none (answer then holds scratch). */
int ultra_rc_answer(const uint8_t *rx, const uint8_t *ferr, uint32_t n, uint8_t answer[32]);
/* an answer's fields: page, index, RFLAGS (ULTRA_RSP_*), SEQ, UID16; DATA from answer + ULTRA_RSP_DATA (24 bytes,
 * offsets ULTRA_ST_* / ULTRA_PW_* / ... per page) */
static inline uint8_t  ultra_rc_page(const uint8_t *a) { return a[1]; }
static inline uint8_t  ultra_rc_index(const uint8_t *a) { return (uint8_t)(a[2] >> 4); }
static inline uint8_t  ultra_rc_rflags(const uint8_t *a) { return (uint8_t)(a[2] & 0x0Fu); }
static inline uint8_t  ultra_rc_seq(const uint8_t *a) { return a[3]; }
static inline uint16_t ultra_rc_uid16(const uint8_t *a) { return (uint16_t)ultra_get16(a + 4); }
/* DATA fields: ultra_get16 / ultra_get24 / ultra_get32 (big-endian) at answer + ULTRA_RSP_DATA + the page's offset.
 * Shortcuts: the features of an IDENTITY answer to part 1 (POLL argument 0 = ULTRA_ID_PART_FEATURES; part 0
 * is the board): ultra_rc_features its bits 0-31 as the ULTRA_FEAT_* masks (what this board takes, e.g.
 * ULTRA_FEAT_RETRY; ULTRA_FEAT_STANDBY: it takes the power-off notice, ultra_sender_power_off),
 * ultra_rc_feature any bit 0-191; and a STATUS answer's reason: why its light isn't as the host set it now,
 * ULTRA_ST_WHY_* (0 = as set) */
static inline uint32_t ultra_rc_features(const uint8_t *a)
{
    return ultra_get32(a + ULTRA_RSP_DATA + ULTRA_IDF_FEATURES);
}
static inline int ultra_rc_feature(const uint8_t *a, uint32_t bit)
{
    return ultra_feat_has(a + ULTRA_RSP_DATA + ULTRA_IDF_FEATURES, bit);
}
static inline uint8_t ultra_rc_status_why(const uint8_t *a)
{
    return (uint8_t)(a[ULTRA_RSP_DATA + ULTRA_ST_STATE] >> 4);   /* ULTRA_ST_WHY_SHIFT */
}
/* a LIFE_SYNC answer: DATA bytes 0..15 as LIFETIME's (after the merge), then the lifetime energy (Wh), the
 * result (ULTRA_LS_R_*), the counters raised (ULTRA_LS_F_*) and RECORDS' low 16 bits */
static inline uint32_t ultra_rc_ls_total_wh(const uint8_t *a)
{
    return ultra_get32(a + ULTRA_RSP_DATA + ULTRA_LS_A_TOTAL_WH);
}
static inline uint8_t ultra_rc_ls_result(const uint8_t *a) { return a[ULTRA_RSP_DATA + ULTRA_LS_A_RESULT]; }
static inline uint8_t ultra_rc_ls_applied(const uint8_t *a) { return a[ULTRA_RSP_DATA + ULTRA_LS_A_APPLIED]; }
static inline uint16_t ultra_rc_ls_records(const uint8_t *a)
{
    return (uint16_t)ultra_get16(a + ULTRA_RSP_DATA + ULTRA_LS_A_RECORDS);
}

/* PIXEL page 0x10 part 0 of an Diff-Power-UR-Digital-M receiver (POLL argument 0 = 0; ULTRA_PXL_*): the line it
 * sits on. mode_in_effect 0 = still deciding (AUTO), ULTRA_PXLM_STREAM, ULTRA_PXLM_CHAIN; flags ULTRA_PXL_F_* (locked: a right
 * Ultra frame within the last 2 s; long: a frame of more than one slot was seen: boards behind this one). busy: relay
 * windows given up at the turnaround (the line above still driven). The counters wrap (u16). Returns ULTRA_OK, or
 * ULTRA_ERR_ARG when the answer is not PIXEL. */
typedef struct {
    uint8_t  config, mode_in_effect, flags, slots;
    uint16_t frames, non_ultra, long_frames, windows, answers, missed, markers, busy;
    uint32_t bits;                     /* of the last Ultra frame */
} ultra_pixel_line_t;
static inline int ultra_rc_pixel_line(const uint8_t *a, ultra_pixel_line_t *o)
{
    const uint8_t *d = a + ULTRA_RSP_DATA;
    if (a[1] != ULTRA_PAGE_PIXEL) {
        return ULTRA_ERR_ARG;
    }
    o->config = d[ULTRA_PXL_CONFIG];
    o->mode_in_effect = d[ULTRA_PXL_MODE];
    o->flags = d[ULTRA_PXL_FLAGS];
    o->slots = d[ULTRA_PXL_SLOTS];
    o->frames = (uint16_t)ultra_get16(d + ULTRA_PXL_FRAMES);
    o->non_ultra = (uint16_t)ultra_get16(d + ULTRA_PXL_NONULTRA);
    o->long_frames = (uint16_t)ultra_get16(d + ULTRA_PXL_LONG);
    o->windows = (uint16_t)ultra_get16(d + ULTRA_PXL_WINDOWS);
    o->answers = (uint16_t)ultra_get16(d + ULTRA_PXL_ANSWERS);
    o->missed = (uint16_t)ultra_get16(d + ULTRA_PXL_MISSED);
    o->bits = ultra_get24(d + ULTRA_PXL_BITS);
    o->markers = (uint16_t)ultra_get16(d + ULTRA_PXL_MARKERS);
    o->busy = (uint16_t)ultra_get16(d + ULTRA_PXL_BUSY);
    return ULTRA_OK;
}

#ifdef __cplusplus
}
#endif

#endif
