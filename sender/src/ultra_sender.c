/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: the scheduler and frame builder (ultra_sender.h). The rules follow the reference host, the
 * ESPHome component dig2analog_ultra (ultra_core.h, class Scheduler, and the hub's start delay / group start).
 */
#include <string.h>

#include "ultra_sender.h"
#include "ultra_sender_internal.h"

#define END             0xFFFFu
#define DAY_US          (86400LL * 1000000LL)
#define START_SLACK_US  10000
#define GLIDE_MAX_MS    1000u          /* longest glide of a streamed SET that waited */
#define REFRESH_RUN     15u            /* after this many queued commands in a row, a board's slot is a refresh */
#define POWER_ON_FRAMES 10u            /* the preamble after a supply switch-on, once a full one reached the boards */
#define HOLDOFF_US      200            /* a frame's gap after its reset, in the start leads (ESPHome: + 200) */
#define GROUP_MAX       1024u          /* lines walked in a group at most (a damaged list never hangs) */
#define STAMP_PASSES    8u             /* TIME / start offset passes at most (finish) */
#define BIT32(n)        ((uint32_t)1 << (n))   /* (never 1u << n: 16-bit int targets) */
/* A frame's wire time in ns fits 32 bits (solve_header, finish: no 64-bit maths): 1024 boards at the slowest legal
 * '1' (12 us) is (32 x 1024 + 8) x 8 x 12000 = 3.15e9 ns */
#if ULTRA_SENDER_MAX_BOARDS > 1024
#error "ULTRA_SENDER_MAX_BOARDS: at most 1024 (a frame's time in ns must fit 32 bits)"
#endif

/* the kinds of state of a light, in refresh order */
enum { RK_DEF = 0, RK_OPT, RK_SET, RK_EFFECT, RK_SCENE, RK_SCENE_FX, RK_STEP, RK_PIXFX, RK_HWCFG };
#define KEY(light, rank, sub) ((uint16_t)(((uint32_t)(light) << 8) | ((uint32_t)(rank) << 4) | (uint32_t)(sub)))
#define KEY_LIGHT(k)          ((uint8_t)((k) >> 8))
#define KEY_RANK(k)           ((uint8_t)(((k) >> 4) & 15u))
/* an item is part of the board's state while its light is in use (the board's own items, light ULTRA_LIGHT_BOARD: the
 * hardware-specific CONFIG pages of an Ultra-Digital receiver, always) */
#define ITEM_ON(b, key)       (KEY_LIGHT(key) == ULTRA_LIGHT_BOARD || KEY_LIGHT(key) < (b)->lights)

#define IT_VALID  0x01u                /* the item has content to send */
#define IT_STREAM 0x02u                /* a streamed SET */
#define IT_HWDIRTY 0x04u               /* a pixel CONFIG page changed: its SEQ and queueing wait for flush_config */

#define KIND_NONE  ULTRA_KIND_NONE
#define KIND_ULTRA ULTRA_KIND_ULTRA
#define KIND_WS    ULTRA_KIND_WS

/* urgent candidates (take_urgent) */
enum { C_NONE = 0, C_CFG, C_ID, C_CLR, C_ITEM };

/* --------------------------------------------------------------------------------------------- small helpers */
static void copy32(uint8_t *d, const uint8_t *s)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        d[i] = s[i];
    }
}

