/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The sender library's frames through the board firmware's own receive path, on a PC: every board runs
 * frame_rx.c / proto.c (its slot from the start of what reaches it, the header trailer from the frame end, the
 * protocol lock, its forwarding gate plan) and its light engine ultra.c (clock sync, SEQ filter, transitions,
 * effects), each with its own oscillator error (HSI +-2 %) and tick phase, as the firmware's main.c does.
 *
 * The chain: each board forwards what its gate plan passes (Ultra: everything after its 256 bits; not locked:
 * nothing; WS2805: everything after its 40 bits), and the plan a frame gives takes effect one frame later (main.c:
 * the frame-end ISR re-arms the gate with the plan the main loop published for the frame before).
 *
 * Scenarios:
 *  1. every command type on 1-64 boards, 20 % of the frames lost and 5 % of the slots corrupted per board: after
 *     the queues and the refresh, every board holds exactly what the host meant;
 *  2. a chain of new boards (never locked) and boards still locked to WS2805: the preamble locks every one, none
 *     shows a colour meanwhile, and the first command reaches every board;
 *  3. effects and transitions in step: two lines in one group (50 x 5 and 10 x 1 lights), started by calls over
 *     8 ms with frame loss: one breathe phase, one strobe, one SET ramp on every board;
 *  4. boards power-cycled while the line runs: the refresh brings back their state and their effects' phase;
 *  5. a host restart (its clock starts again): the boards take the new host's commands;
 *  6. a host-side effect streamed to 10 x 5 lights, then stopped: every light ends at its last value;
 *  7. (review round 7) a fade queued during the start-up preamble starts from dark on every board; TIME's 32-bit wrap
 *     and SEQ 255 -> 0 while the boards run; every board effect 1-9 with its parameters; without loss every light has
 *     its first copy before the group's start (light counts raised at run time).
 *
 * Build: python sender/test/run_tests.py (needs the board firmware's src/core).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_rx.h"
#include "ultra.h"
#include "ultra_sender.h"

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

#define MAXB    64u
#define ITEMS   (MAXB * 5u * 14u)
#define GAP_US  200.0                            /* the host's loop between a frame's reset and the next frame */

static const uint16_t Z5[5] = { 0, 0, 0, 0, 0 };
static const uint16_t ONE5[5] = { 65535, 65535, 65535, 65535, 65535 };

/* ------------------------------------------------------------------------------------------------- random */
static uint32_t rng_state = 1;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static double uni(void) { return (double)(rnd() >> 8) / 16777216.0; }

/* ------------------------------------------------------------------------------------------------- a board */
typedef struct {
    frx_t rx;
    ultra_t u;
    gate_plan_t plan, plan_buf;                  /* the gate's plan for this frame; the one published for the next */
    double drift, phase;
    uint32_t frames, slots, ws_shown, ws_lit;
} board_t;

static uint32_t tick_of(const board_t *bd, double real_us)
{
    return (uint32_t)(uint64_t)((real_us * (1.0 + bd->drift) + bd->phase) / 50.0);
}

static void board_boot(board_t *bd, proto_t sticky, uint32_t seed)
{
    frx_init(&bd->rx, PMODE_AUTO, sticky);
    ultra_init(&bd->u, seed);
    bd->plan = bd->plan_buf = proto_gate_plan(proto_active(&bd->rx.ps));
}

/* ------------------------------------------------------------------------------------------------- a line */
typedef struct {
    ultra_sender_t s;
    ultra_sender_board_t sb[MAXB];
    ultra_sender_item_t it[ITEMS];
    board_t bd[MAXB];
    uint16_t n;
    uint8_t f[ULTRA_SENDER_FRAME_BYTES(MAXB)];
    double now;                                  /* real time, us */
    double host_off;                             /* host clock = real time - host_off */
    uint32_t frame_id, sent;
    double loss, corrupt;                        /* per board and frame: missed, own slot corrupted */
} line_t;

static line_t A, C;

static int64_t host(const line_t *p) { return (int64_t)(p->now - p->host_off); }

static void line_init(line_t *p, uint16_t n, uint8_t lights, uint32_t seed, double drift_max)
{
    ultra_sender_config_t c;
    uint16_t b;
    ultra_sender_config_init(&c, n, lights);
    c.seed = seed;
    CHECK(ultra_sender_init(&p->s, &c, p->sb, p->it, ITEMS) == ULTRA_OK, "init");
    p->n = n;
    p->frame_id = p->sent = 0;
    p->loss = p->corrupt = 0;
    for (b = 0; b < n; b++) {
        board_t *bd = &p->bd[b];
        memset(bd, 0, sizeof *bd);
        board_boot(bd, PROTO_ULTRA, 1000u + b + seed);   /* boards that ran Ultra before (sticky) */
        bd->drift = (uni() * 2.0 - 1.0) * drift_max;
        bd->phase = uni() * 50.0;
    }
}

/* The frame in p->f (len bytes) ended at real time end_us: down the chain, each board decodes what reaches it and
 * forwards what its gate passes. */
static void deliver(line_t *p, uint32_t len, double end_us)
{
    const uint8_t *in = p->f;
    uint32_t bits = len * 8u, k;
    p->frame_id++;
    for (k = 0; k < p->n && bits != 0u; k++) {
        board_t *bd = &p->bd[k];
        const gate_plan_t g = bd->plan;
        uint32_t fwd_off = 0, fwd_bits = 0;
        /* what its gate passes (the plan armed for this frame) */
        if (g.start_open && g.t1 == 0u) {
            fwd_bits = bits;
        } else if (!g.start_open && g.t1 != 0u && g.t2 == 0u && bits > g.t1) {
            fwd_off = g.t1;
            fwd_bits = bits - g.t1;
        } else if (g.start_open && g.t1 != 0u) {
            CHECK(0, "a board locked to WS2915 (not modelled)");
        }
        if (uni() >= p->loss) {
            uint8_t cap[PROTO_BITS_BUF + 8];
            slot_t ws;
            frx_end_t e;
            const uint32_t own = g.own_bits, nb = bits < own ? bits : own;
            const uint8_t *tail;
            uint32_t tk, i;
            memset(cap, 0, sizeof cap);
            memcpy(cap, in, (nb + 7u) / 8u);
            if (uni() < p->corrupt) {
                const uint32_t bit = rnd() % 256u;
                cap[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7u));
            }
            memset(&ws, 0, sizeof ws);
            if (frx_capture(&bd->rx, p->frame_id, proto_active(&bd->rx.ps), cap, nb, own, 0, &ws) & FRX_SHOW_WS) {
                bd->ws_shown++;
                for (i = 0; i < 5u; i++) {
                    bd->ws_lit += ws.ch[i] != 0u;
                }
            }
            tail = frx_tail_wanted(&bd->rx, bits) ? in + bits / 8u - ULTRA_HDR_BYTES : 0;
            frx_frame_end(&bd->rx, p->frame_id, bits, tail, &e);
            if (e.show_ws) {
                bd->ws_shown++;
                for (i = 0; i < 5u; i++) {
                    bd->ws_lit += bd->rx.ws.ch[i] != 0u;
                }
            }
            tk = tick_of(bd, end_us + uni() * 50.0);
            if (e.ultra_sync) {
                ultra_on_header(&bd->u, tk, e.host_us);
                bd->frames++;
                if (e.ultra_slot) {
                    ultra_apply_slot(&bd->u, bd->rx.slot, tk);
                    bd->slots++;
                }
            }
            bd->plan = bd->plan_buf;             /* the frame-end ISR re-arms with the plan published before */
            bd->plan_buf = e.plan;               /* the main loop publishes this frame's plan */
        } else {
            bd->plan = bd->plan_buf;             /* (the ISR still sees the frame end) */
        }
        in += fwd_off / 8u;
        bits = fwd_bits;
    }
}

