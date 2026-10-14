/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender, optional module: the return channel's POLL frames and answers (ultra_sender_rc.h).
 */
#include "ultra_sender_rc.h"
#include "ultra_sender_internal.h"

void ultra_rc_request_init(ultra_rc_request_t *r, uint8_t rate)
{
    r->rate = rate;
    r->count = 0;
}

int ultra_rc_request_add(ultra_rc_request_t *r, uint16_t board, uint8_t page, const uint8_t *args, uint32_t nargs)
{
    ultra_rc_ask_t *a;
    uint32_t i;
    if (r->count >= ULTRA_RW_MAX_SLOTS || nargs > sizeof a->args || (args == 0 && nargs != 0u) ||
        (r->count != 0u && board <= r->ask[r->count - 1u].board)) {
        return ULTRA_ERR_ARG;                    /* full, or not in rising chain order */
    }
    a = &r->ask[r->count++];
    a->board = board;
    a->page = page;
    a->nargs = (uint8_t)nargs;
    for (i = 0; i < nargs; i++) {
        a->args[i] = args[i];
    }
    return ULTRA_OK;
}

uint32_t ultra_rc_slot_start_us(uint8_t rate, uint8_t index)
{
    return ULTRA_RW_FIRST_US + ultra_rw_slot_us(rate) * index;
}

/* the headers the chain needs after its preamble before the first RW frame: ULTRA_RW_LOCK_MS of them at the refresh
 * rate at least (time alone isn't enough: the boards measure their clock rate on headers) */
static uint32_t lock_frames(const ultra_sender_t *l)
{
    const uint32_t n = l->refresh_us != 0u ? (ULTRA_RW_LOCK_MS * 1000u + l->refresh_us - 1u) / l->refresh_us : 1u;
    return n != 0u ? n : 1u;
}

uint32_t ultra_sender_rc_frame(ultra_sender_t *l, const ultra_rc_request_t *r, int64_t now_us, uint8_t *buf)
{
    uint32_t i, k = 0, last;
    if (l->n == 0u || r->count == 0u || r->count > ULTRA_RW_MAX_SLOTS || r->rate >= ULTRA_RW_RATES ||
        r->ask[r->count - 1u].board >= l->n || l->switch_frame < ULTRA_SWITCH_WS_FRAMES || l->preamble_left != 0u ||
        l->ready_us == INT64_MIN || now_us < l->ready_us + (int64_t)ULTRA_RW_LOCK_MS * 1000 || now_us < l->hold_us ||
        l->ready_frames < lock_frames(l)) {
        return 0;
    }
    last = r->ask[r->count - 1u].board;
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        uint8_t *s = buf + ULTRA_SLOT_BYTES * i;
        b->f_start = ULTRA_NO_START;
        b->f_floor = 0;
        if (i > last) {
            ultra_slot_nop(s);                   /* behind the last board asked: any slot (it doesn't take part) */
        } else if (k < r->count && r->ask[k].board == i) {
            const ultra_rc_ask_t *a = &r->ask[k];
            ultra_slot_poll(s, ++b->poll_seq, (uint8_t)k, r->count, a->page, r->rate, a->args, a->nargs);
            k++;
        } else {
            ultra_slot_poll(s, ++b->poll_seq, ULTRA_POLL_RELAY_ONLY, r->count, ULTRA_PAGE_STATUS, r->rate, 0, 0);
        }
    }
    l->has_sent = 1;
    l->last_start_us = now_us;
    l->last_ends_preamble = 0;
    l->frames++;
    if (l->ready_frames < 0xFFFFu) {
        l->ready_frames++;
    }
    l->hdr_flags = ULTRA_HDR_RW;
    l->kind = ULTRA_KIND_ULTRA;
    l->last_bytes = (uint16_t)ultra_frame_bytes(l->n);
    ultra_sender_finish_(l, now_us, buf);
    l->hold_us = ultra_sender_rw_hold_(l, buf);
    return l->last_bytes;
}

int ultra_rc_answer(const uint8_t *rx, const uint8_t *ferr, uint32_t n, uint8_t answer[32])
{
    uint32_t i, j;
    for (i = 0; i < n; i++) {
        if (rx[i] == ULTRA_RSP_SYNC && (ferr == 0 || !ferr[i])) {
            break;                               /* the first SYNC without a framing error: the answer */
        }
    }
    if (i + 32u > n) {
        return -1;
    }
    for (j = 0; j < 32u; j++) {
        if (ferr != 0 && ferr[i + j]) {
            return -1;
        }
        answer[j] = rx[i + j];
    }
    return ultra_answer_check(answer) == 0 ? 0 : -1;
}