static int same32(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;
    for (i = 0; i < ULTRA_SLOT_BYTES; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* an item's start (int64_t, kept in two 32-bit halves) */
static int64_t it_start(const ultra_sender_item_t *it)
{
    return (int64_t)(((uint64_t)it->start_hi << 32) | it->start_lo);
}

static void it_set_start(ultra_sender_item_t *it, int64_t t)
{
    it->start_lo = (uint32_t)(uint64_t)t;
    it->start_hi = (uint32_t)((uint64_t)t >> 32);
}

static uint8_t rnd8(ultra_sender_t *l)
{
    l->rng = l->rng * 1664525u + 1013904223u;
    return (uint8_t)(l->rng >> 24);
}

static int board_ok(const ultra_sender_t *l, uint16_t board)
{
    return board < l->n;
}

static int light_ok(const ultra_sender_t *l, uint16_t board, uint8_t light)
{
    return board < l->n && light < l->boards[board].lights;
}

static uint32_t clear_slots(const ultra_sender_t *l, const ultra_sender_board_t *b)
{
    return l->clear_unused ? (ULTRA_LIGHTS - b->lights) * 3u : 0u;
}

static uint32_t cfg_count(const ultra_sender_board_t *b)
{
    uint32_t i, n = 0, w;
    for (i = 0; i < ULTRA_SENDER_CFG_WORDS; i++) {
        for (w = b->cfg_have[i]; w != 0u; w &= w - 1u) {
            n++;
        }
    }
    return n;
}

static int cfg_has(const ultra_sender_board_t *b, uint32_t id)
{
    return (int)((b->cfg_have[id >> 5] >> (id & 31u)) & 1u);
}

static uint32_t cfg_pages(const ultra_sender_board_t *b)
{
    return (cfg_count(b) + 8u) / 9u;
}

/* ------------------------------------------------------------------------------------------------- queue */
/* A newer command replaces a queued one of the same key and keeps its place while none of its copies went out (so
 * lights that change every frame take turns). copies 0 = the line's repeats. */
static void q_put(ultra_sender_t *l, ultra_sender_board_t *b, ultra_sender_q_t *q, uint8_t copies, int counted)
{
    uint32_t order = l->order++;
    q->behind = 0;
    if (q->copies != 0u) {
        if (q->sends == 0u) {
            q->behind = 1;
            order = q->order;
            if (counted) {
                b->unsent--;
            }
        }
        l->queued--;
    }
    q->order = order;
    q->sends = 0;
    q->copies = copies != 0u ? copies : l->repeats;
    l->queued++;
    if (counted) {
        b->unsent++;
    }
}

static void q_drop(ultra_sender_t *l, ultra_sender_board_t *b, ultra_sender_q_t *q, int counted)
{
    if (q->copies == 0u) {
        return;
    }
    if (counted && q->sends == 0u) {
        b->unsent--;
    }
    q->copies = 0;
    l->queued--;
}

static void q_sent(ultra_sender_t *l, ultra_sender_board_t *b, ultra_sender_q_t *q, int counted)
{
    if (counted && q->sends == 0u) {
        b->unsent--;
    }
    if (++q->sends >= q->copies) {
        q->copies = 0;
        l->queued--;
    }
}

/* ------------------------------------------------------------------------------------------------- items */
static ultra_sender_item_t *item_find(ultra_sender_t *l, const ultra_sender_board_t *b, uint16_t key)
{
    uint16_t i;
    for (i = b->first; i != END; i = l->items[i].next) {
        if (l->items[i].key == key) {
            return &l->items[i];
        }
        if (l->items[i].key > key) {
            break;
        }
    }
    return 0;
}

/* the item of key, allocated (sorted into the board's list) if needed; NULL: the pool is full */
static ultra_sender_item_t *item_get(ultra_sender_t *l, ultra_sender_board_t *b, uint16_t key)
{
    ultra_sender_item_t *it = item_find(l, b, key);
    uint16_t idx, *link;
    if (it != 0) {
        return it;
    }
    if (l->used >= l->n_items) {
        return 0;
    }
    idx = l->used++;
    it = &l->items[idx];
    memset(it, 0, sizeof *it);                   /* (not queued, no flags, an empty slot) */
    it_set_start(it, ULTRA_NO_START);
    it->key = key;
    it->slot[2] = rnd8(l);                       /* the next command takes this + 1 */
    link = &b->first;
    while (*link != END && l->items[*link].key < key) {
        link = &l->items[*link].next;
    }
    it->next = *link;
    *link = idx;
    return it;
}

static uint32_t items_free(const ultra_sender_t *l)
{
    return (uint32_t)(l->n_items - l->used);
}

/* the SEQ of the item's next command; on a light that was cleared, light definitions, scenes and power-on effects
 * never take the SEQ of the clearing slots (the board takes its first command) */
static uint8_t next_seq(const ultra_sender_board_t *b, const ultra_sender_item_t *it)
{
    uint8_t s = (uint8_t)(it->slot[2] + 1u);
    const uint8_t rk = KEY_RANK(it->key);
    if ((rk == RK_DEF || rk == RK_SCENE || rk == RK_SCENE_FX) && s == b->clr_seq &&
        ((b->clr_mask >> KEY_LIGHT(it->key)) & 1u)) {
        s++;
    }
    return s;
}

/* a light's steps (sub 0..ULTRA_MAX_STEPS-1) from `from` on: not sent any more */
static void drop_steps(ultra_sender_t *l, ultra_sender_board_t *b, uint8_t light, uint32_t from)
{
    uint32_t i;
    for (i = from; i < ULTRA_MAX_STEPS; i++) {
        ultra_sender_item_t *st = item_find(l, b, KEY(light, RK_STEP, i));
        if (st != 0) {
            q_drop(l, b, &st->q, 1);
            st->q.flags = 0;
        }
    }
}

/* a static item (LIGHT_DEF, LIGHT_OPT, SCENE, SCENE_EFFECT): s was built with the item's current SEQ; unchanged
 * content queues nothing, else the next SEQ and 3 copies */
static void put_static(ultra_sender_t *l, ultra_sender_board_t *b, ultra_sender_item_t *it, uint8_t *s)
{
    if ((it->q.flags & IT_VALID) && same32(s, it->slot)) {
        return;
    }
    s[2] = next_seq(b, it);
    ultra_slot_seal(s);
    copy32(it->slot, s);
    it->q.flags = IT_VALID;
    it_set_start(it, ULTRA_NO_START);
    q_put(l, b, &it->q, 0, 1);
}

/* ------------------------------------------------------------------------------------------------- slots built on
 * the fly: CONFIG pages (every id set, 9 per slot, in id order), IDENTIFY, and the clearing slots of unused lights */
static void build_config(const ultra_sender_board_t *b, uint32_t page, uint8_t *s)
{
    uint8_t ids[9];
    uint16_t vals[9];
    uint32_t id, k = 0, n = 0;
    for (id = 1; id < ULTRA_CFG_COUNT && n < 9u; id++) {
        if (cfg_has(b, id)) {
            if (k >= 9u * page) {
                ids[n] = (uint8_t)id;
                vals[n++] = b->cfg[id];
            }
            k++;
        }
    }
    ultra_slot_config(s, b->cfg_pseq[page], ids, vals, n);
}

static void build_identify(const ultra_sender_board_t *b, uint8_t *s)
{
    if (b->id_val == ULTRA_IDENT_CAL) {
        ultra_slot_calibrate(s, b->id_seq, b->id_arg);
    } else if (b->id_val == ULTRA_IDENT_DARK) {
        ultra_slot_hold_dark(s, b->id_seq, b->id_arg);
    } else if (b->id_val == ULTRA_IDENT_POWER_OFF) {
        ultra_slot_power_off(s, b->id_seq);       /* (0.2.6: ultra_sender_power_off) */
    } else {
        ultra_slot_identify(s, b->id_seq, b->id_val);
    }
}

/* clearing slot j of the board's unused lights: an empty LIGHT_DEF, SCENE mode 0, SCENE_EFFECT none */
static void build_clear(const ultra_sender_board_t *b, uint32_t j, uint8_t *s)
{
    static const uint16_t zero[5] = { 0, 0, 0, 0, 0 };
    static const uint8_t roles[5] = { 0, 0, 0, 0, 0 };
    const uint8_t light = (uint8_t)(b->lights + j / 3u);
    switch (j % 3u) {
    case 0:
        ultra_slot_light_def(s, light, b->clr_seq, roles, 1000, 0, zero, zero);
        break;
    case 1:
        ultra_slot_scene(s, light, b->clr_seq, 0, 0, 0, zero, zero);
        break;
    default:
        ultra_slot_scene_effect(s, light, b->clr_seq, ULTRA_FX_NONE, 0, 0);
        break;
    }
}

/* (re)queue the clearing slots: one round per copy */
static void clear_put(ultra_sender_t *l, ultra_sender_board_t *b)
{
    b->clr_round = 0;                            /* (the refresh: 3 complete rounds with them from here) */
    b->rr = 0;
    if (clear_slots(l, b) == 0u) {
        q_drop(l, b, &b->clr_q, 0);
        return;
    }
    b->clr_mask |= (uint8_t)(0x1Fu & ~(BIT32(b->lights) - 1u));
    b->clr_pos = 0;
    q_put(l, b, &b->clr_q, 0, 0);
}

/* ------------------------------------------------------------------------------------------------- start times */
/* A periodic effect's start, moved forward by whole periods once it is more than a day old: the offset to a frame
 * stays far inside the EFFECT slot's int32 ms, so a board that comes back after weeks still joins in phase. An effect
 * without a period has no phase to keep: a day back (a board takes a start 2^31 ms back as one still to come). */
static int64_t recent_start(const ultra_sender_item_t *it, int64_t now_us)
{
    uint64_t k;
    if (it_start(it) == ULTRA_NO_START || now_us - it_start(it) < DAY_US || it->slot[0] != ULTRA_EFFECT) {
        return it_start(it);
    }
    if (it->aux == 0u) {
        return now_us - DAY_US;
    }
    /* whole periods since the start: (now - start) / (aux x 1000) = whole ms / aux (a floor of a floor) */
    k = ultra_udiv64_(ultra_udiv64_((uint64_t)(now_us - it_start(it)), 1000u), it->aux);
    return it_start(it) + (int64_t)(k * it->aux * 1000u);
}

/* the latest start <= start_us that is a whole number of periods after the clock's zero */
static int64_t phase_start(int64_t start_us, uint32_t period_ms)
{
    if (period_ms == 0u || start_us < 0) {
        return start_us;
    }
    return (int64_t)(ultra_udiv64_(ultra_udiv64_((uint64_t)start_us, 1000u), period_ms) * period_ms * 1000u);
}

/* how far ahead a command starts so it is on the wire first: the frame that may be on the wire, a frame per command
 * for the busiest board (`commands` new, `waiting` queued before them), every 16th slot a refresh, + slack */
static int64_t lead_for(const ultra_sender_t *l, uint32_t commands, uint32_t waiting)
{
    const int64_t frame = (int64_t)l->frame_us + 500;
    const uint32_t n = commands + waiting;
    return frame * (int64_t)(1u + n + (n + 14u) / 15u) + START_SLACK_US;
}

static uint32_t waiting_max(const ultra_sender_t *l)
{
    uint32_t i, w = 0;
    for (i = 0; i < l->n; i++) {
        if (l->boards[i].unsent > w) {
            w = l->boards[i].unsent;
        }
    }
    return w;
}

/* the line's own frames before a new command's first copy: the WS switch frames and the preamble still to send */
static int64_t startup_us(const ultra_sender_t *l)
{
    int64_t t = (int64_t)l->preamble_left * ((int64_t)l->frame_us + HOLDOFF_US);
    if (l->switch_frame < ULTRA_SWITCH_WS_FRAMES) {
        t += (int64_t)(ULTRA_SWITCH_WS_FRAMES - l->switch_frame) * ((int64_t)l->switch_us + HOLDOFF_US);
    }
    return t;
}

static int64_t command_lead(const ultra_sender_t *l)
{
    return startup_us(l) + (l->start_delay_us >= 0 ? (int64_t)l->start_delay_us
                                                   : lead_for(l, l->group_commands, waiting_max(l)));
}

static int64_t stream_lead(const ultra_sender_t *l)
{
    return startup_us(l) + (l->start_delay_us >= 0 ? (int64_t)l->start_delay_us : lead_for(l, l->max_lights, 0));
}

int64_t ultra_sender_start_time(ultra_sender_t *l, uint16_t board, int64_t now_us)
{
    ultra_sender_group_t *g = l->group;
    const ultra_sender_t *x;
    int64_t lead = command_lead(l), reach;
    uint32_t k;
    for (x = g->lines, k = 0; x != 0 && k < GROUP_MAX; x = x->group_next, k++) {
        const int64_t v = command_lead(x);
        lead = v > lead ? v : lead;
    }
    /* a command joins the open group while its first copy gets out by then: the line's own startup frames, the frame
     * on the wire, what waits on its board, its own frame */
    reach = startup_us(l) + lead_for(l, 1, board < l->n ? l->boards[board].unsent : 0u) - START_SLACK_US;
    if (g->start == ULTRA_NO_START || now_us + reach > g->start) {
        g->start = now_us + lead;
    }
    return g->start;
}

void ultra_sender_group_init(ultra_sender_group_t *g)
{
    g->start = ULTRA_NO_START;
    g->lines = 0;
}

/* every link to l out of g's list (also a stale one: the line was initialized again without leaving g) */
static void group_unlink(ultra_sender_group_t *g, ultra_sender_t *l)
{
    ultra_sender_t **p = &g->lines;
    uint32_t k;
    for (k = 0; *p != 0 && k < GROUP_MAX; k++) {
        if (*p == l) {
            *p = l->group_next != l ? l->group_next : 0;
        } else {
            p = &(*p)->group_next;
        }
    }
}

void ultra_sender_join_group(ultra_sender_t *l, ultra_sender_group_t *g)
{
    group_unlink(l->group, l);
    if (g != l->group) {
        group_unlink(g, l);
    }
    l->group = g;
    l->group_next = g->lines;
    g->lines = l;
}

void ultra_sender_leave_group(ultra_sender_t *l)
{
    l->own_group.start = ULTRA_NO_START;
    ultra_sender_join_group(l, &l->own_group);
}

void ultra_sender_deinit(ultra_sender_t *l)
{
    ultra_sender_leave_group(l);
    l->n = 0;                                    /* every board / light command: ULTRA_ERR_ARG; no frames */
    l->n_items = l->used = 0;
    l->queued = 0;
    l->preamble_left = 0;
    l->switch_frame = ULTRA_SWITCH_WS_FRAMES;
}

/* the most lights on a board; the automatic command budget follows it */
static void lights_changed(ultra_sender_t *l)
{
    uint32_t i;
    uint8_t m = 1;
    for (i = 0; i < l->n; i++) {
        m = l->boards[i].lights > m ? l->boards[i].lights : m;
    }
    l->max_lights = m;
    if (l->gc_auto) {
        l->group_commands = m;
    }
}

int ultra_sender_set_group_commands(ultra_sender_t *l, uint8_t commands)
{
    if (l->n == 0u) {
        return ULTRA_ERR_ARG;
    }
    l->gc_auto = commands == 0u;
    if (!l->gc_auto) {
        l->group_commands = commands;
    }
    lights_changed(l);
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------------------------- set-up */
void ultra_sender_config_init(ultra_sender_config_t *c, uint16_t boards, uint8_t lights)
{
    static const ultra_timing_t rec = ULTRA_TIMING_RECOMMENDED;
    c->boards = boards;
    c->lights = lights;
    c->repeats = 3;
    c->refresh_ms = 200;
    c->start_delay_us = -1;
    c->group_commands = 0;
    c->clear_unused = 1;
    c->seed = 0x51D2A5u;
    c->timing = rec;
}

int ultra_sender_config_check(const ultra_sender_config_t *c)
{
    return (c->boards == 0u || c->boards > ULTRA_SENDER_MAX_BOARDS || c->lights > ULTRA_LIGHTS ||
            ultra_timing_check(&c->timing) != 0) ? ULTRA_ERR_ARG : ULTRA_OK;
}

int ultra_sender_init(ultra_sender_t *l, const ultra_sender_config_t *c, ultra_sender_board_t *boards,
                      ultra_sender_item_t *items, uint16_t n_items)
{
    uint32_t i;
    if (ultra_sender_config_check(c) != ULTRA_OK || boards == 0 || (items == 0 && n_items != 0u)) {
        return ULTRA_ERR_ARG;
    }
    memset(l, 0, sizeof *l);                     /* (every field 0 but these: counters, flags, queues, times) */
    l->boards = boards;
    l->items = items;
    l->n = c->boards;
    l->n_items = n_items;
    l->timing = c->timing;
    l->refresh_us = (uint32_t)c->refresh_ms * 1000u;
    l->start_delay_us = c->start_delay_us < 0 ? -1 : c->start_delay_us;
    l->repeats = c->repeats == 0u ? 1u : c->repeats;
    l->gc_auto = c->group_commands == 0u;
    l->group_commands = c->group_commands;
    l->max_lights = 1;
    l->clear_unused = c->clear_unused != 0u;
    l->rng = c->seed ^ 0x9E3779B9u;
    l->frame_us = ultra_frame_max_us(&l->timing, ultra_frame_bytes(l->n));
    l->switch_us = ultra_frame_max_us(&l->timing, ultra_switch_frame_bytes(l->n));
    l->hold_us = l->ready_us = l->off_cut_us = INT64_MIN;
    l->own_group.start = ULTRA_NO_START;
    l->own_group.lines = l;
    l->group = &l->own_group;
    l->preamble_left = (uint16_t)ultra_preamble_frames(l->n);
    l->preamble_full = 1;
    l->powered = 1;
    l->switch_frame = ULTRA_SWITCH_WS_FRAMES;    /* (kind: KIND_NONE, 0) */
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &boards[i];
        memset(b, 0, sizeof *b);
        b->f_start = ULTRA_NO_START;
        b->first = END;
        b->cfg_seq = rnd8(l);                    /* (the first SEQs: in this order, as before) */
        b->id_seq = rnd8(l);
        b->lights = c->lights;
        b->clr_seq = rnd8(l);
        b->poll_seq = rnd8(l);
        clear_put(l, b);
    }
    lights_changed(l);
    return ULTRA_OK;
}

int ultra_sender_set_lights(ultra_sender_t *l, uint16_t board, uint8_t lights)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint16_t i;
    uint8_t old, tries;
    if (!board_ok(l, board) || lights > ULTRA_LIGHTS) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    old = b->lights;
    if (lights == old) {
        return ULTRA_OK;
    }
    b->lights = lights;
    for (i = b->first; i != END; i = it->next) {
        it = &l->items[i];
        if (KEY_LIGHT(it->key) == ULTRA_LIGHT_BOARD) {
            continue;                              /* (the board's own items stay) */
        }
        if (KEY_LIGHT(it->key) >= lights) {
            q_drop(l, b, &it->q, 1);               /* not sent any more */
        } else if (KEY_LIGHT(it->key) >= old && (it->q.flags & IT_VALID)) {
            q_put(l, b, &it->q, 0, 1);             /* back: its state again */
        }
    }
    /* the clearing slots: a SEQ none of the newly cleared lights' items has (their last commands) */
    for (tries = 0; tries < 16u; tries++) {
        int clash = 0;
        for (i = b->first; i != END; i = it->next) {
            it = &l->items[i];
            if (KEY_LIGHT(it->key) >= lights && KEY_LIGHT(it->key) < old && it->slot[2] == b->clr_seq &&
                KEY_LIGHT(it->key) != ULTRA_LIGHT_BOARD) {
                clash = 1;
            }
        }
        if (!clash) {
            break;
        }
        b->clr_seq++;
    }
    lights_changed(l);
    clear_put(l, b);
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------------------------- frames */
/* Start offset of SET (int16 ms) / EFFECT (int32 ms) relative to this frame's TIME, then the CRC. floor: not before
 * TIME (a streamed SET starts when it arrives, at the latest). A SET that started more than 32.8 s ago (a long
 * transition refreshed to a board that missed it) starts at that limit with its duration shortened by the rest: it
 * still ends on time. */
static uint32_t ones_in(const uint8_t *d, uint32_t n)
{
    static const uint8_t t[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
    uint32_t i, k = 0;
    for (i = 0; i < n; i++) {
        k += (uint32_t)t[d[i] & 15u] + t[d[i] >> 4];
    }
    return k;
}

/* the 1 bits of a slot's bytes that stamp and its seal can change: an EFFECT's offset (4..7), a SET's duration,
 * offset (25..29) and the CRC (30, 31) */
static uint32_t stamp_ones(const uint8_t *s)
{
    return ones_in(s + 4, 4) + ones_in(s + 25, 7);
}

/* Returns the change of the slot's 1 bits (finish_ keeps the frame's count up to date). force: seal, and count
 * nothing (the first pass: a copied slot's CRC may be stale, e.g. a stream's patched glide; the caller counts the
 * whole frame). Later passes leave a SET whose bytes don't change as it is: its CRC holds. */
static int32_t stamp(uint8_t *s, int64_t start_us, int64_t hdr_us, int floor, uint32_t duration, int force)
{
    int64_t d, ms;
    uint32_t before = 0;
    if (start_us == ULTRA_NO_START) {
        return 0;
    }
    d = (floor && start_us < hdr_us) ? 0 : start_us - hdr_us;
    ms = d >= 0 ? (int64_t)ultra_udiv64_((uint64_t)d + 500u, 1000u)
                : -(int64_t)ultra_udiv64_((uint64_t)-d + 499u, 1000u);
    if (s[0] == ULTRA_SET) {
        if (ms < -32768) {
            const uint64_t lost = (uint64_t)(-32768 - ms);
            duration = lost >= duration ? 0u : duration - (uint32_t)lost;
        }
        ms = ms < -32768 ? -32768 : ms > 32767 ? 32767 : ms;
        if (!force && ultra_get24(s + ULTRA_PAYLOAD + 22) == duration &&
            ultra_get16(s + ULTRA_PAYLOAD + 25) == (uint16_t)(int16_t)ms) {
            return 0;
        }
        before = force ? 0u : stamp_ones(s);
        ultra_put24(s + ULTRA_PAYLOAD + 22, duration);
        ultra_put16(s + ULTRA_PAYLOAD + 25, (uint32_t)(uint16_t)(int16_t)ms);
    } else {
        if (!force && s[0] != ULTRA_EFFECT && s[0] != ULTRA_PIXEL_FX) {
            return 0;                            /* (no offset: sealed in the first pass) */
        }
        before = force ? 0u : stamp_ones(s);
        if (s[0] == ULTRA_EFFECT || s[0] == ULTRA_PIXEL_FX) {   /* (START is at slot byte 4 in both) */
            ms = ms < INT32_MIN ? INT32_MIN : ms > INT32_MAX ? INT32_MAX : ms;
            ultra_put32(s + ULTRA_PAYLOAD + 1, (uint32_t)(int32_t)ms);
        }
    }
    ultra_slot_seal(s);
    return force ? 0 : (int32_t)stamp_ones(s) - (int32_t)before;
}

/* the slots' start offsets against TIME t */
static int32_t stamp_slots(ultra_sender_t *l, int64_t t, uint8_t *out, int force)
{
    uint32_t i;
    int32_t delta = 0;
    for (i = 0; i < l->n; i++) {
        const ultra_sender_board_t *b = &l->boards[i];
        delta += stamp(out + ULTRA_SLOT_BYTES * i, b->f_start, t, b->f_floor, b->f_dur, force);
    }
    return delta;
}

/* The header for the slots as they are: the TIME at which a frame carrying that header ends. Its TIME and CRC bits
 * set the frame's length too, so the candidates in the window those 40 bits can span are tried (a few at the
 * recommended timing, ~450 where '0' and '1' differ most), and the one whose frame measures exactly its own TIME is
 * taken, the earliest if several do (none: the one measuring closest, nearest `hint`). The header is written. The
 * search runs in us after start_us (32-bit: a frame lasts < 2^32 ns; hint is one of finish's TIMEs, >= start_us). */
static int64_t solve_header(ultra_sender_t *l, int64_t start_us, uint8_t *out, int64_t hint, uint32_t ones)
{
    const ultra_timing_t *tm = &l->timing;
    const uint32_t lead = ULTRA_SLOT_BYTES * l->n + 3u;          /* the slots and the header's fixed bytes */
    const uint32_t p0 = (uint32_t)tm->t0h_ns + tm->t0l_ns, p1 = (uint32_t)tm->t1h_ns + tm->t1l_ns;
    const uint32_t pmin = p0 < p1 ? p0 : p1, pmax = p0 < p1 ? p1 : p0;
    const uint32_t lmin = tm->t0l_ns < tm->t1l_ns ? tm->t0l_ns : tm->t1l_ns;
    const uint32_t lmax = tm->t0l_ns < tm->t1l_ns ? tm->t1l_ns : tm->t0l_ns;
    const uint32_t s32 = (uint32_t)start_us, rh = (uint32_t)(hint - start_us);
    uint8_t *h = out + ULTRA_SLOT_BYTES * l->n;
    uint32_t fixed, r, hi, best = rh, best_err = UINT32_MAX, best_d = UINT32_MAX;   /* (ns, us: < 2^32) */
    fixed = ones * p1 + (lead * 8u - ones) * p0;
    r = (fixed + 40u * pmin - lmax) / 1000u - 1u;
    hi = (fixed + 40u * pmax - lmin + 999u) / 1000u + 1u;
    for (; r <= hi; r++) {
        uint32_t k, m, err, d;
        ultra_build_header_flags(h, s32 + r, l->hdr_flags);
        k = p0 == p1 ? 0u : ones_in(h + 3, 5);   /* (equal periods, e.g. SPI: the 1 bits don't change the time) */
        m = (fixed + k * p1 + (40u - k) * p0 - ((h[7] & 1u) ? tm->t1l_ns : tm->t0l_ns) + 500u) / 1000u;
        err = m > r ? m - r : r - m;
        d = r > rh ? r - rh : rh - r;
        if (err < best_err || (err == best_err && err != 0u && d < best_d)) {   /* (exact: the earliest) */
            best_err = err;
            best_d = d;
            best = r;
        }
    }
    ultra_build_header_flags(h, s32 + best, l->hdr_flags);
    return start_us + best;
}

/* the worst start a frame gives, us: its slots' offsets against the frame's real end e (SETs clipped at their int16
 * range aside: those start at the limit on purpose) */
static int64_t start_error(const ultra_sender_t *l, const uint8_t *out, int64_t e)
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

/* TIME = start + the frame's bits on the wire, and the start offsets are against TIME; both are bits of the frame.
 * Stamp the offsets against a TIME, solve the header for those slots (its TIME then is the frame's real end), again
 * against that TIME, until it holds (two passes at the recommended timing). Where the '0' and '1' periods differ much,
 * an offset's ms rounding can move the frame's end across its own rounding boundary and it never holds: then the pass
 * whose starts come out best against its real end is used. TIME is the frame's end (to 1 us where no TIME fits a
 * frame exactly: its own bits move the end); a start is to the nearest ms once it holds, else within 0.5 ms + what the
 * flip moves the end (the periods' difference x the bits it flips). The bounds (README, test_review.c timing_space:
 * 64 boards, 1000 frames, the legal box's corners and random timings): '0' and '1' periods within 150 ns: TIME 2 us,
 * starts 502 us; any legal timing: TIME 100 us, starts 2 ms (worst measured: 57 us, 1.46 ms). */
int64_t ultra_sender_finish_(ultra_sender_t *l, int64_t start_us, uint8_t *out)
{
    const uint32_t bytes = ultra_frame_bytes(l->n);
    const ultra_timing_t *tm = &l->timing;
    const uint32_t avg = ((uint32_t)tm->t0h_ns + tm->t0l_ns + tm->t1h_ns + tm->t1l_ns) / 2u;
    const int equal = (uint32_t)tm->t0h_ns + tm->t0l_ns == (uint32_t)tm->t1h_ns + tm->t1l_ns;
    int64_t t = start_us + (int64_t)(bytes * 8u * avg / 1000u), sol = t, best_t = t, best_err = INT64_MAX;
    uint32_t pass, ones = 0;
    for (pass = 0; pass < STAMP_PASSES; pass++) {
        int64_t err;
        if (pass == 0) {
            stamp_slots(l, t, out, 1);
            ultra_build_header_flags(out + ULTRA_SLOT_BYTES * l->n, 0, l->hdr_flags);   /* (the fixed bytes) */
            ones = equal ? 0u : ones_in(out, ULTRA_SLOT_BYTES * l->n + 3u);   /* (equal periods: not needed) */
        } else {
            ones = (uint32_t)((int32_t)ones + stamp_slots(l, t, out, 0));
        }
        sol = solve_header(l, start_us, out, t, ones);
        if (sol == t) {
            best_t = t;                          /* the offsets are against the TIME the frame measures */
            best_err = -1;
            break;
        }
        err = start_error(l, out, sol);
        if (err < best_err) {
            best_err = err;
            best_t = t;
        }
        t = sol;
    }
    if (best_err >= 0) {
        ones = (uint32_t)((int32_t)ones + stamp_slots(l, best_t, out, 0));   /* (never settled: the best pass) */
        sol = solve_header(l, start_us, out, best_t, ones);
    }
    l->last_time_us = sol;
    return sol;
}

/* The urgent command for a board: the fewest copies sent first, then the oldest; at a tie the clearing slots of
 * unused lights come after the real commands. A streamed SET that waited glides at least as long as it has been
 * since the light's last one went out. */
static int take_urgent(ultra_sender_t *l, ultra_sender_board_t *b, uint8_t *s, int64_t now_us)
{
    const ultra_sender_q_t *best = 0;
    ultra_sender_item_t *bit = 0, *it;
    uint32_t kind = C_NONE, page = 0, p;
    uint16_t i;
#define BETTER(q, clr) (best == 0 || (q)->sends < best->sends || \
        ((q)->sends == best->sends && ((clr) ? 0 : kind == C_CLR || (int32_t)((q)->order - best->order) < 0)))
    for (p = 0; p < ULTRA_SENDER_CFG_PAGES; p++) {
        if (b->cfg_q[p].copies != 0u && BETTER(&b->cfg_q[p], 0)) {
            best = &b->cfg_q[p];
            kind = C_CFG;
            page = p;
        }
    }
    if (b->id_q.copies != 0u && BETTER(&b->id_q, 0)) {
        best = &b->id_q;
        kind = C_ID;
    }
    for (i = b->first; i != END; i = it->next) {
        it = &l->items[i];
        if (it->q.copies != 0u && BETTER(&it->q, 0)) {
            best = &it->q;
            kind = C_ITEM;
            bit = it;
        }
    }
    if (b->clr_q.copies != 0u && BETTER(&b->clr_q, 1)) {
        best = &b->clr_q;
        kind = C_CLR;
    }
#undef BETTER
    switch (kind) {
    case C_CFG:
        build_config(b, page, s);
        q_sent(l, b, &b->cfg_q[page], 1);
        return 1;
    case C_ID:
        build_identify(b, s);
        q_sent(l, b, &b->id_q, 1);
        return 1;
    case C_CLR: {
        const uint32_t n = clear_slots(l, b);
        if (n == 0u) {
            q_drop(l, b, &b->clr_q, 0);
            return 0;
        }
        if (b->clr_pos >= n) {
            b->clr_pos = 0;
        }
        build_clear(b, b->clr_pos, s);
        if (++b->clr_pos >= n) {                 /* a round of every clearing slot is one copy */
            b->clr_pos = 0;
            q_sent(l, b, &b->clr_q, 0);
        }
        return 1;
    }
    case C_ITEM:
        copy32(s, bit->slot);
        b->f_start = recent_start(bit, now_us);
        if (bit->q.flags & IT_STREAM) {
            uint32_t glide = bit->aux;
            if (bit->q.behind && bit->last_us != 0u) {
                const uint32_t since = ((uint32_t)now_us - bit->last_us) / 1000u;
                glide = since > glide ? since : glide;
                glide = glide > GLIDE_MAX_MS ? GLIDE_MAX_MS : glide;
                ultra_put24(s + ULTRA_PAYLOAD + 22, glide);
            }
            bit->last_us = (uint32_t)now_us | 1u;   /* (0 = none yet) */
            b->f_floor = 1;
        }
        q_sent(l, b, &bit->q, 1);
        return 1;
    default:
        return 0;
    }
}

/* The refresh rotation (shared with ESPHome's scheduler, round 11): the clearing slots of a board's unused lights are
 * part of its first 3 refresh rounds after init, a resync or a change of its light count, then of every 4th round:
 * rounds 0, 1, 2, 6, 10, 14, ... (clr_round runs 0..6, then 3..6 again). A board with 1 light refreshes its SET
 * every ~1.2 s instead of ~3 s. */
#define CLR_ROUND(r) ((r) < 3u || (r) == 6u)

/* The next item of the board's refresh list: its CONFIG slots, the clearing slots of its unused lights (in the
 * rounds above), then its lights' items in order (LIGHT_DEF, LIGHT_OPT, SET, EFFECT, SCENE, SCENE_EFFECT, STEPs). A
 * round ends when the position wraps to the start. */
static int take_refresh(ultra_sender_t *l, ultra_sender_board_t *b, uint8_t *s, int64_t now_us)
{
    const uint32_t pages = cfg_pages(b);
    uint32_t items = 0, total, tries, clears;
    uint16_t i;
    for (i = b->first; i != END; i = l->items[i].next) {
        items++;
    }
    for (tries = 0; tries < pages + clear_slots(l, b) + items; tries++) {
        uint32_t pos = b->rr;
        clears = CLR_ROUND(b->clr_round) ? clear_slots(l, b) : 0u;
        total = pages + clears + items;
        b->rr = (uint16_t)(pos + 1u >= total ? 0u : pos + 1u);
        if (pos >= total) {
            pos = 0;
            b->rr = total > 1u ? 1u : 0u;
        }
        if (b->rr == 0u) {                       /* this round's last position: the next one starts */
            b->clr_round = (uint8_t)(b->clr_round >= 6u ? 3u : b->clr_round + 1u);
        }
        if (total == 0u) {                       /* (only clearing slots, outside their rounds) */
            continue;
        }
        if (pos < pages) {
            build_config(b, pos, s);
            return 1;
        }
        pos -= pages;
        if (pos < clears) {
            build_clear(b, pos, s);
            return 1;
        }
        pos -= clears;
        for (i = b->first; pos > 0u; pos--) {
            i = l->items[i].next;
        }
        {
            const ultra_sender_item_t *it = &l->items[i];
            if (!(it->q.flags & IT_VALID) || !ITEM_ON(b, it->key)) {
                continue;
            }
            copy32(s, it->slot);
            b->f_start = recent_start(it, now_us);
            b->f_floor = (it->q.flags & IT_STREAM) ? 1u : 0u;
            return 1;
        }
    }
    return 0;
}

/* changed CONFIG values: every CONFIG slot of the board again, each with the next SEQ */
static void flush_config(ultra_sender_t *l)
{
    uint32_t i, p;
    if (l->hw_dirty) {                           /* the pixel CONFIG pages (0.2.8): one SEQ and 3 copies per changed page */
        l->hw_dirty = 0;
        for (i = 0; i < l->n; i++) {
            ultra_sender_board_t *b = &l->boards[i];
            uint16_t k;
            for (k = b->first; k != END; k = l->items[k].next) {
                ultra_sender_item_t *it = &l->items[k];
                if (KEY_RANK(it->key) == RK_HWCFG && (it->q.flags & IT_HWDIRTY)) {
                    it->q.flags = IT_VALID;
                    it->slot[2] = ++b->cfg_seq;  /* (one SEQ count with the official pages) */
                    ultra_slot_seal(it->slot);
                    q_put(l, b, &it->q, 0, 1);
                }
            }
        }
    }
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        uint32_t pages;
        if (!b->cfg_dirty) {
            continue;
        }
        b->cfg_dirty = 0;
        pages = cfg_pages(b);
        for (p = 0; p < ULTRA_SENDER_CFG_PAGES; p++) {
            if (p < pages) {
                b->cfg_pseq[p] = ++b->cfg_seq;
                q_put(l, b, &b->cfg_q[p], 0, 1);
            } else {
                q_drop(l, b, &b->cfg_q[p], 1);
            }
        }
    }
}

static void build_frame(ultra_sender_t *l, int64_t start_us, uint8_t *out)
{
    uint32_t i;
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        uint8_t *s = out + ULTRA_SLOT_BYTES * i;
        b->f_start = ULTRA_NO_START;
        b->f_floor = 0;
        /* every 16th slot of a board with a continuous stream is a refresh: a board that rebooted meanwhile still
         * gets its settings and light definitions */
        if (b->urgent_run >= REFRESH_RUN && take_refresh(l, b, s, start_us)) {
            b->urgent_run = 0;
        } else if (take_urgent(l, b, s, start_us)) {
            b->urgent_run++;
        } else {
            b->urgent_run = 0;
            if (!take_refresh(l, b, s, start_us)) {
                ultra_slot_nop(s);
            }
        }
        b->f_dur = s[0] == ULTRA_SET ? ultra_get24(s + ULTRA_PAYLOAD + 22) : 0u;
    }
}