/* the host's loop once: a frame if one is due (back to back while busy), else time moves to the next due */
static void step(line_t *p, double until)
{
    const uint32_t len = ultra_sender_next_frame(&p->s, host(p), p->f);
    if (len != 0u) {
        const double end = (double)ultra_sender_last_time_us(&p->s) + p->host_off;
        deliver(p, len, end);
        p->sent++;
        p->now = end + p->s.timing.reset_ns / 1000.0 + GAP_US;
    } else {
        double due = (double)ultra_sender_due_us(&p->s) + p->host_off;
        if (due <= p->now) {
            due = p->now + 100.0;
        }
        p->now = due < until ? due : until;
    }
}

static void run(line_t *p, double ms)
{
    const double until = p->now + ms * 1000.0;
    while (p->now < until) {
        step(p, until);
    }
}

static void run2(line_t *a, line_t *c, double ms)
{
    const double until = (a->now > c->now ? a->now : c->now) + ms * 1000.0;
    while (a->now < until || c->now < until) {
        if (a->now <= c->now && a->now < until) {
            step(a, until);
        } else if (c->now < until) {
            step(c, until);
        } else {
            step(a, until);
        }
    }
}

static void drain(line_t *p)
{
    int i;
    for (i = 0; i < 200000 && ultra_sender_busy(&p->s); i++) {
        step(p, p->now + 1e9);
    }
}

static void render(board_t *bd, double real_us, uint16_t *out)
{
    ultra_render(&bd->u, tick_of(bd, real_us), out);
}

/* ------------------------------------------------------------------------------------------------- 1. every command */
static const uint8_t RGBWW[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                  ULTRA_ROLE_COLD_WHITE };

/* a valid value per id and board (ultra_config_value_ok): the first of a few candidates inside the id's range;
 * 0.2.5's values spread over the boards: signal loss 0-3 (3 = scene), the retry policy (count, interval) */
static uint16_t cfg_val(uint8_t id, uint16_t b)
{
    if (id == ULTRA_CFG_LOSS) {
        return (uint16_t)(b % 4u);
    }
    if (id == ULTRA_CFG_RETRY) {
        return ULTRA_SENDER_RETRY(b * 37u + 1u, b % 7u);
    }
    const uint16_t v = (uint16_t)((id * 37u + b) % 97u + 1u);
    const uint16_t cand[5] = { v, (uint16_t)(1440u + v), (uint16_t)(v % 3u), (uint16_t)(v % 2u), (uint16_t)(5u + v % 96u) };
    uint32_t i;
    for (i = 0; i < 5u; i++) {
        if (ultra_config_value_ok(id, cand[i])) {
            return cand[i];
        }
    }
    return 0;
}

static void steps8(ultra_strobe_step_t *st)
{
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        st[i].duration_ms = (uint16_t)(50u + 10u * i);
        st[i].transition_ms = (uint16_t)(i * 3u);
        st[i].v.m = (uint16_t)(8000u * i + 100u);
        memcpy(st[i].v.a, ONE5, sizeof st[i].v.a);
        st[i].v.a[4] = 5;
        memcpy(st[i].v.b, ONE5, sizeof st[i].v.b);
    }
}

static int check_board(const ultra_t *u, uint16_t b, uint8_t nl, int *line_no)
{
    uint8_t id, l;
    int i;
#define OK(c) \
    do { \
        if (!(c)) { \
            *line_no = __LINE__; \
            return 0; \
        } \
    } while (0)
    for (id = 1; id < ULTRA_CFG_COUNT; id++) {
        OK((u->cfg_valid & (1u << id)) && u->cfg[id] == cfg_val(id, b));
    }
    for (l = 0; l < ULTRA_LIGHTS; l++) {
        const ultra_light_t *L = &u->light[l];
        uint8_t fx;
        if (l >= nl) {                           /* cleared: an empty LIGHT_DEF, SCENE 0, no power-on effect */
            OK(L->defined && L->mask == 0u && L->scene_mode == 0u && L->scene_fx == ULTRA_FX_NONE &&
               L->fx == ULTRA_FX_NONE);
            continue;
        }
        OK(L->defined && L->mask == (nl == 1u ? 0x1Fu : 1u << l));
        OK(L->g_q16 == ((2200u + 100u * l) * 65536u + 500u) / 1000u);
        OK(L->flags == (nl == 1u ? ULTRA_DEF_CONSTANT_BRIGHTNESS : 0u) && L->min_p[2] == 300u && L->max_p[4] == 64000u);
        OK(L->opt_flags == (nl == 1u ? (ULTRA_OPT_DIM_TO_WARM | ULTRA_OPT_POINTS) : 0u) && L->opt_n == (nl == 1u ? 3u : 0u));
        if (nl == 1u) {
            OK(L->opt_pm[0] == 60000u && L->opt_pt[2] == 5556u && L->opt_warm == 5556u && L->opt_assist == 20000u);
        }
        OK(L->scene_mode == 1u && L->scene_fade == 7u && L->scene_v.m == 30000u + b && L->scene_v.a[4] == 55u &&
           L->scene_v.b[2] == 60000u);
        OK(L->scene_fx == ULTRA_FX_CANDLE && ultra_get16(L->scene_fxp) == 26214u && ultra_get16(L->scene_fxp + 2) == 120u);
        OK(L->cur.to.m == 40000u + l && L->cur.to.a[1] == 30000u && L->cur.smooth == 1u);
        fx = l == 0u ? ULTRA_FX_BREATHE : l == 1u ? ULTRA_FX_STROBE : l == 2u ? ULTRA_FX_CANDLE
           : l == 3u ? ULTRA_FX_PULSE : ULTRA_FX_COLOR_LOOP;
        OK(L->fx == fx && !L->pend);
        if (fx == ULTRA_FX_STROBE) {
            OK(L->step_count == 8u && L->step_have == 0xFFu && L->step_seq == L->fx_seq);
            for (i = 0; i < 8; i++) {
                OK(L->step[i].dur == 50u + 10u * (unsigned)i && L->step[i].trans == (unsigned)i * 3u &&
                   L->step[i].v.m == 8000u * (unsigned)i + 100u && L->step[i].v.a[4] == 5u);
            }
        }
    }
#undef OK
    return 1;
}

