/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * One shared start against the boards' boot epochs: the C99 sender's frames into the board firmware's receive path
 * (header parse, ultra_on_header, ultra_apply_slot). Light 0 on and light 1 off share one start, sent in two frames
 * (the second 500 us after the first frame's end). Two boards get both: one synchronized since the host's boot, one that
 * first synchronizes on the first of the two frames, at a late host epoch (power-up hours into the hub's uptime). For
 * every launch phase (1000), chain length (1 / 8 / 20 / 34 / 50), slot position and line rate (800 / 600 / 200 kHz),
 * both items land on the same ms on both boards, and that ms is the same host instant within 500 us of the start.
 * Built by run_tests.py (step sim) against the firmware of the same protocol release.
 */
#include <stdio.h>
#include <string.h>
#include "ultra_sender.h"
#include "ultra_sender_internal.h"
#include "ultra.h"

#define NB 50u
static ultra_sender_t line;
static ultra_sender_board_t boards[NB];
static ultra_sender_item_t items[NB * 5u];
static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(NB)];
static ultra_t old_rx, fresh_rx, synced;

static uint32_t hdr_time(uint32_t n)
{
    uint32_t t = 0;
    return ultra_parse_header(frame + ULTRA_SLOT_BYTES * n, &t) == 0 ? t : 0xFFFFFFFFu;
}

int main(void)
{
    static const unsigned counts[] = { 1, 8, 20, 34, 50 }, profiles[] = { 0, 1, 3 }, rates[] = { 800, 600, 200 };
    static const int64_t epochs[] = { 5000000000LL, 10000000000LL, 4194304000LL * 3 - 700000LL };
    const uint16_t one[5] = { 65535, 65535, 65535, 65535, 65535 };
    const uint16_t lo[5] = { 0, 0, 0, 0, 0 };
    static const uint8_t r0[5] = { ULTRA_ROLE_BRIGHTNESS, 0, 0, 0, 0 }, r1[5] = { 0, ULTRA_ROLE_BRIGHTNESS, 0, 0, 0 };
    unsigned c, p, pos, e, phase, b, runs = 0, split = 0, cross = 0, bound = 0, bad_hdr = 0;
    uint8_t s[32];
    for (e = 0; e < sizeof epochs / sizeof epochs[0]; e++) {
        int64_t t;
        /* the board synchronized since the host's boot: a header every 10 minutes up to the epoch */
        ultra_init(&synced, 5);
        for (t = 1000; t < epochs[e] - 1000000; t += 600000000) {
            ultra_on_header(&synced, (uint32_t)(t / 50), ultra_time_wire((uint64_t)t));
        }
        ultra_on_header(&synced, (uint32_t)((epochs[e] - 1000000) / 50), ultra_time_wire((uint64_t)(epochs[e] - 1000000)));
        ultra_slot_light_def(s, 0, 1, r0, 1000, 0, lo, one, 0);
        ultra_on_slot(&synced, s, 0);
        ultra_slot_light_def(s, 1, 1, r1, 1000, 0, lo, one, 0);
        ultra_on_slot(&synced, s, 0);
        for (c = 0; c < 5u; ++c) for (p = 0; p < 3u; ++p) for (pos = 0; pos < 3u; ++pos) {
            const unsigned n = counts[c], slot = pos == 0 ? 0 : pos == 1 ? n / 2 : n - 1;
            unsigned m_split = 0, m_cross = 0;
            ultra_sender_config_t cfg;
            ultra_sender_config_init(&cfg, (uint16_t)n, 5);
            cfg.clear_unused = 0;
            if (ultra_timing_profile(&cfg.timing, (uint8_t)profiles[p], 9600000, 500) ||
                ultra_sender_init(&line, &cfg, boards, items, NB * 5u)) {
                printf("FAIL set-up n=%u rate=%u\n", n, rates[p]);
                return 2;
            }
            for (phase = 0; phase < 1000u; ++phase) {
                const int64_t now = epochs[e] + phase, start = now + 1000000 + (int64_t)((phase * 7919u) % 1000u);
                int64_t ta, tb, sa, ra[2], rb[2];
                uint32_t ha, hb, i;
                ultra_t *rx[2];
                for (b = 0; b < n; ++b) {
                    ultra_slot_nop(frame + 32 * b);
                    boards[b].f_start = ULTRA_NO_START;
                }
                boards[slot].f_start = start; boards[slot].f_floor = 0; boards[slot].f_dur = 0;
                ultra_slot_set(frame + 32 * slot, 0, 2, 0, 65535, one, one, 0, 0);
                ta = ultra_sender_finish_(&line, now, frame);
                ha = hdr_time(n);
                old_rx = synced;
                ultra_init(&fresh_rx, 9);
                rx[0] = &old_rx; rx[1] = &fresh_rx;
                ultra_on_header(&old_rx, (uint32_t)(ta / 50), ha);
                ultra_on_header(&fresh_rx, 1000u, ha);           /* the late boot: this is its first header */
                ultra_slot_light_def(s, 0, 1, r0, 1000, 0, lo, one, 0);
                ultra_on_slot(&fresh_rx, s, 1000u);
                ultra_slot_light_def(s, 1, 1, r1, 1000, 0, lo, one, 0);
                ultra_on_slot(&fresh_rx, s, 1000u);
                for (i = 0; i < 2u; i++) ultra_on_slot(rx[i], frame + 32 * slot, 0);
                ultra_slot_set(frame + 32 * slot, 1, 2, 0, 0, one, one, 0, 0);
                tb = ultra_sender_finish_(&line, ta + 500, frame);
                hb = hdr_time(n);
                ultra_on_header(&old_rx, (uint32_t)(tb / 50), hb);
                ultra_on_header(&fresh_rx, 1000u + (uint32_t)((tb - ta) / 50), hb);
                for (i = 0; i < 2u; i++) ultra_on_slot(rx[i], frame + 32 * slot, 0);
                bad_hdr += ha == 0xFFFFFFFFu || hb == 0xFFFFFFFFu;
                /* each board's ms as the host's time: its ms count was seeded from TIME (the host's minus whole wraps) */
                sa = 0;
                for (i = 0; i < 2u; i++) {
                    const int64_t seed = i ? ta : 1000, off = seed - (int64_t)ultra_time_wire((uint64_t)seed);
                    ra[i] = (int64_t)rx[i]->light[0].cur.t0 * 1000 + off;
                    rb[i] = (int64_t)rx[i]->light[1].cur.t0 * 1000 + off;
                    m_split += ra[i] != rb[i];
                    sa = ra[i] - start;
                    bound += sa < -500 || sa > 499;
                }
                m_cross += ra[0] != ra[1] || rb[0] != rb[1];
                runs++;
            }
            if (m_split || m_cross) {
                printf("epoch=%lld n=%u rate=%u slot=%u: split %u/1000, boards apart %u/1000\n", (long long)epochs[e], n,
                       rates[p], slot, m_split, m_cross);
            }
            split += m_split;
            cross += m_cross;
        }
    }
    printf("epoch: %u runs (3 epochs x 5 lengths x 3 rates x 3 slots x 1000 phases) x 2 boards: split %u, boards apart %u, "
           "start outside -500 .. +499 us %u, bad headers %u\n", runs, split, cross, bound, bad_hdr);
    if (split || cross || bound || bad_hdr) {
        printf("FAIL epoch: one shared start must land on one host ms on every board\n");
        return 1;
    }
    printf("PASS epoch\n");
    return 0;
}