/* the power-off notice (0.2.6): l->off counts its copies down to OFF_QUIET (all out: nothing until the wake); they go
 * at least OFF_GAP_US apart, the cut OFF_CUT_US after the first */
#define OFF_QUIET  1u
#define OFF_COPIES 3u
#define OFF_GAP_US 50000
#define OFF_CUT_US 200000

/* a notice copy went out at start_us (or was retimed to it): the next not before 50 ms; the first sets the cut */
static void notice_at(ultra_sender_t *l, int64_t start_us)
{
    l->hold_us = start_us + OFF_GAP_US;
    if (l->off == OFF_QUIET + OFF_COPIES - 1u) {
        l->off_cut_us = start_us + OFF_CUT_US;
    }
}

int64_t (*ultra_sender_rw_hold_)(const ultra_sender_t *l, const uint8_t *buf);

/* the WS config-frame sequence's next frame (no Ultra header), with the line's bit timing. Reached through ws_frame,
 * which ultra_sender_switch_to_ultra() sets: a host that never switches boards doesn't link it */
static uint32_t ws_frame_build(ultra_sender_t *l, int64_t now_us, uint8_t *buf)
{
    const uint32_t len = ultra_switch_frame(buf, l->switch_frame++, l->n);
    l->kind = KIND_WS;
    l->last_time_us = now_us + (int64_t)(((uint32_t)ultra_wire_ns(&l->timing, buf, len) + 500u) / 1000u);
    l->last_bytes = (uint16_t)len;
    return len;
}