static int check_line(line_t *p, const char *what, int verbose)
{
    uint16_t b;
    int bad = 0;
    for (b = 0; b < p->n; b++) {
        uint16_t out[5];
        int ln = 0;
        render(&p->bd[b], p->now, out);         /* (an effect switches at its start time, in the render) */
        if (!check_board(&p->bd[b].u, b, (uint8_t)(b % 5u + 1u), &ln)) {
            if (verbose && bad < 3) {
                printf("  %s: board %u differs from the host's state at line %d (frames %u, slots %u)\n", what, b, ln,
                       p->bd[b].frames, p->bd[b].slots);
            }
            bad++;
        }
    }
    return bad;
}

/* the refresh at rest until every board holds the host's state (at most max_s): the seconds it took, -1 = never */
static double converge(line_t *p, const char *what, double max_s)
{
    double t;
    for (t = 0; t < max_s; t += 1.0) {
        if (check_line(p, what, 0) == 0) {
            return t;
        }
        run(p, 1000);
    }
    return check_line(p, what, 1) == 0 ? t : -1.0;
}

static void every_command(uint16_t n, double loss, double corrupt)
{
    line_t *p = &A;
    ultra_strobe_step_t st[8];
    uint8_t prm[ULTRA_FX_PARAMS];
    uint16_t b;
    uint8_t id, l;
    double t1, t2;
    uint32_t frames0, frames1, ident = 0;
    line_init(p, n, 5, 77u + n, 0.02);
    p->now = 5e9;                                /* 5000 s: the 32-bit TIME wrapped */
    p->host_off = 0;
    p->loss = loss;
    p->corrupt = corrupt;
    for (b = 0; b < n; b++) {
        if (b % 7u == 3u) {
            board_boot(&p->bd[b], PROTO_NONE, 99u + b);   /* a new board, never locked */
        }
        for (id = 1; id < ULTRA_CFG_COUNT; id++) {
            ultra_sender_config(&p->s, b, id, cfg_val(id, b));
        }
    }
    steps8(st);
    for (b = 0; b < n; b++) {
        const uint8_t nl = (uint8_t)(b % 5u + 1u);
        ultra_sender_set_lights(&p->s, b, nl);   /* the others are cleared */
        for (l = 0; l < nl; l++) {
            ultra_light_def_t d;
            ultra_light_opt_t o;
            ultra_values_t v;
            static const uint16_t pm[3] = { 60000, 20000, 655 }, pt[3] = { 3704, 4545, 5556 };
            ultra_light_def_init(&d);
            if (nl == 1u) {
                memcpy(d.roles, RGBWW, 5);
            } else {
                d.roles[l] = ULTRA_ROLE_BRIGHTNESS;
            }
            d.gamma_x1000 = (uint16_t)(2200u + 100u * l);
            d.flags = nl == 1u ? ULTRA_DEF_CONSTANT_BRIGHTNESS : 0u;
            {
                static const uint16_t mn[5] = { 100, 200, 300, 400, 500 }, mx[5] = { 60000, 61000, 62000, 63000, 64000 };
                memcpy(d.min_power, mn, sizeof mn);
                memcpy(d.max_power, mx, sizeof mx);
            }
            CHECK(ultra_sender_light_def(&p->s, b, l, &d) == ULTRA_OK, "light_def");
            memset(&o, 0, sizeof o);
            o.flags = nl == 1u ? (ULTRA_OPT_DIM_TO_WARM | ULTRA_OPT_POINTS) : 0u;
            o.cw_mired10 = 1538;
            o.ww_mired10 = 3704;
            o.warm_mired10 = 5556;
            o.red_assist = 20000;
            o.points = nl == 1u ? 3u : 0u;
            memcpy(o.pt_m, pm, sizeof pm);
            memcpy(o.pt_mired10, pt, sizeof pt);
            CHECK(ultra_sender_light_opt(&p->s, b, l, &o) == ULTRA_OK, "light_opt");
            v.m = (uint16_t)(30000u + b);
            {
                static const uint16_t sa[5] = { 11, 22, 33, 44, 55 }, sb[5] = { 65535, 65535, 60000, 65535, 65535 };
                memcpy(v.a, sa, sizeof sa);
                memcpy(v.b, sb, sizeof sb);
            }
            CHECK(ultra_sender_scene(&p->s, b, l, 1, 7, &v) == ULTRA_OK, "scene");
            CHECK(ultra_sender_scene_effect(&p->s, b, l, ULTRA_FX_CANDLE, prm, ultra_fx_candle(prm, 26214, 120)) ==
                  ULTRA_OK, "scene_effect");
            v.m = (uint16_t)(40000u + l);
            {
                static const uint16_t a[5] = { 65535, 30000, 20000, 10000, 5000 };
                memcpy(v.a, a, sizeof a);
                memcpy(v.b, ONE5, sizeof v.b);
            }
            CHECK(ultra_sender_set(&p->s, b, l, &v, 500, ULTRA_SET_SMOOTH, host(p)) == ULTRA_OK, "set");
        }
        ultra_sender_identify(&p->s, b, ULTRA_IDENT_VERSION, 0);
    }
    drain(p);
    /* effects after the state: breathe, an 8-step strobe, candle, pulse, colour loop */
    for (b = 0; b < n; b++) {
        const uint8_t nl = (uint8_t)(b % 5u + 1u);
        CHECK(ultra_sender_effect(&p->s, b, 0, ULTRA_FX_BREATHE, prm, ultra_fx_breathe(prm, 3000, 1000, 65535), host(p))
              == ULTRA_OK, "breathe");
        if (nl > 1u) {
            CHECK(ultra_sender_strobe(&p->s, b, 1, st, 8, host(p)) == ULTRA_OK, "strobe");
        }
        if (nl > 2u) {
            ultra_sender_effect(&p->s, b, 2, ULTRA_FX_CANDLE, prm, ultra_fx_candle(prm, 30000, 100), host(p));
        }
        if (nl > 3u) {
            ultra_sender_effect(&p->s, b, 3, ULTRA_FX_PULSE, prm, ultra_fx_pulse(prm, 300, 300, 600, 0, 65535),
                                host(p));
        }
        if (nl > 4u) {
            ultra_sender_effect(&p->s, b, 4, ULTRA_FX_COLOR_LOOP, prm, ultra_fx_color_loop(prm, 10000, 65535),
                                host(p));
        }
    }
    drain(p);
    frames0 = p->sent;
    for (b = 0; b < n; b++) {
        ident += p->bd[b].u.identify != ULTRA_IDENT_VERSION;   /* (3 copies, not refreshed: one-shot actions) */
    }
    /* refresh cycles: the boards that lost copies catch up */
    t1 = converge(p, "after the refresh", 180.0);
    frames1 = p->sent;
    /* a signal loss off / fade off on every board (main.c's signal_loss_check: ultra_all_off), then frames again */
    for (b = 0; b < n; b++) {
        board_t *bd = &p->bd[b];
        ultra_all_off(&bd->u, tick_of(bd, p->now + 4e6), b & 1u ? 1000u : 0u);
    }
    p->now += 6e6;
    t2 = converge(p, "after a signal loss", 180.0);
    /* P(3 copies lost) at 24 % per board and frame: 1.4 % */
    CHECK(t1 >= 0 && t2 >= 0 && ident <= 2u + n / 20u, "%u boards, %.0f %% loss: every board at the host's state after "
          "%.0f s of refresh, after a signal loss %.0f s; IDENTIFY missed by %u", n, loss * 100.0, t1, t2, ident);
    printf("  %2u boards, %2.0f %% frames lost + %2.0f %% slots corrupted: queues out in %4u frames, every board at the "
           "host's state after %3.0f s of refresh (%3u frames); after a signal loss off: %3.0f s. IDENTIFY (not "
           "refreshed) missed by %u\n", n, loss * 100.0, corrupt * 100.0, frames0, t1, frames1 - frames0, t2, ident);
}

