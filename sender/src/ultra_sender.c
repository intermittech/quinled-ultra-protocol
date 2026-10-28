/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: the scheduler and frame builder (ultra_sender.h). The rules follow the reference host, the
 * ESPHome component ultra_line (ultra_core.h, class Scheduler, and the hub's start delay / group start).
 */
#include <string.h>

#include "ultra_sender.h"
#include "ultra_sender_internal.h"
#include "ultra_sender_descriptor.h"

#define END             0xFFFFu
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
enum { RK_DEF = 0, RK_OPT, RK_SET, RK_SCENE, RK_PIXFX, RK_HWCFG };
#define KEY(light, rank, sub) ((uint16_t)(((uint32_t)(light) << 8) | ((uint32_t)(rank) << 4) | (uint32_t)(sub)))
#define KEY_LIGHT(k)          ((uint8_t)((k) >> 8))
#define KEY_RANK(k)           ((uint8_t)(((k) >> 4) & 15u))
/* an item is part of the board's state while its light is in use (the board's own items, light ULTRA_TARGET_BOARD: the
 * hardware-specific CONFIG pages of an Diff-Power-UR-Digital-M receiver, always) */
#define ITEM_ON(b, key)       (KEY_RANK(key) >= RK_PIXFX || KEY_LIGHT(key) < (b)->lights)

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
    return l->clear_unused ? (b->capacity - b->lights) * 3u : 0u;
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
    if(KEY_RANK(key)==RK_HWCFG){
        uint16_t k;
        it->last_us=it->slot[2];
        for(k=b->first;k!=END;k=l->items[k].next)
            if(KEY_RANK(l->items[k].key)==RK_HWCFG && KEY_LIGHT(l->items[k].key)==KEY_LIGHT(key)){
                it->last_us=l->items[k].last_us;break;
            }
    }
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

/* the SEQ of the item's next command; on a light that was cleared, light definitions, stored scenes
 * never take the SEQ of the clearing slots (the board takes its first command) */
static uint8_t next_seq(const ultra_sender_board_t *b, const ultra_sender_item_t *it)
{
    uint8_t s = (uint8_t)(it->slot[2] + 1u);
    const uint8_t rk = KEY_RANK(it->key);
    if ((rk == RK_DEF || rk == RK_SCENE) && s == b->clr_seq &&
        (KEY_LIGHT(it->key) >= b->clr_min)) {
        s++;
    }
    return s;
}

/* a static item (LIGHT_DEF, LIGHT_OPT, SCENE): s was built with the item's current SEQ; unchanged
 * content queues nothing, else the next SEQ and 3 copies */
static void put_static(ultra_sender_t *l, ultra_sender_board_t *b, ultra_sender_item_t *it, uint8_t *s)
{
    if ((it->q.flags & IT_VALID) && same32(s, it->slot)) {
        return;
    }
    s[2] = next_seq(b, it);
    if (KEY_RANK(it->key) == RK_SCENE) {
        const ultra_sender_item_t *other = item_find(l, b, (uint16_t)(it->key ^ 1u));
        /* The receiver deduplicates by TARGET/type, before dispatching the selector. */
        while ((other != 0 && (other->q.flags & IT_VALID) && s[2] == other->slot[2]) ||
               (KEY_LIGHT(it->key) >= b->clr_min &&
                (s[2] == b->clr_seq || s[2] == (uint8_t)(b->clr_seq + 1u)))) {
            s[2]++;
        }
    }
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
    for (id = 1; id < ULTRA_SENDER_CFG_COUNT && n < 9u; id++) {
        if (cfg_has(b, id)) {
            if (k >= 9u * page) {
                ids[n] = (uint8_t)id;
                vals[n++] = b->cfg[id];
            }
            k++;
        }
    }
    ultra_slot_config(s, ULTRA_TARGET_BOARD, b->cfg_pseq[page], ids, vals, n);
}

static void build_identify(const ultra_sender_board_t *b, uint8_t *s)
{
    if (b->id_val == ULTRA_IDENT_CAL) {
        ultra_slot_calibrate(s, b->id_seq, b->id_arg);
    } else if (b->id_val == ULTRA_IDENT_DARK) {
        ultra_slot_hold_dark(s, b->id_seq, b->id_arg);
    } else if (b->id_val == ULTRA_IDENT_POWER_OFF) {
        ultra_slot_power_off(s, b->id_seq);       /* ultra_sender_power_off */
    } else {
        ultra_slot_identify(s, b->id_seq, b->id_val);
    }
}

/* clearing slot j of the board's unused lights: an empty LIGHT_DEF and both zero-valued SCENEs */
static void build_clear(const ultra_sender_board_t *b, uint32_t j, uint8_t *s)
{
    static const uint16_t zero[5] = { 0, 0, 0, 0, 0 };
    static const uint8_t roles[5] = { 0, 0, 0, 0, 0 };
    const uint8_t light = (uint8_t)(b->lights + j / 3u);
    switch (j % 3u) {
    case 0:
        ultra_slot_light_def(s, light, b->clr_seq, roles, 1000, 0, zero, zero, 0);
        break;
    case 1:
        ultra_slot_scene(s, light, b->clr_seq, 0, 0, 0, zero, zero);
        break;
    default:
        ultra_slot_scene(s, light, (uint8_t)(b->clr_seq + 1u), 1, 0, 0, zero, zero);
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
    if (b->lights < b->clr_min) b->clr_min = b->lights;
    b->clr_pos = 0;
    q_put(l, b, &b->clr_q, 0, 0);
}

/* ------------------------------------------------------------------------------------------------- start times */
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
    c->light_capacity = lights > 5u ? lights : 5u;
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
    return (c->boards == 0u || c->boards > ULTRA_SENDER_MAX_BOARDS || c->light_capacity > 240u || c->lights > c->light_capacity ||
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
        b->capacity = c->light_capacity;
        b->channels = 240u;
        b->clr_min = c->lights;
        b->clr_seq = rnd8(l);
        b->poll_seq = rnd8(l);
        clear_put(l, b);
    }
    lights_changed(l);
    return ULTRA_OK;
}

int ultra_sender_digital(ultra_sender_t *l, int on)
{
    if (l->n != 1u) {
        return ULTRA_ERR_ARG;                    /* one receiver per port */
    }
    l->digital = (uint8_t)(on != 0);
    if (on) {
        l->preamble_left = 0;
        l->preamble_full = 1;
        l->synced_once = 1;
        l->ready_us = INT64_MIN;
        l->ready_frames = 0;
    } else {
        l->preamble_left = (uint16_t)ultra_preamble_frames(l->n);
    }
    return ULTRA_OK;
}

int ultra_sender_set_lights(ultra_sender_t *l, uint16_t board, uint8_t lights)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint16_t i;
    uint8_t old, tries;
    if (!board_ok(l, board) || lights > l->boards[board].capacity) {
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
        if (KEY_RANK(it->key) >= RK_PIXFX) {
            continue;                              /* (the board's own items stay) */
        }
        if (KEY_LIGHT(it->key) >= lights) {
            q_drop(l, b, &it->q, 1);               /* not sent any more */
        } else if (KEY_LIGHT(it->key) >= old && (it->q.flags & IT_VALID)) {
            if (KEY_RANK(it->key) == RK_PIXFX) {
                it->aux = 0;
            }
            q_put(l, b, &it->q, 0, 1);             /* back: its state again */
        }
    }
    /* the clearing slots: a SEQ none of the newly cleared lights' items has (their last commands) */
    for (tries = 0; tries < 16u; tries++) {
        int clash = 0;
        for (i = b->first; i != END; i = it->next) {
            it = &l->items[i];
            if (KEY_LIGHT(it->key) >= lights && KEY_LIGHT(it->key) < old && KEY_RANK(it->key) < RK_PIXFX &&
                (it->slot[2] == b->clr_seq ||
                 (KEY_RANK(it->key) == RK_SCENE && it->slot[2] == (uint8_t)(b->clr_seq + 1u)))) {
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
/* Start offset of SET (int16 ms) / PIXEL_FX (int32 ms) relative to this frame's host ms floor(TIME / 1000), then the
 * CRC. The absolute start is rounded to its ms once: every frame that carries an item of that start names the same
 * host ms, and every board adds it to its own floor(TIME / 1000), which the whole-ms TIME wrap keeps on the host's ms
 * grid. floor: not before the frame's ms (a streamed SET starts when it arrives, at the latest). A SET that started
 * more than 32.8 s ago (a long transition refreshed to a board that missed it) starts at that limit with its duration
 * shortened by the rest: it still ends on time. */
static uint32_t ones_in(const uint8_t *d, uint32_t n)
{
    static const uint8_t t[16] = { 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4 };
    uint32_t i, k = 0;
    for (i = 0; i < n; i++) {
        k += (uint32_t)t[d[i] & 15u] + t[d[i] >> 4];
    }
    return k;
}

/* the 1 bits of a slot's bytes that stamp and its seal can change: a PIXEL_FX's offset (4..7), a SET's duration,
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
    int64_t ms;
    uint32_t before = 0;
    if (start_us == ULTRA_NO_START) {
        return 0;
    }
    ms = start_us >= 0 ? (int64_t)ultra_udiv64_((uint64_t)start_us + 500u, 1000u)
                       : -(int64_t)ultra_udiv64_((uint64_t)-start_us + 499u, 1000u);   /* (half up, as before) */
    ms -= (int64_t)ultra_udiv64_((uint64_t)hdr_us, 1000u);                                /* (hdr_us >= 0) */
    if (floor && ms < 0) {
        ms = 0;
    }
    if ((s[0] & ULTRA_TYPE_MASK) == ULTRA_SET) {
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
        if (!force && s[0] != ULTRA_PIXEL_FX) {
            return 0;                            /* (no offset: sealed in the first pass) */
        }
        before = force ? 0u : stamp_ones(s);
        if (s[0] == ULTRA_PIXEL_FX) {   /* (START is at slot byte 4 in both) */
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
static int64_t solve_header(ultra_sender_t *l,int64_t start_us,uint8_t *out,int64_t hint,uint32_t ones)
{
    const ultra_timing_t *t=&l->timing;
    uint8_t *h=out+32u*l->n;
    uint64_t prefix=((uint64_t)ultra_frame_bytes(l->n)*8u-1u)*t->bit_ticks,err=UINT64_MAX;
    uint32_t lo,hi,k,best=0;
    const uint32_t base=ultra_time_wire((uint64_t)start_us);
    (void)hint; (void)ones;
    lo=(uint32_t)((prefix+t->zero_ticks)*1000000u/t->clock_hz);
    hi=(uint32_t)(((prefix+t->one_ticks)*1000000u+t->clock_hz-1u)/t->clock_hz);
    for(k=lo;k<=hi;++k) {
        uint64_t a,b,e;
        ultra_build_header_flags(h,ultra_time_add(base,k),l->hdr_flags);
        a=(prefix+((h[7]&1u)?t->one_ticks:t->zero_ticks))*1000000u;b=(uint64_t)k*t->clock_hz;
        e=a>b?a-b:b-a;
        if(e<err){err=e;best=k;}
    }
    ultra_build_header_flags(h,ultra_time_add(base,best),l->hdr_flags);
    return start_us+best;
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
        if ((sl[0] & ULTRA_TYPE_MASK) == ULTRA_SET) {
            off = (int16_t)ultra_get16(sl + ULTRA_PAYLOAD + 25);
            if (off == -32768 || off == 32767) {
                continue;
            }
        } else if (sl[0] == ULTRA_PIXEL_FX) {
            off = (int32_t)ultra_get32(sl + ULTRA_PAYLOAD + 1);
        } else {
            continue;
        }
        d = (int64_t)ultra_udiv64_((uint64_t)e, 1000u) * 1000 + off * 1000 - ((b->f_floor && b->f_start < e) ? e : b->f_start);
        d = d < 0 ? -d : d;
        worst = d > worst ? d : worst;
    }
    return worst;
}

/* Stamp relative starts against TIME, then solve TIME from the actual tick recipe
 * and the trailer's CRC-dependent final HIGH. Recheck the integer timestamp when
 * the CRC's last bit changes. Every supported profile has one fixed bit period;
 * exact-clock sweeps cover normal/update frames, wrap and the <=2 us TIME bound. */
int64_t ultra_sender_finish_(ultra_sender_t *l, int64_t start_us, uint8_t *out)
{
    const uint32_t bytes = ultra_frame_bytes(l->n);
    const ultra_timing_t *tm = &l->timing;
    const uint32_t avg = (uint32_t)((uint64_t)tm->bit_ticks*1000000000u/tm->clock_hz);
    const int equal = 1;
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
        if (s[0] == ULTRA_PIXEL_FX && bit->aux != 0u) {   /* a PIXEL_FX asked to restart: its copies say so */
            s[ULTRA_PX_FLAGS] = (uint8_t)(s[ULTRA_PX_FLAGS] | ULTRA_PXF_RESTART);
            ultra_slot_seal(s);
        }
        b->f_start = it_start(bit);
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
 * rounds above), then its lights' items in order (LIGHT_DEF, LIGHT_OPT, SET, SCENE). A
 * round ends when the position wraps to the start. */
static int take_refresh(ultra_sender_t *l, ultra_sender_board_t *b, uint8_t *s, int64_t now_us)
{
    (void)now_us;
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
            b->f_start = it_start(it);
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
    if (l->hw_dirty) {                           /* the pixel CONFIG pages: one SEQ and 3 copies per changed page */
        l->hw_dirty = 0;
        for (i = 0; i < l->n; i++) {
            ultra_sender_board_t *b = &l->boards[i];
            uint16_t k;
            for (k = b->first; k != END; k = l->items[k].next) {
                ultra_sender_item_t *it = &l->items[k];
                if (KEY_RANK(it->key) == RK_HWCFG && (it->q.flags & IT_HWDIRTY)) {
                    it->q.flags = IT_VALID;
                    {
                        uint16_t j;
                        uint8_t seq = KEY_LIGHT(it->key)==255u ? ++b->cfg_seq : (uint8_t)(it->last_us + 1u);
                        for (j=b->first; j!=END; j=l->items[j].next)
                            if (KEY_RANK(l->items[j].key)==RK_HWCFG && KEY_LIGHT(l->items[j].key)==KEY_LIGHT(it->key))
                                l->items[j].last_us=seq;
                        it->slot[2]=seq;
                    }
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
        b->f_dur = (s[0] & ULTRA_TYPE_MASK) == ULTRA_SET ? ultra_get24(s + ULTRA_PAYLOAD + 22) : 0u;
    }
}

/* the power-off notice: l->off counts its copies down to OFF_QUIET (all out: nothing until the wake); they go
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
    const int64_t wire = l->last_bytes ? l->last_start_us + l->frame_us : INT64_MIN;
    const int64_t held = due > l->hold_us ? due : l->hold_us;
    return held > wire ? held : wire;
}

uint32_t ultra_sender_beacon_frame(ultra_sender_t *l, int64_t now_us, uint8_t *buf)
{
    l->has_sent = 0;                             /* (the refresh interval is the only thing that has_sent gates) */
    return ultra_sender_next_frame(l, now_us, buf);
}

uint32_t ultra_sender_next_frame(ultra_sender_t *l, int64_t now_us, uint8_t *buf)
{
    if (l->update_active==1u || l->tx_active) return 0;
    if (l->profile_training && !l->preamble_left) return 0;
    uint32_t len;
    if (l->n == 0u || l->off == OFF_QUIET) {
        return 0;                                /* deinitialized; after a power-off notice until the wake */
    }
    flush_config(l);
    if ((l->last_bytes && now_us < l->last_start_us + l->frame_us) || now_us < l->hold_us ||
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
            l->ready_us = ultra_wire_end_us(&l->timing,buf,len,now_us);       /* (moved again if this frame is retimed) */
            l->ready_frames = 0;
            l->last_ends_preamble = 1;
            if (l->preamble_full && l->powered) {
                l->synced_once = 1;              /* a full preamble reached the powered boards */
            }
        }
    } else {
        build_frame(l, now_us, buf);
        ultra_sender_finish_(l, now_us, buf);
        if (l->digital && l->ready_us == INT64_MIN) {
            l->ready_us = ultra_wire_end_us(&l->timing,buf,len,now_us);
            l->ready_frames = 0;
        }
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
    if (l->n == 0u || !l->last_bytes || l->tx_active || l->update_active==1u) {
        return 0;
    }
    l->last_start_us = now_us;
    if (l->kind == KIND_ULTRA) {
        ultra_sender_finish_(l, now_us, buf);
        if (l->hdr_flags & ULTRA_HDR_RW) {
            l->hold_us = ultra_sender_rw_hold_(l, buf);
        }
        if (l->last_ends_preamble) {
            l->ready_us = ultra_wire_end_us(&l->timing,buf,l->last_bytes,now_us);       /* the preamble ends when its last frame really goes out */
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

/* everything again as new commands; SETs restart at start_us, pixel effects keep their own start */
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
            if (KEY_RANK(it->key) == RK_PIXFX) {
                it->aux = 0;                         /* (a RESTART is an action, done: the replay is a state) */
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
    if (l->digital) {
        l->preamble_left = 0;                    /* (a digital port needs none) */
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
    if (l->timing.profile != 0u) return ULTRA_ERR_ARG;
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
        if (d->roles[i] > ULTRA_ROLE_WARM_WHITE ||
            (d->roles[i] != ULTRA_ROLE_NONE && (uint16_t)d->base_channel + i >= l->boards[board].channels)) {
            return ULTRA_ERR_ARG;                /* 8-15 reserved */
        }
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_DEF, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    ultra_slot_light_def(s, light, it->slot[2], d->roles, d->gamma_x1000, d->flags, d->min_power, d->max_power, d->base_channel);
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
 * set() / stream(). */
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

int ultra_sender_scene(ultra_sender_t *l, uint16_t board, uint8_t light, uint8_t mode, uint8_t fade_100ms,
                       const ultra_values_t *v)
{
    static const uint16_t zero[5] = { 0, 0, 0, 0, 0 };
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t s[ULTRA_SLOT_BYTES];
    if (!light_ok(l, board, light) || mode > 1u) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    it = item_get(l, b, KEY(light, RK_SCENE, mode));
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

/* ------------------------------------------------------------------------------- local pixel effects */
static int pixel_fx_ok(const ultra_pixel_fx_t *fx)
{
    return fx != 0 && (fx->flags & ~0x7Fu) == 0u;     /* (bit 7 is reserved: the header's builder drops it) */
}

int ultra_sender_pixel_fx_at(ultra_sender_t *l, uint16_t board, uint8_t segment, const ultra_pixel_fx_t *fx,
                             int64_t start_us)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    ultra_pixel_fx_t f;
    if (!board_ok(l, board) || segment > 239u || !pixel_fx_ok(fx)) {
        return ULTRA_ERR_ARG;
    }
    b = &l->boards[board];
    if (b->descriptor) {
        ultra_desc_pixel_t px;
        if (ultra_descriptor_pixel(b->descriptor,(uint8_t)(segment>>4),&px) || !(px.flags&ULTRA_PIXEL_LOCAL) ||
            ((segment&15u)!=15u && (segment&15u)>=px.segments)) return ULTRA_ERR_ARG;
    }
    it = item_get(l, b, KEY(segment, RK_PIXFX, 0));
    if (it == 0) {
        return ULTRA_ERR_FULL;
    }
    f = *fx;
    /* RESTART is an action, not a state: the command's copies carry it, the stored slot (the refresh, a resync's replay)
     * does not, or every refresh would start the effect over */
    it->aux = (f.flags & ULTRA_PXF_RESTART) ? 1u : 0u;
    f.flags = (uint8_t)(f.flags & ~ULTRA_PXF_RESTART);
    ultra_slot_pixel_fx(it->slot, (uint8_t)(segment >> 4), (uint8_t)(segment & 15u), next_seq(b, it), &f);   /* (START: stamped against every frame's TIME) */
    it->q.flags = IT_VALID;
    it_set_start(it, start_us);
    q_put(l, b, &it->q, 0, 1);
    return ULTRA_OK;
}

int ultra_sender_pixel_fx(ultra_sender_t *l, uint16_t board, uint8_t segment, const ultra_pixel_fx_t *fx, int64_t now_us)
{
    if (!board_ok(l, board) || segment > 239u || !pixel_fx_ok(fx)) {
        return ULTRA_ERR_ARG;
    }
    return ultra_sender_pixel_fx_at(l, board, segment, fx, ultra_sender_start_time(l, board, now_us));
}

int64_t ultra_sender_pixel_fx_start(const ultra_sender_t *l, uint16_t board, uint8_t segment)
{
    const ultra_sender_item_t *it;
    if (!board_ok(l, board) || segment > 239u) {
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
    case ULTRA_CFG_PX_LINE_MODE:
        return value <= ULTRA_PXLM_CHAIN;
    case ULTRA_CFG_PX_SMOOTH:
        return value <= ULTRA_PXS_OFF;
    case ULTRA_CFG_PX_SOURCE:
        return value <= 16u;
    case ULTRA_CFG_PX_FPS:
        return 1;
    case ULTRA_CFG_PX_MAX:
    case ULTRA_CFG_PX_MA_CH:
        return value < 65535u;
    case ULTRA_CFG_PX_ORDER:
    case ULTRA_CFG_PX_PIXELS:
    case ULTRA_CFG_PX_WHITE:
    case ULTRA_CFG_PX_VOFFSET:
    case ULTRA_CFG_PX_VLENGTH:
        return 1;
    default:
        return 0;
    }
}

int ultra_config_scope_ok(uint8_t target, uint8_t id)
{
    if (target == 255u) return (id >= 1u && id <= 10u) || (id >= 21u && id <= 25u) || (id >= 33u && id <= 46u);
    if (id == 11u || id == 12u) return target <= 239u;
    return target <= 14u && id >= 64u && id <= 77u;
}

static uint16_t scoped_value(const ultra_sender_t *l,const ultra_sender_board_t *b,uint8_t target,uint8_t id,uint16_t fallback)
{
    uint16_t k;unsigned j;
    for(k=b->first;k!=END;k=l->items[k].next){const ultra_sender_item_t *it=&l->items[k];
        if(KEY_RANK(it->key)!=RK_HWCFG || KEY_LIGHT(it->key)!=target || !(it->q.flags&IT_VALID))continue;
        for(j=0;j<9u;++j)if(it->slot[3u+3u*j]==id)return (uint16_t)ultra_get16(it->slot+4u+3u*j);
    }
    return fallback;
}

int ultra_sender_config_target(ultra_sender_t *l, uint16_t board, uint8_t target, uint8_t id, uint16_t value)
{
    ultra_sender_board_t *b;
    ultra_sender_item_t *it;
    uint8_t ids[9], s[32];
    uint16_t vals[9];
    uint32_t page, n = 0, j;
    if (target == ULTRA_TARGET_BOARD && id < ULTRA_SENDER_CFG_COUNT) return ultra_sender_config(l, board, id, value);
    if (!board_ok(l, board) || !ultra_config_scope_ok(target,id) ||
        !((id == 11u || id == 12u) ? value < 65535u : target==255u ? ultra_config_value_ok(id,value) : ultra_pixel_config_value_ok(id,value))) return ULTRA_ERR_ARG;
    b = &l->boards[board];
    if ((id == 11u || id == 12u) && target >= b->channels) return ULTRA_ERR_ARG;
    if (b->descriptor && ultra_descriptor_config(b->descriptor,target,id,value,&value)) return ULTRA_ERR_ARG;
    if(b->descriptor && id>=64u) {
        ultra_desc_pixel_t px;ultra_desc_link_t link;uint8_t mode;
        if(ultra_descriptor_pixel(b->descriptor,target,&px) || ultra_descriptor_link(b->descriptor,px.control,&link))return ULTRA_ERR_ARG;
        mode=(uint8_t)scoped_value(l,b,target,75,l->digital?1u:2u);
        if(id==66u){
            uint16_t chip=scoped_value(l,b,target,64,0);uint8_t bpp=(uint8_t)(chip==5u?5u:chip==3u || chip==4u?4u:3u);
            if(chip>=16u || !(px.chips&((uint16_t)1u<<chip)))return ULTRA_ERR_ARG;
            if(value>px.max_pixels[bpp-3u])value=px.max_pixels[bpp-3u];
        }
        if(id==75u && link.phy==1u){
            if(l->digital ? value==2u : value==1u || !(link.flags&12u) || !(px.flags&4u) ||
               ((link.flags&8u) && board+1u!=l->n))return ULTRA_ERR_ARG;
        }
        if(id==77u && value) {
            ultra_desc_link_t ingress;uint8_t raw,other;
            if(ultra_descriptor_link(b->descriptor,(uint8_t)(value-1u),&ingress))return ULTRA_ERR_ARG;
            raw=(uint8_t)(ingress.phy==2u || (l->digital && value==px.control+1u));
            if(raw && value==px.control+1u && mode!=1u && !(mode==0u && l->digital))return ULTRA_ERR_ARG;
            if(raw)for(other=0;other<b->descriptor->outputs;++other)
                if(other!=target && scoped_value(l,b,other,77,0)==value)return ULTRA_ERR_ARG;
        }
    }
    page = target==255u ? 3u+(id-33u)/9u : id < 64u ? 0u : 1u + (id - 64u) / 9u;
    it = item_get(l, b, KEY(target, RK_HWCFG, page));
    if (!it) return ULTRA_ERR_FULL;
    if (it->q.flags & IT_VALID) {
        for (j=0; j<9u && it->slot[3u+3u*j]; ++j) {
            ids[n]=it->slot[3u+3u*j]; vals[n++]=(uint16_t)ultra_get16(it->slot+4u+3u*j);
        }
    }
    for (j=0; j<n && ids[j]!=id; ++j) {}
    if (j==n) {
        for (j=n; j && ids[j-1u]>id; --j) { ids[j]=ids[j-1u]; vals[j]=vals[j-1u]; }
        ++n;
    }
    ids[j]=id; vals[j]=value;
    ultra_slot_config(s,target,it->slot[2],ids,vals,n);
    if ((it->q.flags & IT_VALID) && memcmp(s+3,it->slot+3,27)==0) return ULTRA_OK;
    copy32(it->slot,s); it->q.flags=IT_VALID|IT_HWDIRTY;
    it_set_start(it,ULTRA_NO_START); l->hw_dirty=1;
    return ULTRA_OK;
}

int ultra_sender_pixel_config(ultra_sender_t *l, uint16_t board, uint8_t output, uint8_t id, uint16_t value)
{
    if (output > ULTRA_PIXEL_OUTPUT_MAX || !ultra_pixel_config_value_ok(id,value)) return ULTRA_ERR_ARG;
    return ultra_sender_config_target(l,board,output,id,value);
}

/* ------------------------------------------------------------------------------- the line mode */
int ultra_pixel_line_mode_ok(uint8_t mode, int digital_line, uint16_t board, uint16_t boards)
{
    if (mode > ULTRA_PXLM_CHAIN || boards == 0u || board >= boards) {
        return 0;
    }
    if (digital_line) {
        return mode != ULTRA_PXLM_CHAIN && boards == 1u;   /* one receiver in the strip's data path */
    }
    return mode != ULTRA_PXLM_STREAM;                     /* any position: it holds its slot and forwards the rest */
}

uint8_t ultra_pixel_transport(uint8_t mode, int digital_line)
{
    if (mode == ULTRA_PXLM_STREAM) {
        return ULTRA_PXT_DIGITAL;
    }
    if (mode == ULTRA_PXLM_CHAIN) {
        return ULTRA_PXT_CHAIN;
    }
    return digital_line ? ULTRA_PXT_DIGITAL : ULTRA_PXT_CHAIN;   /* AUTO: what the line is */
}

int ultra_sender_pixel_line_mode(ultra_sender_t *l,uint16_t board,uint8_t output,uint8_t mode)
{
    if(!board_ok(l,board))return ULTRA_ERR_ARG;
    if(!l->boards[board].descriptor && !ultra_pixel_line_mode_ok(mode,l->digital,board,l->n))return ULTRA_ERR_ARG;
    return ultra_sender_pixel_config(l,board,output,ULTRA_CFG_PX_LINE_MODE,mode);
}

/* The values each board CONFIG id takes, lo..hi. A protocol release that adds values
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
    { 0, 3 },                                    /*  8: signal loss (3 = scene) */
    { 5, 600 },                                  /*  9: signal loss timeout */
    { 0, 2 },                                    /* 10: feedback channel */
    { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 },   /* 11-15: LED current, mA */
    { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 }, { 0, 65535 },   /* 16-20: channel limits, mA */
    { 0, 65535 },                                /* 21: board limit, mA */
    { 0, 3 },                                    /* 22: boot protocol */
    { 0, 1 },                                    /* 23: soft start */
    { 0, 65535 },                                /* 24: retry policy: count | interval << 8, every value */
};

int ultra_config_value_ok(uint8_t id, uint16_t value)
{
    if (!ultra_config_scope_ok(ULTRA_TARGET_BOARD,id)) {
        return 0;
    }
    if ((id == ULTRA_CFG_BOARD_MAX || (id >= 33u && id <= 46u)) && value == 65535u) return 0;
    if (id == ULTRA_CFG_SKIP_RATE) return value == ULTRA_SKIP_HZ_DEFAULT || value == ULTRA_SKIP_HZ_SLOW;
    if (id >= sizeof cfg_range / sizeof cfg_range[0]) {
        return 1;
    }
    return value >= cfg_range[id][0] && value <= cfg_range[id][1];
}

int ultra_sender_config(ultra_sender_t *l, uint16_t board, uint8_t id, uint16_t value)
{
    uint32_t i;
    if ((board != ULTRA_SENDER_ALL && !board_ok(l, board)) || !ultra_config_value_ok(id, value)) {
        return ULTRA_ERR_ARG;
    }
    if (id>=33u) {
        uint32_t needed=0;
        for(i=0;i<l->n;++i) {
            uint16_t applied;
            ultra_sender_board_t *b=&l->boards[i];
            if(board!=ULTRA_SENDER_ALL && board!=i)continue;
            if(b->descriptor && ultra_descriptor_config(b->descriptor,255,id,value,&applied))return ULTRA_ERR_ARG;
            if(!item_find(l,b,KEY(255,RK_HWCFG,3u+(id-33u)/9u)))++needed;
        }
        if(needed>items_free(l))return ULTRA_ERR_FULL;
        for(i=0;i<l->n;++i)if(board==ULTRA_SENDER_ALL || board==i)
            (void)ultra_sender_config_target(l,(uint16_t)i,255,id,value);
        return ULTRA_OK;
    }
    for (i=0;i<l->n;++i) {
        uint16_t applied;
        if ((board==ULTRA_SENDER_ALL || board==i) && l->boards[i].descriptor &&
            ultra_descriptor_config(l->boards[i].descriptor,255,id,value,&applied)) return ULTRA_ERR_ARG;
    }
    for (i = 0; i < l->n; i++) {
        uint16_t applied=value;
        ultra_sender_board_t *b = &l->boards[i];
        if (board != ULTRA_SENDER_ALL && board != i) {
            continue;
        }
        if (b->descriptor) (void)ultra_descriptor_config(b->descriptor,255,id,value,&applied);
        if (cfg_has(b, id) && b->cfg[id] == applied) {
            continue;
        }
        b->cfg[id] = applied;
        b->cfg_have[id >> 5] |= BIT32(id & 31u);
        b->cfg_dirty = 1;
    }
    return ULTRA_OK;
}

uint16_t ultra_sender_config_crc(const ultra_sender_t *l,uint16_t board)
{
    uint32_t target,id;uint16_t crc=65535;uint8_t record[4];
    const ultra_sender_board_t *b;
    if(!l || board>=l->n)return 0;
    b=&l->boards[board];
    for(target=0;target<=255u;++target)for(id=1;id<=77u;++id) {
        uint16_t k,value=0;int have=0;
        if(target==255u && id<ULTRA_SENDER_CFG_COUNT && cfg_has(b,id)){value=b->cfg[id];have=1;}
        else if(ultra_config_scope_ok((uint8_t)target,(uint8_t)id))
            for(k=b->first;k!=END;k=l->items[k].next){const ultra_sender_item_t *it=&l->items[k];unsigned j;
                if(KEY_RANK(it->key)!=RK_HWCFG || KEY_LIGHT(it->key)!=target || !(it->q.flags&IT_VALID))continue;
                for(j=0;j<9u;++j)if(it->slot[3u+3u*j]==id){value=(uint16_t)ultra_get16(it->slot+4u+3u*j);have=1;}
            }
        if(have){record[0]=(uint8_t)target;record[1]=(uint8_t)id;ultra_put16(record+2,value);crc=ultra_crc16_update(crc,record,4);}
    }
    return crc;
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
        *period = (uint16_t)(((steps < 1440u ? 1440u : steps > 65535u ? 65535u : steps) * 25u + 18u) / 36u);
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
    if(type==ULTRA_CONFIG && light!=ULTRA_TARGET_BOARD){
        uint16_t k;
        if(light>239u)return ULTRA_ERR_ARG;
        for(k=b->first;k!=END;k=l->items[k].next)
            if(KEY_RANK(l->items[k].key)==RK_HWCFG && KEY_LIGHT(l->items[k].key)==light &&
               (l->items[k].q.flags&IT_VALID))return ULTRA_ERR_ARG;
        it=item_get(l,b,KEY(light,RK_HWCFG,0));
        if(!it)return ULTRA_ERR_FULL;
        it->last_us=(uint8_t)(seq-1u);return ULTRA_OK;
    }
    if (light == ULTRA_TARGET_BOARD) {
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
    case ULTRA_SCENE: rk = RK_SCENE; break;
    case ULTRA_PIXEL_FX: rk = RK_PIXFX; break;
    default: return ULTRA_ERR_ARG;
    }
    if (type==ULTRA_PIXEL_FX ? light>239u : !light_ok(l,board,light)) return ULTRA_ERR_ARG;
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
    d->base_channel = 0;
    uint32_t i;
    for (i = 0; i < 5u; i++) {
        d->roles[i] = ULTRA_ROLE_NONE;
        d->min_power[i] = 0;
        d->max_power[i] = 65535;
    }
    d->gamma_x1000 = 2800;
    d->flags = 0;
}