static uint32_t (*ws_frame)(ultra_sender_t *l, int64_t now_us, uint8_t *buf);

int ultra_sender_busy(const ultra_sender_t *l)
{
    if (l->off == OFF_QUIET) {
        return 0;                                /* (commands wait for the wake) */
    }
    if (l->switch_frame < ULTRA_SWITCH_WS_FRAMES || l->preamble_left != 0u || l->queued != 0u || l->off != 0u) {
        return 1;
    }
    {
        uint32_t i;
        for (i = 0; i < l->n; i++) {
            if (l->boards[i].cfg_dirty) {
                return 1;
            }
        }
    }
    return 0;
}

int64_t ultra_sender_due_us(const ultra_sender_t *l)
{
    const int64_t due = l->off == OFF_QUIET ? INT64_MAX               /* nothing until the wake */
                      : (!l->has_sent || ultra_sender_busy(l)) ? l->last_start_us
                                                               : l->last_start_us + (int64_t)l->refresh_us;
    return due > l->hold_us ? due : l->hold_us;   /* not before a response window's end (or the notice's next copy) */
}

uint32_t ultra_sender_next_frame(ultra_sender_t *l, int64_t now_us, uint8_t *buf)
{
    uint32_t len;
    if (l->n == 0u || l->off == OFF_QUIET) {
        return 0;                                /* deinitialized; after a power-off notice until the wake */
    }
    flush_config(l);
    if (now_us < l->hold_us ||
        (!ultra_sender_busy(l) && l->has_sent && now_us - l->last_start_us < (int64_t)l->refresh_us)) {
        return 0;
    }
    l->has_sent = 1;
    l->hdr_flags = 0;
    l->last_start_us = now_us;
    l->last_ends_preamble = 0;
    l->frames++;
    if (l->switch_frame < ULTRA_SWITCH_WS_FRAMES && l->off == 0u) {
        return ws_frame(l, now_us, buf);         /* a WS config frame (only after ultra_sender_switch_to_ultra) */
    }
    len = ultra_frame_bytes(l->n);
    l->last_notice = l->off != 0u;               /* (quiet returned above: copies to send) */
    if (l->last_notice || l->preamble_left != 0u) {
        /* one slot kind on every board: the power-off notice (before everything; each board's IDENTIFY SEQ), else
         * the preamble's DARK */
        uint32_t i;
        if (l->last_notice) {
            l->off--;
        } else {
            l->preamble_left--;
        }
        for (i = 0; i < l->n; i++) {
            ultra_sender_board_t *b = &l->boards[i];
            uint8_t *s = buf + ULTRA_SLOT_BYTES * i;
            if (l->last_notice) {
                build_identify(b, s);
            } else {
                ultra_slot_dark(s);
            }
            b->f_start = ULTRA_NO_START;
            b->f_floor = 0;
        }
        ultra_sender_finish_(l, now_us, buf);
        if (l->last_notice) {
            notice_at(l, now_us);
        } else if (l->preamble_left == 0u) {
            l->ready_us = l->last_time_us;       /* (moved again if this frame is retimed) */
            l->ready_frames = 0;
            l->last_ends_preamble = 1;
            if (l->preamble_full && l->powered) {
                l->synced_once = 1;              /* a full preamble reached the powered boards */
            }
        }
    } else {
        build_frame(l, now_us, buf);
        ultra_sender_finish_(l, now_us, buf);
        if (l->ready_frames < 0xFFFFu) {
            l->ready_frames++;                   /* headers since the preamble (the return channel waits for them) */
        }
    }
    l->kind = KIND_ULTRA;
    l->last_bytes = (uint16_t)len;
    return len;
}

