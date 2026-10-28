/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * Appended by run_tests.py to its patched copy of the firmware's test/test_dline.c (see sim_dline_hook.inc): the scenarios
 * of the hub side. The reference hub (dport_driver.h: the sender library on one board in digital mode, ultra_dport.h's
 * planner and RMT words) drives the receiver's real delay line (core/dline.c) on the firmware's sample-exact model of the
 * hardware, with the model's checks: the strip sees every pixel frame whole and in order and nothing else (no marker, frame,
 * release or answer), every poll is answered, every capture decodes to the frame sent, SEND only into the released line.
 */
static int sim_fails;

/* These are steady-state poll/strip scenarios. Wire 3 requires measured-header
 * training on digital ports too; advance the real sender through that prehistory
 * before starting the sample model. Startup refusal is covered by test_dport.c's
 * test_sender_digital. Do not fabricate ready_us / ready_frames or bypass rc_ready. */
static void sim_train(hub_t *h)
{
    uint8_t buf[ULTRA_SENDER_FRAME_BYTES(1)];
    unsigned n = 0;
    while (!ultra_sender_rc_ready(&h->line, h->now_us)) {
        if (++n > 100 || ultra_sender_beacon_frame(&h->line, h->now_us, buf) != sizeof buf) {
            printf("sim: sender clock training failed\n");
            exit(3);
        }
        h->now_us += h->line.refresh_us;
    }
}

/* one scenario: chip, pixels per frame, a pixel frame ready every pix_us (0: none), a poll due every poll_us (0: none),
 * a command before every cmd_every-th transmission (effect cmd_fx), local (a local effect runs: no pixels, the
 * beacon), ntx transmissions at most, the hub's release latency rel_min..rel_max us after the frame's tail */
static void sim_case(const char *name, int chip, int px, int pix_us, int poll_us, int cmd_every, int cmd_fx, int local, int ntx,
                     int rel_min, int rel_max, int seeds)
{
    const chip_t *c = &chips[chip];
    int seed, ran;
    for (seed = 0; seed < seeds; seed++) {
        char *nm = (char *)malloc(96);
        const unsigned bytes = (unsigned)(px * c->bpp + c->hdr) / 8u;
        sprintf(nm, "dport %s #%d", name, seed);
        rs = 0x2545F4914F6CDD1DULL ^ ((unsigned long long)(seed + 1) * 0x9E3779B97F4A7C15ULL);
        rs |= 1u;
        hub_init(&SH, c->t0h, c->per0 - c->t0h, c->t1h, c->per1 - c->t1h, (uint32_t)(c->reset_us + 0.5), bytes);
        sim_train(&SH);
        SH.rng ^= (uint64_t)(seed + 7) * 0x2545F4914F6CDD1DULL;
        ultra_sender_set_lights(&SH.line, 0, ULTRA_SENDER_DEFAULT_LIGHTS);
        sh_base_us = SH.now_us;
        sh_poll_rel_min = rel_min;
        sh_poll_rel_max = rel_max;
        sh_expect = 1;
        sh_cmd_every = cmd_every;
        sh_cmd_n = 0;
        sh_cmd_effect = cmd_fx;
        if (local) {
            ultra_dp_local(&SH.dp, 1);
        }
        if (pix_us > 0) {
            hub_pixels(&SH, pix_us);
        }
        if (poll_us > 0) {
            hub_start_poll_timer(&SH, poll_us);
        }
        sc_begin(nm, chip);
        S.nit = ntx;
        S.want_frames = -1;
        ran = only == NULL || strncmp(nm, only, strlen(only)) == 0;
        {
            const int before = g_fail;
            go();
            sim_fails += g_fail - before;
        }
        /* the hub's own view of the run */
        if (getenv("DPORT_TRACE") != NULL) {                       /* DPORT_TRACE=<n>: the last n transmissions */
            int i, from = SH.ntx - atoi(getenv("DPORT_TRACE"));
            for (i = from < 0 ? 0 : from; i < SH.ntx; i++) {
                const tx_t *t = &SH.tx[i];
                printf("    tx %3d %-6s %9.1f .. %9.1f us  lead %3u tail %3u  %u bytes  slot %02x%s\n", i,
                       t->kind == ULTRA_DP_PIXELS ? "pixels" : t->kind == ULTRA_DP_POLL ? "poll" : "frame",
                       (double)(t->start_us - sh_base_us), (double)(t->end_us - sh_base_us), t->lead_us, t->tail_us,
                       t->nbytes, t->slot_type, t->beacon ? " beacon" : "");
            }
        }
        if (ran) {
            int i, nm_ = 0, np = 0, nf = 0;
            for (i = 0; i < SH.ntx; i++) {
                nm_ += SH.tx[i].kind == ULTRA_DP_POLL;
                np += SH.tx[i].kind == ULTRA_DP_PIXELS;
                nf += SH.tx[i].kind == ULTRA_DP_FRAME;
            }
            CHECK(local ? np == 0 : (pix_us == 0 || np > 0), "%s: %d pixel frames from the hub", nm, np);
            CHECK(poll_us == 0 || nm_ > 0, "%s: %d polls from the hub", nm, nm_);
            CHECK(cmd_every == 0 || nf > 0, "%s: %d command frames from the hub", nm, nf);
        }
        {
            int i;
            for (i = 0; i < SH.ntx; i++) {
                free(SH.tx[i].words);
            }
        }
        free(nm);
    }
}

int main(int argc, char **argv)
{
    only = argc > 1 && strcmp(argv[1], "*") != 0 ? argv[1] : NULL;     /* ... then, as the firmware's: <scenario> [<trace scenario>] */
    dbg_name = argc > 2 && strcmp(argv[2], "-") != 0 ? argv[2] : NULL;
    printf("dport/dline: the reference hub (sender library + ultra_dport.h) on the receiver's delay line\n");
    /* a stream with polls (no commands): each chip of the receiver's table */
    sim_case("WS2812B stream + polls", C_WS2812, 30, 2500, 9000, 0, 0, 0, 400, 5, 145, 3);
    sim_case("SK6812-RGBW (80 us reset)", C_SK6812, 24, 2500, 9000, 0, 0, 0, 400, 5, 145, 3);
    sim_case("WS2811-800k", C_WS2811, 30, 2500, 9000, 0, 0, 0, 400, 5, 145, 2);
    sim_case("WS2805", C_WS2805, 20, 2500, 9000, 0, 0, 0, 400, 5, 145, 2);
    sim_case("WS2915 (header + 80 bit)", C_WS2915, 12, 2500, 9000, 0, 0, 0, 400, 5, 145, 2);
    /* frames back to back (the hub never idles), polls and commands in between */
    sim_case("WS2812B flat out", C_WS2812, 10, 1, 4000, 0, 0, 0, 400, 5, 145, 3);
    sim_case("WS2812B + commands", C_WS2812, 30, 2500, 9000, 3, 0, 0, 400, 5, 145, 3);
    sim_case("SK6812 + commands, flat out", C_SK6812, 16, 1, 6000, 4, 0, 0, 400, 5, 145, 2);
    /* a local effect: no pixels, the commands and the header beacon, polls as ever */
    sim_case("local effect, commands + polls", C_WS2812, 30, 2500, 9000, 5, 7, 1, 120, 5, 145, 2);
    printf("dline sim: %ld checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