/* ------------------------------------------------------------------------------------------------- 2. lock */
static void lock_chain(uint16_t n, int with_ws)
{
    line_t *p = &A;
    uint16_t b;
    uint32_t ws_lit = 0, ws_shown = 0, got = 0, locked_frame = 0, i;
    line_init(p, n, 1, 300u + n, 0.02);
    p->now = 1e6;
    for (b = 0; b < n; b++) {
        board_boot(&p->bd[b], with_ws && (b % 3u == 1u) ? PROTO_WS2805 : PROTO_NONE, 7u + b);
    }
    for (b = 0; b < n; b++) {
        ultra_light_def_t d;
        ultra_values_t v;
        ultra_light_def_init(&d);
        d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
        d.gamma_x1000 = 1000;
        ultra_sender_light_def(&p->s, b, 0, &d);
        v.m = (uint16_t)(1000u + b);
        memcpy(v.a, ONE5, sizeof v.a);
        memcpy(v.b, ONE5, sizeof v.b);
        ultra_sender_set(&p->s, b, 0, &v, 0, 0, host(p));
    }
    for (i = 0; i < ultra_preamble_frames(n); i++) {
        step(p, 1e18);
        if (!locked_frame) {
            uint32_t all = 1;
            for (b = 0; b < n; b++) {
                all &= proto_active(&p->bd[b].rx.ps) == PROTO_ULTRA;
            }
            locked_frame = all ? i + 1u : 0u;
        }
    }
    drain(p);
    for (b = 0; b < n; b++) {
        ws_lit += p->bd[b].ws_lit;
        ws_shown += p->bd[b].ws_shown;
        got += p->bd[b].u.light[0].cur.to.m == 1000u + b && p->bd[b].u.light[0].defined;
    }
    CHECK(locked_frame != 0u && got == n && ws_lit == 0u,
          "%u boards (%s): locked after %u of %u preamble frames, %u got their state, %u WS channels lit", n,
          with_ws ? "new + WS2805" : "new", locked_frame, ultra_preamble_frames(n), got, ws_lit);
    printf("  preamble, %2u %s boards: all locked after %3u of %3u frames, %u / %u got the first command, WS slots shown "
           "%u (lit channels %u)\n", n, with_ws ? "new + WS2805" : "new", locked_frame, ultra_preamble_frames(n), got,
           n, ws_shown, ws_lit);
}

/* ------------------------------------------------------------------------------------------------- 3. in step */
static void def_lights(line_t *p, uint8_t lights)
{
    uint16_t b;
    uint8_t l;
    for (b = 0; b < p->n; b++) {
        for (l = 0; l < lights; l++) {
            ultra_light_def_t d;
            ultra_values_t v;
            ultra_light_def_init(&d);
            d.roles[l] = ULTRA_ROLE_BRIGHTNESS;
            d.gamma_x1000 = 1000;                /* linear: the output is M */
            ultra_sender_light_def(&p->s, b, l, &d);
            v.m = 65535;
            memcpy(v.a, ONE5, sizeof v.a);
            memcpy(v.b, ONE5, sizeof v.b);
            ultra_sender_set(&p->s, b, l, &v, 0, 0, host(p));
        }
    }
}

/* which lights count: the ones that have the command under test */
typedef int (*has_fn)(const ultra_light_t *L);
static has_fn has_cmd;
static uint32_t have_n;                          /* lights that had it at the last spread() */

/* the spread of the outputs of the lights (of both lines) that have the command, at real time t */
static uint32_t spread(double t, uint8_t la, uint8_t lc, uint16_t *lo_out)
{
    uint16_t lo = 65535, hi = 0, out[5];
    uint16_t b;
    uint8_t l;
    have_n = 0;
    for (b = 0; b < A.n + C.n; b++) {
        board_t *bd = b < A.n ? &A.bd[b] : &C.bd[b - A.n];
        const uint8_t lights = b < A.n ? la : lc;
        render(bd, t, out);
        for (l = 0; l < lights; l++) {
            if (has_cmd != 0 && !has_cmd(&bd->u.light[l])) {
                continue;
            }
            have_n++;
            lo = out[l] < lo ? out[l] : lo;
            hi = out[l] > hi ? out[l] : hi;
        }
    }
    if (lo_out) {
        *lo_out = lo;
    }
    return hi >= lo ? (uint32_t)(hi - lo) : 0u;
}