uint32_t ultra_sender_retime(ultra_sender_t *l, int64_t now_us, uint8_t *buf)
{
    if (l->n == 0u) {
        return 0;
    }
    l->last_start_us = now_us;
    if (l->kind == KIND_ULTRA) {
        ultra_sender_finish_(l, now_us, buf);
        if (l->hdr_flags & ULTRA_HDR_RW) {
            l->hold_us = ultra_sender_rw_hold_(l, buf);
        }
        if (l->last_ends_preamble) {
            l->ready_us = l->last_time_us;       /* the preamble ends when its last frame really goes out */
        }
        if (l->last_notice) {
            notice_at(l, now_us);                /* its spacing and the cut move with it */
        }
    } else if (l->kind == KIND_WS) {
        l->last_time_us = now_us + (int64_t)(((uint32_t)ultra_wire_ns(&l->timing, buf, l->last_bytes) + 500u) /
                                             1000u);
    }
    return l->last_bytes;
}

int64_t ultra_sender_last_time_us(const ultra_sender_t *l)
{
    return l->last_time_us;
}

uint32_t ultra_sender_frame_us(const ultra_sender_t *l)
{
    return l->frame_us;
}

/* everything again as new commands; SETs restart at start_us, effects keep their own start */
static void replay(ultra_sender_t *l, int64_t start_us)
{
    uint32_t i, p;
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        const uint32_t pages = cfg_pages(b);
        uint16_t k;
        for (p = 0; p < pages; p++) {
            q_put(l, b, &b->cfg_q[p], 0, 1);
        }
        for (k = b->first; k != END; k = l->items[k].next) {
            ultra_sender_item_t *it = &l->items[k];
            if (!(it->q.flags & IT_VALID) || !ITEM_ON(b, it->key)) {
                continue;
            }
            if (KEY_RANK(it->key) == RK_SET) {
                it_set_start(it, start_us);
            }
            q_put(l, b, &it->q, 0, 1);
        }
        clear_put(l, b);
    }
}

static void resync(ultra_sender_t *l, int64_t now_us, int full)
{
    l->preamble_full = (uint8_t)(full != 0);
    l->preamble_left = (uint16_t)ultra_preamble_frames(l->n);
    l->ready_us = INT64_MIN;
    if (!full && l->preamble_left > POWER_ON_FRAMES) {
        l->preamble_left = POWER_ON_FRAMES;
    }
    replay(l, now_us + stream_lead(l));          /* (the lead counts the preamble and switch frames in) */
}

