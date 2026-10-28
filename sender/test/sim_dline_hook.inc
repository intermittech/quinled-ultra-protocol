/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Included by tests/run_tests.py's patched copy of the firmware's test/test_dline.c, just before its run(): the model's hub
 * (gen_item) is replaced by the reference hub of dport_driver.h, which makes its transmissions with the sender library
 * and ultra_dport.h. Everything else is the firmware's: the receiver's delay line (dline.c) on a sample-exact model of
 * its hardware, and its checks on the strip: every pixel frame whole and in order, no marker / frame / release /
 * answer on the output (n_leak, n_mismatch), every poll answered, every capture decoding to the frame sent.
 */
#include "dport_driver.h"

static hub_t SH;                         /* the reference hub */
static int64_t sh_base_us;               /* the hub's clock at the model's hub_ns = 0 */
static int sh_poll_rel_min, sh_poll_rel_max;   /* the hub lets go of the line this long after t_end, us */
static int sh_expect;                    /* polls are answered */
static int sh_cmd_every, sh_cmd_n;       /* a command is queued before every sh_cmd_every-th transmission (0: none) */
static int sh_cmd_effect;                /* ... PIXEL_FX with this effect (0: a hand-back command: the port stays a stream) */

static double sh_ns(int64_t us)
{
    return (double)(us - sh_base_us) * 1000.0;
}

static void sim_gen_item(void)
{
    tx_t *t;
    double t0, ns;
    uint32_t k, first;
    long long last_fall_s = 0, first_fall_s = 0, first_rise_s = 0;
    int pix, bit_i = 0;
    int64_t want = sh_base_us + (int64_t)ceil(hub_ns / 1000.0);      /* (never before the model's own time) */
    int guard = 0;
    itn++;
    if (nmf >= MF_N - 4) {                                    /* (the model keeps 64 marker frames) */
        itn = SC->nit;
        return;
    }
    if (sh_cmd_every != 0 && ++sh_cmd_n % sh_cmd_every == 0) {
        ultra_pixel_fx_t fx;
        memset(&fx, 0, sizeof fx);
        fx.effect = (uint8_t)sh_cmd_effect;
        fx.brightness = 100;
        if (ultra_sender_pixel_fx(&SH.line, 0, 0, &fx, SH.now_us) != ULTRA_OK) {
            printf("  sim: pixel_fx failed\n");
            exit(3);
        }
    }
    if (SH.now_us < want) {
        SH.now_us = want;
    }
    while (!hub_step(&SH)) {
        SH.now_us += SH.loop_us + (int64_t)(hub_rand(&SH) % (uint32_t)(SH.loop_us + 1));
        if (++guard > 100000000) {
            printf("  sim: the hub never starts a transmission\n");
            exit(3);
        }
    }
    t = &SH.tx[SH.ntx - 1];
    pix = t->kind == ULTRA_DP_PIXELS;
    t0 = sh_ns(t->start_us);
    ns = t0;
    first = (uint32_t)(t->lead_us != 0u) + (uint32_t)(!pix);       /* the first bit's word */
    for (k = 0; k < t->nwords; k++) {
        const uint32_t w = t->words[k];
        const double d0 = (double)(w & 0x7FFFu) * 1000.0 / 80.0, d1 = (double)((w >> 16) & 0x7FFFu) * 1000.0 / 80.0;
        const int l0 = (int)((w >> 15) & 1u), l1 = (int)((w >> 31) & 1u);
        const int is_bit = k >= first && k < first + t->nbytes * 8u;
        put(ns, l0, l0 ? (pix ? K_PIX : K_WIN) : K_LOW);
        if (is_bit) {
            if (bit_i == 0) {
                first_rise_s = s_of(ns);
                first_fall_s = s_of(ns + d0);
            }
            last_fall_s = s_of(ns + d0);
            bit_i++;
        }
        put(ns + d0, l1, l1 ? (pix ? K_PIX : K_WIN) : K_LOW);
        ns += d0 + d1;
    }
    put(ns, 0, K_LOW);
    if (pix) {
        xframe_t *x = &xf[nxf < XF_N ? nxf++ : XF_N - 1];
        x->nbits = (int)t->nbytes * 8;
        x->bits = (unsigned char *)realloc(x->bits, t->nbytes);
        memcpy(x->bits, t->bytes, t->nbytes);
        x->s0 = first_rise_s;
        x->s1 = last_fall_s;
        x->ff = first_fall_s;
        hub_ns = ns;
    } else {
        mframe_t *m = &mf[nmf < MF_N ? nmf++ : MF_N - 1];
        memset(m, 0, sizeof *m);
        m->rw = t->kind == ULTRA_DP_POLL;
        m->nbits = (int)(ULTRA_SLOT_BITS + ULTRA_HDR_BITS);
        memcpy(m->bytes, t->bytes, t->nbytes);
        m->t_end = last_fall_s;
        m->t_rel = -1;
        m->expect = m->rw ? sh_expect : 0;
        if (m->rw) {
            const double t_end_ns = ns_of(last_fall_s);
            /* the hub lets go of the line after its 2 us tail, sh_poll_rel_min .. max us after t_end (the driver's latency) */
            const double rel = (ns - t_end_ns) / 1000.0 + urand(sh_poll_rel_min, sh_poll_rel_max);
            put(t_end_ns + rel * 1000.0, 1, K_WIN);               /* the hub lets go: the idle HIGH */
            m->t_rel = s_of(t_end_ns + rel * 1000.0);
            hub_wait = 1;
            wd_s = s_of(t_end_ns + (double)ultra_dp_window_us(ULTRA_RW_RATE_1M) * 1000.0);
            retake_s = wd_s;                                      /* the hub retakes at W_D, whatever happens */
            hub_dead = 1;
            post_us = 0.0;
            ret_lat_us = 0.0;
            poll_rate = ULTRA_RW_RATE_1M;
        } else {
            hub_ns = ns;
        }
    }
}
#define gen_item sim_gen_item