static int has_breathe(const ultra_light_t *L) { return L->fx == ULTRA_FX_BREATHE || (L->pend && L->pend_fx == ULTRA_FX_BREATHE); }
static int has_strobe(const ultra_light_t *L) { return L->fx == ULTRA_FX_STROBE && L->step_have == 0x07u; }
static int has_ramp(const ultra_light_t *L) { return L->cur.to.m == 0u; }

/* both lines run on; every `every_ms` the spread of all lights at that moment: the worst, and how often 0 */
static uint32_t have_min;                        /* the fewest lights that had the command at a sample */
static double measure(double every_ms, uint32_t count, uint32_t *equal)
{
    double worst = 0;
    uint32_t k;
    have_min = 1000000u;
    for (k = 0; k < count; k++) {
        double t;
        uint32_t sp;
        run2(&A, &C, every_ms);
        t = A.now > C.now ? A.now : C.now;
        sp = spread(t, 5, 1, 0);
        worst = sp > worst ? sp : worst;
        have_min = have_n < have_min ? have_n : have_min;
        if (equal != 0) {
            *equal += sp == 0u;
        }
    }
    return worst;
}

/* calls over 8 ms: lights 0-1 (and the other line) at t0, 2-3 at +4 ms, 4 at +8 ms, as one Home Assistant change */
typedef int (*cmd_fn)(line_t *p, uint16_t b, uint8_t l);

static void calls(cmd_fn f)
{
    static const struct {
        double at;
        uint8_t first, last;
    } cl[] = { { 0, 0, 1 }, { 4000, 2, 3 }, { 8000, 4, 4 } };
    const double t0 = (A.now > C.now ? A.now : C.now);
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        line_t *ps[2] = { &A, &C };
        uint32_t k;
        while (A.now < t0 + cl[i].at) {
            step(&A, t0 + cl[i].at);
        }
        while (C.now < t0 + cl[i].at) {
            step(&C, t0 + cl[i].at);
        }
        for (k = 0; k < 2u; k++) {
            line_t *p = ps[k];
            const uint8_t lights = p == &A ? 5u : 1u;
            uint16_t b;
            uint8_t l;
            for (b = 0; b < p->n; b++) {
                for (l = cl[i].first; l <= cl[i].last && l < lights; l++) {
                    CHECK(f(p, b, l) == ULTRA_OK, "command");
                }
            }
        }
    }
}

static int cmd_breathe(line_t *p, uint16_t b, uint8_t l)
{
    uint8_t prm[ULTRA_FX_PARAMS];
    return ultra_sender_effect(&p->s, b, l, ULTRA_FX_BREATHE, prm, ultra_fx_breathe(prm, 2000, 0, 65535), host(p));
}

static ultra_strobe_step_t strobe_st[3];
static int cmd_strobe(line_t *p, uint16_t b, uint8_t l)
{
    return ultra_sender_strobe(&p->s, b, l, strobe_st, 3, host(p));
}

static int cmd_stop(line_t *p, uint16_t b, uint8_t l)
{
    return ultra_sender_effect(&p->s, b, l, ULTRA_FX_NONE, 0, 0, host(p));
}

static int cmd_on(line_t *p, uint16_t b, uint8_t l)
{
    ultra_values_t v;
    v.m = 65535;
    memcpy(v.a, ONE5, sizeof v.a);
    memcpy(v.b, ONE5, sizeof v.b);
    return ultra_sender_set(&p->s, b, l, &v, 0, 0, host(p));
}

/* both lines run until every light has the command (at most max_ms): the ms it took */
static double until_all(double max_ms)
{
    double t = 0;
    for (; t < max_ms; t += 100.0) {
        (void)spread(A.now > C.now ? A.now : C.now, 5, 1, 0);
        if (have_n == 260u) {
            break;
        }
        run2(&A, &C, 100.0);
    }
    return t;
}

static int cmd_ramp(line_t *p, uint16_t b, uint8_t l)
{
    ultra_values_t v;
    v.m = 0;
    memcpy(v.a, ONE5, sizeof v.a);
    memcpy(v.b, ONE5, sizeof v.b);
    return ultra_sender_set(&p->s, b, l, &v, 2000, 0, host(p));
}