void ultra_sender_resync(ultra_sender_t *l, int64_t now_us)
{
    if (l->n != 0u) {
        resync(l, now_us, 1);
    }
}

void ultra_sender_power(ultra_sender_t *l, int on, int64_t now_us)
{
    if (l->n == 0u) {
        return;
    }
    if (!on) {
        l->powered = 0;
        return;
    }
    l->powered = 1;
    l->off = 0;                                  /* (a power-off notice is over: the boards start afresh) */
    resync(l, now_us, !l->synced_once);
}

int ultra_sender_power_off(ultra_sender_t *l)
{
    uint32_t i;
    if (l->n == 0u || l->off != 0u) {
        return ULTRA_ERR_ARG;                    /* deinitialized, or a notice under way */
    }
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        q_drop(l, b, &b->id_q, 1);               /* (a queued IDENTIFY gives way) */
        b->id_seq++;                             /* the notice's SEQ: new, one for its 3 copies */
        b->id_val = ULTRA_IDENT_POWER_OFF;
    }
    l->off = OFF_QUIET + OFF_COPIES;
    return ULTRA_OK;
}

int64_t ultra_sender_power_off_at(const ultra_sender_t *l)
{
    return l->off == 0u ? INT64_MIN : l->off != OFF_QUIET ? INT64_MAX
         : l->off_cut_us > l->last_time_us ? l->off_cut_us : l->last_time_us;
}

int ultra_sender_switch_to_ultra(ultra_sender_t *l, int64_t now_us, uint8_t boot_protocol)
{
    if (l->n == 0u || (boot_protocol != 0xFFu && !ultra_config_value_ok(ULTRA_CFG_BOOT_PROTOCOL, boot_protocol))) {
        return ULTRA_ERR_ARG;
    }
    ws_frame = ws_frame_build;
    l->switch_frame = 0;
    if (boot_protocol != 0xFFu) {
        (void)ultra_sender_config(l, ULTRA_SENDER_ALL, ULTRA_CFG_BOOT_PROTOCOL, boot_protocol);
        flush_config(l);
    }
    resync(l, now_us, 1);
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------------------------- lights */
int ultra_sender_light_def(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_light_def_t *d)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t s[ULTRA_SLOT_BYTES];
    uint32_t i;
    if (!light_ok(l, board, light) || d == 0 || (d->flags & ~ULTRA_DEF_FLAGS) != 0u) {
        return ULTRA_ERR_ARG;
    }
    for (i = 0; i < 5u; i++) {
        if (d->roles[i] > ULTRA_ROLE_WARM_WHITE) {
            return ULTRA_ERR_ARG;                /* 8-15 reserved */
        }
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_DEF, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    ultra_slot_light_def(s, light, it->slot[2], d->roles, d->gamma_x1000, d->flags, d->min_power, d->max_power);
    put_static(l, b, it, s);
    return ULTRA_OK;
}

int ultra_sender_light_opt(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_light_opt_t *o)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t s[ULTRA_SLOT_BYTES];
    if (!light_ok(l, board, light) || o == 0 || o->points > ULTRA_OPT_POINTS_MAX ||
        (o->flags & ~ULTRA_OPT_FLAGS) != 0u) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_OPT, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    ultra_slot_light_opt_points(s, light, it->slot[2], o->flags, o->cw_mired10, o->ww_mired10, o->warm_mired10,
                                o->red_assist, o->points, o->pt_m, o->pt_mired10);
    put_static(l, b, it, s);
    return ULTRA_OK;
}

/* A SET slot as ultra_slot_set() builds it (start offset 0), without its CRC: every SET has a start, so finish_
 * stamps and seals it in each frame that carries it (the first pass seals every stamped slot). One CRC less per
 * set() / stream() (review round 10, O3 #6). */
static void set_fill(uint8_t *s, uint8_t light, uint8_t seq, uint8_t flags, const ultra_values_t *v, uint32_t dur_ms)
{
    uint8_t *p = s + ULTRA_PAYLOAD;
    uint32_t i;
    ultra_slot_begin(s, ULTRA_SET, light, (uint8_t)(flags & ULTRA_SET_FLAGS), seq);
    ultra_put16(p, v->m);
    for (i = 0; i < 5u; i++) {
        ultra_put16(p + 2 + 2 * i, v->a[i]);
        ultra_put16(p + 12 + 2 * i, v->b[i]);
    }
    ultra_put24(p + 22, dur_ms > 0xFFFFFFu ? 0xFFFFFFu : dur_ms);
}

/* SET with an absolute start, or (autostart) the group start for now_us, taken once the command is accepted */
static int set_impl(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v, uint32_t transition_ms,
                    uint8_t flags, int64_t t_us, int autostart)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    if (!light_ok(l, board, light) || v == 0 || (flags & ~ULTRA_SET_FLAGS) != 0u ||
        transition_ms > 0xFFFFFFu) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_SET, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    set_fill(it->slot, light, next_seq(b, it), flags, v, transition_ms);
    it->q.flags = IT_VALID;
    it_set_start(it, autostart ? ultra_sender_start_time(l, board, t_us) : t_us);
    q_put(l, b, &it->q, 0, 1);
    return ULTRA_OK;
}

int ultra_sender_set_at(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v,
                        uint32_t transition_ms, uint8_t flags, int64_t start_us)
{
    return set_impl(l, board, light, v, transition_ms, flags, start_us, 0);
}

int ultra_sender_set(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v,
                     uint32_t transition_ms, uint8_t flags, int64_t now_us)
{
    return set_impl(l, board, light, v, transition_ms, flags, now_us, 1);
}

int ultra_sender_stream(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_values_t *v, uint32_t glide_ms,
                        int64_t now_us)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    if (!light_ok(l, board, light) || v == 0 || glide_ms > 0xFFFFFFu) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_SET, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    set_fill(it->slot, light, next_seq(b, it), 0, v, glide_ms);
    it->q.flags = IT_VALID | IT_STREAM;
    it_set_start(it, now_us + stream_lead(l));
    it->aux = glide_ms;
    q_put(l, b, &it->q, 1, 1);
    return ULTRA_OK;
}

uint32_t ultra_fx_period_ms(uint8_t fx, const uint8_t *params, uint32_t nparams, const ultra_strobe_step_t *steps,
                            uint8_t count)
{
    uint32_t v = 0, i;
    if (params == 0) {
        nparams = 0;
    }
    switch (fx) {
    case ULTRA_FX_PULSE:
        v = nparams >= 9u ? ultra_get24(params + 6) : 0u;
        return 2u * (v != 0u ? v : 1u);
    case ULTRA_FX_BREATHE:
        v = nparams >= 3u ? ultra_get24(params) : 0u;
        return v != 0u ? v : 4000u;
    case ULTRA_FX_COLOR_LOOP:
        v = nparams >= 3u ? ultra_get24(params) : 0u;
        return v != 0u ? v : 30000u;
    case ULTRA_FX_STROBE:
        for (i = 0; steps != 0 && i < count && i < ULTRA_MAX_STEPS; i++) {
            v += steps[i].duration_ms != 0u ? steps[i].duration_ms : 1u;
        }
        return v;
    default:
        return 0;
    }
}

/* an effect id and exactly its parameter bytes (a defined layout, nothing zero-filled or cut off) */
static int fx_args_ok(uint8_t fx, const uint8_t *params, uint32_t nparams)
{
    return fx < ULTRA_FX_COUNT && nparams == ultra_fx_param_bytes(fx) && (params != 0 || nparams == 0u);
}

/* The light's EFFECT item (0: the pool is full): the slot, its period (a strobe's from its steps, else as the board
 * reads the parameters), its start (t_us, or autostart: the group start for now t_us on the effect's phase), the old
 * effect's steps dropped (also from the queue), queued */
static ultra_sender_item_t *effect_put(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t fx,
                                       const uint8_t *params, uint32_t nparams, const ultra_strobe_step_t *steps,
                                       uint8_t count, int64_t t_us, int autostart)
{
    ultra_sender_board_t *b = &l->boards[board];
    ultra_sender_item_t *it = item_get(l, b, KEY(light, RK_EFFECT, 0));
    if (it == 0) {
        return 0;
    }
    ultra_slot_effect(it->slot, light, next_seq(b, it), fx, 0, params, nparams);
    it->q.flags = IT_VALID;
    it->aux = ultra_fx_period_ms(fx, it->slot + ULTRA_PAYLOAD + 5, nparams, steps, count);
    it_set_start(it, autostart ? phase_start(ultra_sender_start_time(l, board, t_us), it->aux) : t_us);
    drop_steps(l, b, light, 0);
    q_put(l, b, &it->q, 0, 1);
    return it;
}

/* EFFECT with an absolute start, or (autostart) the group start for now_us on the effect's phase */
static int effect_impl(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params,
                       uint32_t nparams, int64_t t_us, int autostart)
{
    if (!light_ok(l, board, light) || fx == ULTRA_FX_STROBE || !fx_args_ok(fx, params, nparams)) {
        return ULTRA_ERR_ARG;
    }
    return effect_put(l, board, light, fx, params, nparams, 0, 0, t_us, autostart) != 0 ? ULTRA_OK : ULTRA_ERR_FULL;
}

int ultra_sender_effect_at(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params,
                           uint32_t nparams, int64_t start_us)
{
    return effect_impl(l, board, light, fx, params, nparams, start_us, 0);
}

int ultra_sender_effect(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params,
                        uint32_t nparams, int64_t now_us)
{
    return effect_impl(l, board, light, fx, params, nparams, now_us, 1);
}

