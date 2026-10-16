/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender, optional module: the return channel (protocol 0.2.4, the pages of 0.2.5 and 0.2.6; wiki:
 * Return channel). Only QuinLED Diff-Power Ultra receivers answer (firmware 0.6.8 and later, BOARD=2; the LIFETIME and
 * DIAG pages from 0.6.9, LIFE_SYNC from 0.6.10); a Dig2Analog-Ultra never does, and a host that never polls needs none
 * of this. Preview: the frame rules are tested against the protocol's vectors, not yet against a receiver.
 *
 * An RW frame carries header FLAGS RW and a POLL in the slot of every board up to the last one asked: the boards
 * asked get answer index 0..R-1 (rising along the chain), the ones between get ULTRA_POLL_RELAY_ONLY (they turn around
 * and pass the answers on), the ones behind a NOP. After the frame the hub releases the line (within
 * ULTRA_RW_RELEASE_US) and receives each answer as UART bytes (8N1, LSB first, idle LOW; 1 Mbaud at rate 0) in its
 * response slot: t0 + ULTRA_RW_FIRST_US + i x ultra_rw_slot_us(rate), t0 = the frame's last falling edge (TIME). The
 * line takes no frame until t0 + W + ULTRA_RW_HOLDOFF_US (W = ultra_rw_window_us); ultra_sender_next_frame waits for
 * it. A missing answer is asked again in a later window (each POLL gets a new SEQ).
 *
 * Rules the builder keeps: no RW frame during the preamble or the WS switch sequence, nor before the chain has seen
 * headers for ULTRA_RW_LOCK_MS after the preamble (that long, and as many frames as the refresh sends in that time);
 * one R and one rate per frame; never with UPD or EXT.
 *
 * The transport: the hub releases the line within ULTRA_RW_RELEASE_US (150 us) of the RW frame's last falling edge,
 * so it sends that frame without the reset gap behind it and then switches its transceiver to receive
 * (ultra_esp32_rmt_send() appends 300 us of reset: not for RW frames). Each answer: ultra_rc_answer() checks SYNC,
 * framing and CRC only; the caller matches INDEX, SEQ (the POLL's, ultra_rc_seq), PAGE and UID16 to the window it
 * sent, starts each window's parsing afresh, and polls little enough that commands and the refresh still get frames.
 * Firmware updates over the line are not part of this library: the update sender is ESPHome's (quinled-ultra-esphome,
 * components/dig2analog_ultra: update_session.h, fw_update.cpp, ultra_update.h).
 */
#ifndef ULTRA_SENDER_RC_H
#define ULTRA_SENDER_RC_H

#include "ultra_sender.h"

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
 * arguments (POLL slot byte 7 on) as the header gives them per page (POWER, SUPPLY, CAL_TEST, ACKS; DIAG, 0.2.5: one
 * byte, the part: 0 the running board, 1 the fault memory and the calibration trend; LIFETIME none). A page the board
 * doesn't have is answered with page ULTRA_PAGE_UNSUPPORTED (a board from before 0.2.5: LIFETIME, DIAG). */
int  ultra_rc_request_add(ultra_rc_request_t *r, uint16_t board, uint8_t page, const uint8_t *args, uint32_t nargs);
/* (0.2.6) ask `board` for page LIFE_SYNC (ULTRA_FEAT_LIFE_SYNC): the hub's ledger of the board's lifetime counters for
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
/* the start of answer `index`'s response slot after the frame's TIME, us */
uint32_t ultra_rc_slot_start_us(uint8_t rate, uint8_t index);

/* One response slot's received bytes (rx, n; ferr: a framing error per byte, or NULL): every byte before the first
 * SYNC without a framing error is skipped (the turnaround checks' pulses arrive as junk bytes, then the 0xFF lead
 * byte), the 32 bytes from it are the answer. 0 = a valid answer copied to answer (SYNC and CRC-16 ok), -1 = none. */
int ultra_rc_answer(const uint8_t *rx, const uint8_t *ferr, uint32_t n, uint8_t answer[32]);
/* an answer's fields: page, index, RFLAGS (ULTRA_RSP_*), SEQ, UID16; DATA from answer + ULTRA_RSP_DATA (24 bytes,
 * offsets ULTRA_ST_* / ULTRA_PW_* / ... per page) */
static inline uint8_t  ultra_rc_page(const uint8_t *a) { return a[1]; }
static inline uint8_t  ultra_rc_index(const uint8_t *a) { return (uint8_t)(a[2] >> 4); }
static inline uint8_t  ultra_rc_rflags(const uint8_t *a) { return (uint8_t)(a[2] & 0x0Fu); }
static inline uint8_t  ultra_rc_seq(const uint8_t *a) { return a[3]; }
static inline uint16_t ultra_rc_uid16(const uint8_t *a) { return (uint16_t)ultra_get16(a + 4); }
/* DATA fields: ultra_get16 / ultra_get24 / ultra_get32 (big-endian) at answer + ULTRA_RSP_DATA + the page's offset.
 * Shortcuts: the features of an IDENTITY answer to part 1 (0.2.7: POLL argument 0 = ULTRA_ID_PART_FEATURES; part 0
 * is the board): ultra_rc_features its bits 0-31 as the ULTRA_FEAT_* masks (what this board takes, e.g.
 * ULTRA_FEAT_RETRY; ULTRA_FEAT_STANDBY, 0.2.6: it takes the power-off notice, ultra_sender_power_off),
 * ultra_rc_feature any bit 0-191; and a STATUS answer's reason (0.2.5): why its light isn't as the host set it now,
 * ULTRA_ST_WHY_* (0 = as set; a board from before 0.2.5 sends 0) */
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
/* (0.2.6) a LIFE_SYNC answer: DATA bytes 0..15 as LIFETIME's (after the merge), then the lifetime energy (Wh), the
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

#ifdef __cplusplus
}
#endif

#endif