static void in_step(void)
{
    ultra_sender_group_t g;
    double worst = 0, t0;
    uint32_t k, equal = 0, samples = 0;
    uint16_t b, lo;
    line_init(&A, 50, 5, 5, 0.02);
    line_init(&C, 10, 1, 6, 0.02);
    A.now = C.now = 7e9;
    A.host_off = C.host_off = 0;
    ultra_sender_group_init(&g);
    ultra_sender_join_group(&A.s, &g);
    ultra_sender_join_group(&C.s, &g);
    def_lights(&A, 5);
    def_lights(&C, 1);
    drain(&A);
    drain(&C);
    run2(&A, &C, 3000);
    A.loss = C.loss = 0.1;
    /* a breathe on 260 lights */
    calls(cmd_breathe);
    has_cmd = has_breathe;
    worst = measure(37.0, 160, 0);              /* from the calls on, 6 s */
    /* a breathe over 2 s moves at most pi / 2 s x 65535 = ~103 counts per ms */
    CHECK(worst <= 2.0 * 103.0 + 40.0 && have_n == 260u, "breathe on 260 lights over two lines: spread %.0f counts, "
          "%u lights have it", worst, have_n);
    printf("  breathe, 2 lines (50 x 5 + 10 x 1 lights), calls over 8 ms, 10 %% loss, HSI +-2 %%: the lights that "
           "have it within %.0f counts (~%.1f ms) from the start on; all 260 have it\n", worst, worst / 103.0);
    /* a strobe of 3 steps: every light shows the same step at the same time (except at its edges) */
    for (k = 0; k < 3u; k++) {
        strobe_st[k].duration_ms = (uint16_t)(100u + 50u * k);
        strobe_st[k].transition_ms = 0;
        strobe_st[k].v.m = (uint16_t)(10000u + 20000u * k);
        memcpy(strobe_st[k].v.a, ONE5, sizeof strobe_st[k].v.a);
        memcpy(strobe_st[k].v.b, ONE5, sizeof strobe_st[k].v.b);
    }
    calls(cmd_strobe);
    has_cmd = has_strobe;
    t0 = until_all(60000.0);                     /* a light that lost a step gets it from the refresh */
    samples = 2000;
    (void)measure(1.0, samples, &equal);
    CHECK(equal * 100u >= samples * 97u && have_n == 260u, "strobe: every light on the same step (%u of %u samples, "
          "%u lights with all steps)", equal, samples, have_n);
    printf("  strobe (3 steps, 450 ms), same lines: all 260 lights have every step %.1f s after the calls, then on the "
           "same step in %u of %u samples (1 ms apart)\n", t0 / 1000.0, equal, samples);
    /* a 2 s ramp to off, after the effect stops: every light starts it at the same moment */
    calls(cmd_stop);
    run2(&A, &C, 3000);
    calls(cmd_ramp);
    has_cmd = has_ramp;
    t0 = A.now;
    worst = measure(5.0, 600, 0);                /* 3 s, every 5 ms */
    has_cmd = 0;
    (void)spread((A.now > C.now ? A.now : C.now), 5, 1, &lo);
    /* 2 s from 65535 to 0: ~33 counts per ms */
    CHECK(worst <= 33.0 * 2.0 + 10.0 && lo == 0u, "SET ramp: spread %.0f counts during the ramp, end %u", worst, lo);
    printf("  SET ramp (2 s to off) after the effect stops, same lines: the lights that have it (late copies "
           "included) within %.0f counts (~%.1f ms); %.0f ms after the calls all 260 at 0\n", worst, worst / 33.0,
           (A.now - t0) / 1000.0);

    /* 4. power cycles: 5 boards of line A restart (they ran Ultra: sticky); the refresh brings back their breathe */
    calls(cmd_on);
    run2(&A, &C, 3000);
    calls(cmd_breathe);
    has_cmd = has_breathe;
    run2(&A, &C, 3000);
    for (b = 5; b < 50u; b += 10u) {
        board_boot(&A.bd[b], PROTO_ULTRA, 4242u + b);
    }
    run2(&A, &C, 20000);
    worst = measure(37.0, 40, 0);
    {
        uint32_t ok = 0;
        for (b = 5; b < 50u; b += 10u) {
            ok += A.bd[b].u.light[4].fx == ULTRA_FX_BREATHE && A.bd[b].u.light[0].defined;
        }
        CHECK(ok == 5u && worst <= 2.0 * 103.0 + 40.0 && worst > 0.0 && have_min == 260u,
              "power-cycled boards rejoin in phase (%u / 5, spread %.0f, %u lights)", ok, worst, have_min);
        printf("  5 boards power-cycled while the breathe runs: %u / 5 back (definitions, breathe) within 20 s, all "
               "260 lights within %.0f counts\n", ok, worst);
    }
}

/* ------------------------------------------------------------------------------------------------- 5. host restart */
static void host_restart(void)
{
    line_t *p = &A;
    uint16_t b;
    uint32_t got = 0;
    line_init(p, 10, 1, 11, 0.02);
    p->now = 3e9;
    p->host_off = 0;
    def_lights(p, 1);
    drain(p);
    run(p, 2000);
    /* the host restarts: a new line (other seed), its clock from 0 again; the boards keep running */
    {
        ultra_sender_config_t c;
        ultra_sender_config_init(&c, 10, 1);
        c.seed = 99;
        ultra_sender_init(&p->s, &c, p->sb, p->it, ITEMS);
        p->host_off = p->now - 1000.0;
    }
    run(p, 50);
    for (b = 0; b < 10u; b++) {
        ultra_light_def_t d;
        ultra_values_t v;
        ultra_light_def_init(&d);
        d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
        d.gamma_x1000 = 1000;
        ultra_sender_light_def(&p->s, b, 0, &d);
        v.m = (uint16_t)(222u + b);
        memcpy(v.a, ONE5, sizeof v.a);
        memcpy(v.b, ONE5, sizeof v.b);
        /* the same SEQ as the old host's last SET, on purpose: the board must not take it for a repeat */
        ultra_sender_preset_seq(&p->s, b, 0, ULTRA_SET, (uint8_t)A.bd[b].u.last_seq[0][ULTRA_SET]);
        ultra_sender_set(&p->s, b, 0, &v, 0, 0, host(p));
    }
    drain(p);
    run(p, 1000);
    for (b = 0; b < 10u; b++) {
        uint16_t out[5];
        render(&p->bd[b], p->now, out);
        got += out[0] == 222u + b;
    }
    CHECK(got == 10u, "after a host restart (TIME from 0, same SEQ as before) every board takes the new state (%u)", got);
    printf("  host restart (clock from 0, the new SET with the old SEQ): %u / 10 boards show the new state\n", got);
}

/* ------------------------------------------------------------------------------------------------- 6. stream */
static void stream(void)
{
    line_t *p = &A;
    uint16_t b, out[5];
    uint32_t i, ok = 0;
    uint8_t l;
    line_init(p, 10, 5, 21, 0.02);
    p->now = 2e9;
    p->loss = 0.1;
    def_lights(p, 5);
    drain(p);
    /* a host-side effect at 50 Hz for 3 s: every light a new value every 20 ms (5 lights per board, one slot per
     * frame: each light gets every 5th frame) */
    for (i = 0; i < 150u; i++) {
        for (b = 0; b < 10u; b++) {
            for (l = 0; l < 5u; l++) {
                ultra_values_t v;
                v.m = (uint16_t)(i * 400u + b * 10u + l);
                memcpy(v.a, ONE5, sizeof v.a);
                memcpy(v.b, ONE5, sizeof v.b);
                ultra_sender_stream(&p->s, b, l, &v, 20, host(p));
            }
        }
        run(p, 20);
    }
    run(p, 6000);                                /* stopped: the refresh brings the last value to every light */
    for (b = 0; b < 10u; b++) {
        render(&p->bd[b], p->now, out);
        for (l = 0; l < 5u; l++) {
            ok += out[l] == 149u * 400u + b * 10u + l;
        }
    }
    CHECK(ok == 50u, "a streamed effect, stopped: every light at its last value (%u / 50)", ok);
    printf("  host-side effect streamed to 10 x 5 lights at 50 Hz, 10 %% loss, then stopped: %u / 50 lights at their "
           "last value\n", ok);
}