/* strobe with an absolute start, or (autostart) the group start for now_us on the strobe's phase */
static int strobe_impl(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_strobe_step_t *steps,
                       uint8_t count, int64_t t_us, int autostart)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint32_t i, missing = 0;
    uint8_t seq, p[1];
    if (!light_ok(l, board, light) || steps == 0 || count == 0u || count > ULTRA_MAX_STEPS) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    missing = item_find(l, b, KEY(light, RK_EFFECT, 0)) == 0;
    for (i = 0; i < count; i++) {
        missing += item_find(l, b, KEY(light, RK_STEP, i)) == 0;
    }
    if (missing > items_free(l)) {
        return ULTRA_ERR_FULL;                   /* all or nothing */
    }
    p[0] = count;
    it = effect_put(l, board, light, ULTRA_FX_STROBE, p, 1, steps, count, t_us, autostart);
    seq = it->slot[2];
    for (i = 0; i < count; i++) {
        const ultra_strobe_step_t *st = &steps[i];
        ultra_sender_item_t *si = item_get(l, b, KEY(light, RK_STEP, i));
        ultra_slot_step(si->slot, light, seq, (uint8_t)i, count, st->duration_ms, st->transition_ms, st->v.m, st->v.a,
                        st->v.b);
        si->q.flags = IT_VALID;
        it_set_start(si, ULTRA_NO_START);
        q_put(l, b, &si->q, 0, 1);
    }
    return ULTRA_OK;
}

int ultra_sender_strobe_at(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_strobe_step_t *steps,
                           uint8_t count, int64_t start_us)
{
    return strobe_impl(l, board, light, steps, count, start_us, 0);
}

int ultra_sender_strobe(ultra_sender_t *l, uint16_t board, uint8_t light, const ultra_strobe_step_t *steps, uint8_t count,
                        int64_t now_us)
{
    return strobe_impl(l, board, light, steps, count, now_us, 1);
}

int ultra_sender_scene(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t mode, uint8_t fade_100ms,
                       const ultra_values_t *v)
{
    static const uint16_t zero[5] = { 0, 0, 0, 0, 0 };
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t s[ULTRA_SLOT_BYTES];
    if (!light_ok(l, board, light) || mode > 1u || (mode == 1u && v == 0)) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_SCENE, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    if (v != 0) {
        ultra_slot_scene(s, light, it->slot[2], mode, fade_100ms, v->m, v->a, v->b);
    } else {
        ultra_slot_scene(s, light, it->slot[2], mode, fade_100ms, 0, zero, zero);
    }
    put_static(l, b, it, s);
    return ULTRA_OK;
}

int ultra_sender_scene_effect(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t fx, const uint8_t *params,
                              uint32_t nparams)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t s[ULTRA_SLOT_BYTES];
    if (!light_ok(l, board, light) || fx == ULTRA_FX_STROBE || !fx_args_ok(fx, params, nparams)) {
        return ULTRA_ERR_ARG;                    /* (strobe: the board stores no steps, so no power-on strobe) */
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_SCENE_FX, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    ultra_slot_scene_effect(s, light, it->slot[2], fx, params, nparams);
    put_static(l, b, it, s);
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------- local pixel effects (0.2.8) */
static int pixel_fx_ok(const ultra_pixel_fx_t *fx)
{
    return fx != 0 && (fx->flags & ~0x3Fu) == 0u;     /* (bits 6-7 are reserved: the header's builder drops them) */
}

int ultra_sender_pixel_fx_at(ultra_sender_t *l, uint16_t board, uint8_t segment, const ultra_pixel_fx_t *fx,
                             int64_t start_us)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    ultra_pixel_fx_t f;
    if (!light_ok(l, board, segment) || segment >= ULTRA_PX_SEGMENTS || !pixel_fx_ok(fx)) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(segment, RK_PIXFX, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    f = *fx;
    ultra_slot_pixel_fx(it->slot, segment, next_seq(b, it), &f);   /* (START: stamped against every frame's TIME) */
    it->q.flags = IT_VALID;
    it->aux = 0;
    it_set_start(it, start_us);
    q_put(l, b, &it->q, 0, 1);
    return ULTRA_OK;
}

int ultra_sender_pixel_fx(ultra_sender_t *l, uint16_t board, uint8_t segment, const ultra_pixel_fx_t *fx, int64_t now_us)
{
    if (!light_ok(l, board, segment) || segment >= ULTRA_PX_SEGMENTS || !pixel_fx_ok(fx)) {
        return ULTRA_ERR_ARG;
    }
    return ultra_sender_pixel_fx_at(l, board, segment, fx, ultra_sender_start_time(l, board, now_us));
}

int64_t ultra_sender_pixel_fx_start(const ultra_sender_t *l, uint16_t board, uint8_t segment)
{
    const ultra_sender_item_t *it;
    if (!light_ok(l, board, segment) || segment >= ULTRA_PX_SEGMENTS) {
        return ULTRA_NO_START;
    }
    it = item_find((ultra_sender_t *)l, &l->boards[board], KEY(segment, RK_PIXFX, 0));
    return it != 0 && (it->q.flags & IT_VALID) ? it_start(it) : ULTRA_NO_START;
}

int ultra_pixel_config_value_ok(uint8_t id, uint16_t value)
{
    switch (id) {
    case ULTRA_CFG_PX_CHIP:
        return value < ULTRA_PXC_COUNT;
    case ULTRA_CFG_PX_POLICY:
        return value <= (ULTRA_PXP_POWER_ON | ULTRA_PXP_ON_LOSS | ULTRA_PXP_STREAM_WINS);
    case ULTRA_CFG_PX_LOSS_TIMEOUT:
        return value == 0u || (value >= 5u && value <= 600u);
    case ULTRA_CFG_PX_ORDER:
    case ULTRA_CFG_PX_PIXELS:
    case ULTRA_CFG_PX_FPS:
    case ULTRA_CFG_PX_MA_CH:
    case ULTRA_CFG_PX_LIMIT_MA:
    case ULTRA_CFG_PX_WHITE:
    case ULTRA_CFG_PX_VOFFSET:
    case ULTRA_CFG_PX_VLENGTH:
        return 1;
    default:
        return 0;
    }
}

int ultra_sender_pixel_config(ultra_sender_t *l, uint16_t board, uint8_t id, uint16_t value)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t ids[ULTRA_CFG_PX_COUNT];
    uint16_t vals[ULTRA_CFG_PX_COUNT];
    uint8_t s[ULTRA_SLOT_BYTES];
    uint32_t page, n = 0, j;
    if (!board_ok(l, board) || !ultra_pixel_config_value_ok(id, value)) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    page = (uint32_t)(id - ULTRA_CFG_PX_FIRST) / 9u;
    it = item_get(l, b, KEY(ULTRA_LIGHT_BOARD, RK_HWCFG, page));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    /* the page's pairs as they are (consecutive from pair 0, in id order), with this one set */
    if (it->q.flags & IT_VALID) {
        for (j = 0; j < 9u && it->slot[ULTRA_PAYLOAD + 3u * j] != 0u; j++) {
            ids[n] = it->slot[ULTRA_PAYLOAD + 3u * j];
            vals[n++] = (uint16_t)ultra_get16(it->slot + ULTRA_PAYLOAD + 3u * j + 1u);
        }
    }
    for (j = 0; j < n && ids[j] != id; j++) {
    }
    if (j == n) {
        for (j = n; j > 0u && ids[j - 1u] > id; j--) {
            ids[j] = ids[j - 1u];
            vals[j] = vals[j - 1u];
        }
        n++;
    }
    ids[j] = id;
    vals[j] = value;
    ultra_slot_config(s, it->slot[2], ids, vals, n);
    if ((it->q.flags & IT_VALID) && memcmp(s + ULTRA_PAYLOAD, it->slot + ULTRA_PAYLOAD, ULTRA_PAYLOAD_LEN) == 0) {
        return ULTRA_OK;                         /* unchanged: nothing to send */
    }
    copy32(it->slot, s);                         /* (its SEQ and CRC when the page is flushed with the next frame) */
    it->q.flags = IT_VALID | IT_HWDIRTY;
    it_set_start(it, ULTRA_NO_START);
    l->hw_dirty = 1;
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------------------------- effects */
uint32_t ultra_fx_pulse(uint8_t *p, uint32_t on_ms, uint32_t off_ms, uint32_t interval_ms, uint16_t min,
                        uint16_t max)
{
    ultra_put24(p, on_ms > 0xFFFFFFu ? 0xFFFFFFu : on_ms);
    ultra_put24(p + 3, off_ms > 0xFFFFFFu ? 0xFFFFFFu : off_ms);
    ultra_put24(p + 6, interval_ms > 0xFFFFFFu ? 0xFFFFFFu : interval_ms);
    ultra_put16(p + 9, min);
    ultra_put16(p + 11, max);
    return 13;
}

uint32_t ultra_fx_flicker(uint8_t *p, uint16_t alpha, uint16_t intensity, uint16_t interval_ms)
{
    ultra_put16(p, alpha);
    ultra_put16(p + 2, intensity);
    ultra_put16(p + 4, interval_ms);
    return 6;
}

uint32_t ultra_fx_random(uint8_t *p, uint32_t transition_ms, uint32_t interval_ms)
{
    ultra_put24(p, transition_ms > 0xFFFFFFu ? 0xFFFFFFu : transition_ms);
    ultra_put24(p + 3, interval_ms > 0xFFFFFFu ? 0xFFFFFFu : interval_ms);
    return 6;
}

uint32_t ultra_fx_candle(uint8_t *p, uint16_t intensity, uint16_t speed_pct)
{
    ultra_put16(p, intensity);
    ultra_put16(p + 2, speed_pct);
    return 4;
}

uint32_t ultra_fx_breathe(uint8_t *p, uint32_t period_ms, uint16_t min, uint16_t max)
{
    ultra_put24(p, period_ms > 0xFFFFFFu ? 0xFFFFFFu : period_ms);
    ultra_put16(p + 3, min);
    ultra_put16(p + 5, max);
    return 7;
}

uint32_t ultra_fx_color_loop(uint8_t *p, uint32_t period_ms, uint16_t saturation)
{
    ultra_put24(p, period_ms > 0xFFFFFFu ? 0xFFFFFFu : period_ms);
    ultra_put16(p + 3, saturation);
    return 5;
}

uint32_t ultra_fx_sunrise(uint8_t *p, uint32_t duration_ms)
{
    ultra_put24(p, duration_ms > 0xFFFFFFu ? 0xFFFFFFu : duration_ms);
    return 3;
}

uint32_t ultra_fx_sunset(uint8_t *p, uint32_t duration_ms)
{
    return ultra_fx_sunrise(p, duration_ms);
}

/* ------------------------------------------------------------------------------------------------- board */
/* The values each CONFIG id takes (wiki: CONFIG settings, protocol 0.2.5), lo..hi. A protocol release that adds values
 * to an id extends its row; ids after the table (added by a later header) take any value. */
static const uint16_t cfg_range[][2] = {
    { 1, 0 },                                    /*  0: none (never sent) */
    { 0, 14 },                                   /*  1: PWM preset */
    { 0, 65535 },                                /*  2: PWM period: 0 or 1440..65535 (below) */
    { 0, 2 },                                    /*  3: dithering */
    { 1, 255 },                                  /*  4: dither threshold */
    { 0, 2 },                                    /*  5: dim floor */
    { 5, 100 },                                  /*  6: minimum pulse */
    { 0, 4 },                                    /*  7: stagger */
    { 0, 3 },                                    /*  8: signal loss (3 = scene: 0.2.5) */
    { 5, 600 },                                  /*  9: signal loss timeout */
    { 0, 2 },                                    /* 10: feedback channel */
    { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 },   /* 11-15: LED current, mA */
    { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 },   /* 16-20: channel limits, mA */
    { 0, 65535 },                                /* 21: board limit, mA */
    { 0, 3 },                                    /* 22: boot protocol */
    { 0, 1 },                                    /* 23: soft start */
    { 0, 65535 },                                /* 24: retry policy (0.2.5): count | interval << 8, every value */
};

int ultra_config_value_ok(uint8_t id, uint16_t value)
{
    if (id == ULTRA_CFG_NONE || id >= ULTRA_CFG_COUNT) {
        return 0;
    }
    if (id >= sizeof cfg_range / sizeof cfg_range[0]) {
        return 1;
    }
    if (id == ULTRA_CFG_PWM_PERIOD && value != 0u && value < 1440u) {
        return 0;
    }
    return value >= cfg_range[id][0] && value <= cfg_range[id][1];
}

int ultra_sender_config(ultra_sender_t *l, uint16_t board, uint8_t id, uint16_t value)
{
    uint32_t i;
    if ((board != ULTRA_SENDER_ALL && !board_ok(l, board)) || !ultra_config_value_ok(id, value)) {
        return ULTRA_ERR_ARG;
    }
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        if (board != ULTRA_SENDER_ALL && board != i) {
            continue;
        }
        if (cfg_has(b, id) && b->cfg[id] == value) {
            continue;
        }
        b->cfg[id] = value;
        b->cfg_have[id >> 5] |= BIT32(id & 31u);
        b->cfg_dirty = 1;
    }
    return ULTRA_OK;
}

