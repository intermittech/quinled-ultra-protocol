/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender, optional module: the return channel (protocol 0.2.4, the pages of 0.2.5; wiki: Return
 * channel). Only QuinLED Diff-Power ultra receivers answer (firmware 0.6.8 and later, BOARD=2; the LIFETIME and DIAG
 * pages from 0.6.9); a dig2analog-ultra never does, and a host that never polls needs none of this. Preview: the
 * frame rules are tested against the protocol's vectors, not yet against a receiver.
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

/* The RW frame for request r into buf, its first bit at now_us; returns its length, or 0 when the line can't poll
 * now (preamble, switch sequence, < ULTRA_RW_LOCK_MS after the preamble, an empty or invalid request). Send it like
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
 * Two shortcuts: an IDENTITY answer's features (ULTRA_FEAT_*: what this board takes, e.g. ULTRA_FEAT_RETRY), and a
 * STATUS answer's reason (0.2.5): why its light isn't as the host set it now, ULTRA_ST_WHY_* (0 = as set; a board from
 * before 0.2.5 sends 0) */
static inline uint32_t ultra_rc_features(const uint8_t *a)
{
    return ultra_get32(a + ULTRA_RSP_DATA + ULTRA_ID_FEATURES);
}
static inline uint8_t ultra_rc_status_why(const uint8_t *a)
{
    return (uint8_t)(a[ULTRA_RSP_DATA + ULTRA_ST_STATE] >> 4);   /* ULTRA_ST_WHY_SHIFT */
}

#ifdef __cplusplus
}
#endif

#endif