/* ------------------------------------------------------------------------------------------------- 7. review round 7 */
/* a fade queued at start-up (the 50-board preamble still to send): the boards start it from dark, not at its end */
static void startup_fade(void)
{
    line_t *p = &A;
    ultra_values_t v;
    uint16_t out[5], b;
    uint32_t dark = 0;
    line_init(p, 50, 1, 1234, 0);
    p->now = 1e6;
    p->host_off = 0;
    v.m = 65535;
    memcpy(v.a, ONE5, sizeof v.a);
    memcpy(v.b, ONE5, sizeof v.b);
    for (b = 0; b < 50u; b++) {
        ultra_light_def_t d;
        ultra_light_def_init(&d);
        d.gamma_x1000 = 1000;
        d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
        ultra_sender_light_def(&p->s, b, 0, &d);
        ultra_sender_set(&p->s, b, 0, &v, 1000, 0, host(p));
    }
    while (!p->bd[49].u.light[0].cur.to.m) {
        step(p, 1e18);
    }
    for (b = 0; b < 50u; b++) {
        render(&p->bd[b], p->now, out);
        dark += out[0] < 2000u;
    }
    CHECK(dark == 50u, "a 1 s fade queued during the 50-board preamble: %u / 50 boards start it from dark", dark);
    printf("  start-up fade, 50 boards: the last board's first SET at %.3f s, %u / 50 boards still near dark then\n",
           p->now / 1e6, dark);
}

/* TIME wraps (71.6 min) while the boards run, and SET's SEQ goes 254, 255, 0, 1 */
static void wrap_and_seq(void)
{
    line_t *p = &A;
    uint16_t out[5];
    uint32_t i, ok = 0, b;
    uint8_t prm[ULTRA_FX_PARAMS];
    line_init(p, 3, 1, 77, 0.02);
    p->now = 4294.0e6;                           /* host clock 4294 s: TIME wraps at 4294.967296 s */
    p->host_off = 0;
    def_lights(p, 1);
    drain(p);
    run(p, 300);
    /* four SETs, one after the other, around the wrap: SEQ 254, 255, 0, 1 */
    for (i = 0; i < 4u; i++) {
        ultra_values_t v;
        v.m = (uint16_t)(1000u + 7000u * i);
        memcpy(v.a, ONE5, sizeof v.a);
        memcpy(v.b, ONE5, sizeof v.b);
        for (b = 0; b < 3u; b++) {
            ultra_sender_item_t *it = 0;
            uint16_t k;
            for (k = p->s.boards[b].first; k != 0xFFFFu; k = p->s.items[k].next) {
                if (p->s.items[k].slot[0] == ULTRA_SET) {
                    it = &p->s.items[k];
                }
            }
            if (i == 0u && it != 0) {
                it->slot[2] = 253;               /* the next SET takes 254 */
            }
            ultra_sender_set(&p->s, (uint16_t)b, 0, &v, 0, 0, host(p));
        }
        drain(p);
        run(p, 250);
        for (b = 0; b < 3u; b++) {
            render(&p->bd[b], p->now, out);
            ok += out[0] == v.m;
        }
    }
    CHECK(ok == 12u, "SETs with SEQ 254, 255, 0, 1, across TIME's wrap: %u / 12 shown", ok);
    /* a breathe across the wrap: in step (a line again, settled 3 s, from 4293.9 s: the wrap is 1.07 s into the
     * samples) */
    line_init(p, 3, 1, 78, 0.02);
    p->now = 4290.9e6;
    p->host_off = 0;
    def_lights(p, 1);
    drain(p);
    run(p, 3000);
    {
        double worst = 0, before = p->now;
        uint32_t k, moving = 0;
        uint16_t last = 0;
        for (b = 0; b < 3u; b++) {
            ultra_sender_effect(&p->s, (uint16_t)b, 0, ULTRA_FX_BREATHE, prm, ultra_fx_breathe(prm, 2000, 0, 65535),
                                host(p));
        }
        drain(p);
        for (k = 0; k < 60u; k++) {
            uint16_t lo = 65535, hi = 0;
            run(p, 37);
            for (b = 0; b < 3u; b++) {
                render(&p->bd[b], p->now, out);
                lo = out[0] < lo ? out[0] : lo;
                hi = out[0] > hi ? out[0] : hi;
            }
            worst = hi - lo > worst ? hi - lo : worst;
            moving += k > 0u && lo != last;
            last = lo;
        }
        CHECK(worst <= 2.0 * 103.0 + 40.0 && before < 4294.967296e6 && p->now > 4294.967296e6 && moving > 50u &&
              p->bd[0].u.light[0].fx == ULTRA_FX_BREATHE,
              "a breathe across TIME's wrap: in step (spread %.0f), running (%u changes)", worst, moving);
        printf("  TIME's wrap at 4294.97 s with SEQ 254 -> 1: %u / 12 SETs shown; a breathe across it within %.0f "
               "counts\n", ok, worst);
    }
}

/* every board effect (1-9) on the boards: id and parameters as sent */
static void all_effects(void)
{
    line_t *p = &A;
    uint8_t prm[10][ULTRA_FX_PARAMS], n[10];
    ultra_strobe_step_t st[2];
    uint32_t ok = 0, l, b;
    line_init(p, 2, 5, 31, 0.02);
    p->now = 2e9;
    def_lights(p, 5);
    drain(p);
    n[1] = (uint8_t)ultra_fx_pulse(prm[1], 300, 400, 900, 100, 60000);
    n[3] = (uint8_t)ultra_fx_flicker(prm[3], 62000, 1000, 16);
    n[4] = (uint8_t)ultra_fx_random(prm[4], 500, 1000);
    n[5] = (uint8_t)ultra_fx_candle(prm[5], 26214, 120);
    n[6] = (uint8_t)ultra_fx_breathe(prm[6], 3000, 100, 65000);
    n[7] = (uint8_t)ultra_fx_color_loop(prm[7], 20000, 60000);
    n[8] = (uint8_t)ultra_fx_sunrise(prm[8], 600000);
    n[9] = (uint8_t)ultra_fx_sunset(prm[9], 600000);
    memset(st, 0, sizeof st);
    st[0].duration_ms = 100;
    st[0].v.m = 65535;
    st[1].duration_ms = 200;
    for (l = 0; l < 10u; l++) {
        const uint8_t fx = (uint8_t)(l + 1u), board = (uint8_t)(l / 5u), light = (uint8_t)(l % 5u);
        if (fx == ULTRA_FX_STROBE) {
            CHECK(ultra_sender_strobe(&p->s, board, light, st, 2, host(p)) == ULTRA_OK, "strobe");
        } else if (fx < ULTRA_FX_COUNT) {
            CHECK(ultra_sender_effect(&p->s, board, light, fx, prm[fx], n[fx], host(p)) == ULTRA_OK, "effect %u", fx);
        }
    }
    drain(p);
    run(p, 2000);
    for (l = 0; l < 9u; l++) {
        const uint8_t fx = (uint8_t)(l + 1u);
        const ultra_light_t *L = &p->bd[l / 5u].u.light[l % 5u];
        uint16_t out[5];
        render(&p->bd[l / 5u], p->now, out);
        if (fx == ULTRA_FX_STROBE) {
            ok += L->fx == fx && L->step_count == 2u && L->step_have == 3u;
        } else {
            ok += L->fx == fx && memcmp(L->fxp, prm[fx], n[fx] < ULTRA_SCENE_FX_PARAMS ? n[fx] : ULTRA_SCENE_FX_PARAMS) == 0;
        }
    }
    for (b = 0; b < 2u; b++) {
        uint16_t out[5];
        render(&p->bd[b], p->now + 500000.0, out);   /* (they run) */
    }
    CHECK(ok == 9u, "every board effect 1-9 on the boards with its parameters (%u / 9)", ok);
    printf("  effects 1-9 (pulse, strobe, flicker, random, candle, breathe, colour loop, sunrise, sunset): %u / 9 on "
           "the boards with their parameters\n", ok);
}