/* the boards' PWM presets: the period in 144 MHz counts, index = CONFIG 1's value */
static const uint16_t pwm_steps[15] = { 65535, 46341, 32768, 23170, 16384, 11585, 8192, 7200,
                                        5760,  4800,  4096,  3600,  2880,  2048,  1440 };
#define PWM_CLOCK_HZ 144000000u

void ultra_pwm_for_hz(uint32_t hz, uint16_t *preset, uint16_t *period)
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
        const uint64_t err = ultra_udiv64_(diff * 1000000u, hs);   /* ppm */
        if (err < best) {
            best = err;
            *preset = (uint16_t)i;
        }
    }
    if (best > 5000u) {
        uint32_t steps = (PWM_CLOCK_HZ + hz / 2u) / hz;          /* (< 2^32: no 64-bit division) */
        *period = (uint16_t)(steps < 1440u ? 1440u : steps > 65535u ? 65535u : steps);
    }
}

int ultra_sender_pwm_hz(ultra_sender_t *l, uint16_t board, uint32_t hz)
{
    uint16_t preset, period;
    int r;
    ultra_pwm_for_hz(hz, &preset, &period);
    r = ultra_sender_config(l, board, ULTRA_CFG_PWM_PRESET, preset);
    return r != ULTRA_OK ? r : ultra_sender_config(l, board, ULTRA_CFG_PWM_PERIOD, period);
}

int ultra_sender_identify(ultra_sender_t *l, uint16_t board, uint8_t value, uint8_t arg)
{
    uint32_t i;
    if ((board != ULTRA_SENDER_ALL && !board_ok(l, board)) || value > ULTRA_IDENT_VERSION ||
        (value == ULTRA_IDENT_CAL && arg != 0u && (arg < ULTRA_IDENT_CAL_MIN || arg > ULTRA_IDENT_CAL_MAX))) {
        return ULTRA_ERR_ARG;
    }
    for (i = 0; i < l->n; i++) {
        ultra_sender_board_t *b = &l->boards[i];
        if (board != ULTRA_SENDER_ALL && board != i) {
            continue;
        }
        b->id_seq++;
        b->id_val = value;
        b->id_arg = (value == ULTRA_IDENT_CAL || value == ULTRA_IDENT_DARK) ? arg : 0u;
        q_put(l, b, &b->id_q, 0, 1);
    }
    return ULTRA_OK;
}

int ultra_sender_identify_pending(const ultra_sender_t *l, uint16_t board)
{
    return board_ok(l, board) && l->boards[board].id_q.copies != 0u;
}

/* ------------------------------------------------------------------------------------------------- inspection */
int ultra_sender_pending(const ultra_sender_t *l)
{
    return l->queued != 0u;
}

uint16_t ultra_sender_waiting(const ultra_sender_t *l, uint16_t board)
{
    return board_ok(l, board) ? l->boards[board].unsent : 0u;
}

uint16_t ultra_sender_items_used(const ultra_sender_t *l)
{
    return l->used;
}

int ultra_sender_preset_seq(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t type, uint8_t seq)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t rk;
    if (!board_ok(l, board)) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    if (light == ULTRA_LIGHT_BOARD) {
        if (type == ULTRA_CONFIG) {
            b->cfg_seq = (uint8_t)(seq - 1u);
        } else if (type == ULTRA_IDENTIFY) {
            b->id_seq = (uint8_t)(seq - 1u);
        } else if (type == ULTRA_POLL) {
            b->poll_seq = (uint8_t)(seq - 1u);
        } else {
            return ULTRA_ERR_ARG;
        }
        return ULTRA_OK;
    }
    switch (type) {
    case ULTRA_LIGHT_DEF: rk = RK_DEF; break;
    case ULTRA_LIGHT_OPT: rk = RK_OPT; break;
    case ULTRA_SET: rk = RK_SET; break;
    case ULTRA_EFFECT: rk = RK_EFFECT; break;
    case ULTRA_SCENE: rk = RK_SCENE; break;
    case ULTRA_SCENE_EFFECT: rk = RK_SCENE_FX; break;
    case ULTRA_PIXEL_FX: rk = RK_PIXFX; break;
    default: return ULTRA_ERR_ARG;
    }
    if (!light_ok(l, board, light)) {
        return ULTRA_ERR_ARG;
    }
    it = item_get(l, b, KEY(light, rk, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    if (it->q.flags & IT_VALID) {
        return ULTRA_ERR_ARG;
    }
    it->slot[2] = (uint8_t)(seq - 1u);
    return ULTRA_OK;
}

/* ------------------------------------------------------------------------------------------------- values */
void ultra_values_from_color(const uint8_t roles[5], const ultra_color_t *c, ultra_values_t *v)
{
    uint32_t i;
    v->m = c->brightness;
    for (i = 0; i < 5u; i++) {
        v->a[i] = 0;
        v->b[i] = 65535;
        switch (roles[i]) {
        case ULTRA_ROLE_BRIGHTNESS:
            v->a[i] = 65535;
            break;
        case ULTRA_ROLE_RED:
            v->a[i] = c->red;
            v->b[i] = c->rgb_level;
            break;
        case ULTRA_ROLE_GREEN:
            v->a[i] = c->green;
            v->b[i] = c->rgb_level;
            break;
        case ULTRA_ROLE_BLUE:
            v->a[i] = c->blue;
            v->b[i] = c->rgb_level;
            break;
        case ULTRA_ROLE_WHITE:
            v->a[i] = c->white;
            break;
        case ULTRA_ROLE_COLD_WHITE:
            v->a[i] = c->cold_white;
            break;
        case ULTRA_ROLE_WARM_WHITE:
            v->a[i] = c->warm_white;
            break;
        default:
            break;
        }
    }
}

void ultra_light_def_init(ultra_light_def_t *d)
{
    uint32_t i;
    for (i = 0; i < 5u; i++) {
        d->roles[i] = ULTRA_ROLE_NONE;
        d->min_power[i] = 0;
        d->max_power[i] = 65535;
    }
    d->gamma_x1000 = 2800;
    d->flags = 0;
}