/* 0.2.5 on the receiver: signal loss 3 (scene) and the retry policy reach every board's CONFIG store through loss
 * and corruption, and a change of either is an event the board acts on (ULTRA_EV_CONFIG, cfg_changed) */
static void config_025(void)
{
    line_t *p = &A;
    uint16_t b;
    uint32_t ok = 0, changed = 0;
    line_init(p, 20, 1, 77, 0.02);
    p->loss = 0.2;
    p->corrupt = 0.05;
    for (b = 0; b < 20u; b++) {
        CHECK(ultra_sender_config(&p->s, b, ULTRA_CFG_LOSS, 3) == ULTRA_OK &&
              ultra_sender_config(&p->s, b, ULTRA_CFG_RETRY, ULTRA_SENDER_RETRY(2u + b, 3u)) == ULTRA_OK, "config");
    }
    run(p, 20000);
    for (b = 0; b < 20u; b++) {
        const ultra_t *u = &p->bd[b].u;
        ok += (u->cfg_valid & (1u << ULTRA_CFG_LOSS)) && u->cfg[ULTRA_CFG_LOSS] == 3u &&
              (u->cfg_valid & (1u << ULTRA_CFG_RETRY)) && u->cfg[ULTRA_CFG_RETRY] == ULTRA_SENDER_RETRY(2u + b, 3u);
        p->bd[b].u.cfg_changed = 0;
    }
    (void)ultra_sender_config(&p->s, 7, ULTRA_CFG_RETRY, ULTRA_SENDER_RETRY(255u, 60u));
    run(p, 20000);
    for (b = 0; b < 20u; b++) {
        changed += (p->bd[b].u.cfg_changed & (1u << ULTRA_CFG_RETRY)) != 0u;
    }
    CHECK(ok == 20u && changed == 1u && p->bd[7].u.cfg[ULTRA_CFG_RETRY] == 0x3CFFu,
          "0.2.5: signal loss 3 and CONFIG 24 on all 20 boards (%u), 20 %% loss; a new retry policy: only board 7 "
          "changes (%u)", ok, changed);
    printf("  0.2.5 CONFIG, 20 boards, 20 %% loss + 5 %% corrupt slots: signal loss 3 and the retry policy stored on %u / "
           "20; a new policy for board 7: %u board changed\n", ok, changed);
}

/* without loss, every light has its first copy before the group's start: two lines, five lights per board raised at
 * run time, calls over 8 ms */
static int has_target_off(const ultra_light_t *L) { return L->cur.to.m == 0u; }
static void on_time(void)
{
    ultra_sender_group_t g;
    uint16_t b;
    int64_t start;
    line_init(&A, 50, 1, 8, 0.02);
    line_init(&C, 10, 1, 9, 0.02);
    A.now = C.now = 9e9;
    ultra_sender_group_init(&g);
    ultra_sender_join_group(&A.s, &g);
    ultra_sender_join_group(&C.s, &g);
    for (b = 0; b < 50u; b++) {
        ultra_sender_set_lights(&A.s, b, 5);     /* raised after init: the command budget follows */
    }
    def_lights(&A, 5);
    def_lights(&C, 1);
    drain(&A);
    drain(&C);
    run2(&A, &C, 3000);
    calls(cmd_ramp);
    start = A.s.items[A.s.boards[0].first].start_us;
    {
        uint16_t k;
        for (k = A.s.boards[0].first; k != 0xFFFFu; k = A.s.items[k].next) {
            if (A.s.items[k].slot[0] == ULTRA_SET) {
                start = A.s.items[k].start_us;
            }
        }
    }
    while ((A.now < C.now ? A.now : C.now) < (double)start - 1000.0) {
        run2(&A, &C, 0.5);
    }
    has_cmd = has_target_off;
    (void)spread((double)start - 1000.0, 5, 1, 0);
    has_cmd = 0;
    CHECK(have_n == 260u, "no loss: all 260 lights have their command 1 ms before the group's start (%u)", have_n);
    printf("  on time: 260 lights on 2 lines (5 per board, raised at run time), calls over 8 ms: %u have their "
           "command before the shared start\n", have_n);
}

int main(void)
{
    static const uint16_t sizes[] = { 1, 2, 3, 5, 8, 16, 25, 50, 64 };
    uint32_t i;
    printf("1. every command type through the boards' receive path\n");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        every_command(sizes[i], 0.2, 0.05);
    }
    printf("2. the preamble on new boards and boards locked to WS2805\n");
    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        lock_chain(sizes[i], 0);
        lock_chain(sizes[i], 1);
    }
    printf("3. / 4. effects and transitions in step, power cycles\n");
    in_step();
    printf("5. host restart\n");
    host_restart();
    printf("6. host-side effect\n");
    stream();
    printf("7. review round 7: start-up fade, TIME / SEQ wrap, every effect, on time\n");
    startup_fade();
    wrap_and_seq();
    all_effects();
    on_time();
    config_025();
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL SIMULATION TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
