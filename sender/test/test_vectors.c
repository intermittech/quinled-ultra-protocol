/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The sender library against the protocol's test vectors (test-vectors/vectors.json): every vector a host sends is
 * produced through the library's own API (commands, the scheduler, the preamble, the WS switch sequence, the
 * return-channel POLL frame) with the clock set so the header's TIME is the vector's, and compared byte for byte.
 * Vectors only a receiver meets (invalid headers, the reserved HINT flag) are checked with the header's parser; a
 * CONFIG vector whose pairs aren't in id order (the library packs every id in id order) as the same pairs.
 *
 * Usage: test_vectors <path to vectors.json>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra_sender.h"
#include "ultra_sender_rc.h"

#define TIME_V 0x00ABCDEFLL                      /* the vectors' TIME */
#define MAXB 3u

static char *json;
static int failures, checked;
static char done[128][48];
static int ndone;

static ultra_sender_t line;
static ultra_sender_board_t boards[MAXB];
static ultra_sender_item_t items[64];
static uint8_t buf[ULTRA_SENDER_FRAME_BYTES(MAXB)];

static const uint16_t full[5] = { 65535, 65535, 65535, 65535, 65535 };

/* -------------------------------------------------------------------------------------------- vectors.json */
static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                       : -1;
}

/* the vector's bytes into out; returns its length, -1 if missing */
static int vector(const char *name, uint8_t *out, int max)
{
    char key[96];
    const char *p, *h;
    int n = 0;
    snprintf(key, sizeof key, "\"name\": \"%s\"", name);
    p = strstr(json, key);
    if (p == NULL) {
        return -1;
    }
    h = strstr(p, "\"hex\": \"");
    if (h == NULL) {
        return -1;
    }
    for (h += 8; hexval(h[0]) >= 0 && hexval(h[1]) >= 0 && n < max; h += 2) {
        out[n++] = (uint8_t)(hexval(h[0]) * 16 + hexval(h[1]));
    }
    return n;
}

static void compare(const char *name, const uint8_t *got, int len)
{
    uint8_t want[256];
    const int n = vector(name, want, (int)sizeof want);
    int i;
    checked++;
    if (ndone < 128) {
        snprintf(done[ndone++], sizeof done[0], "%s", name);
    }
    if (n < 0) {
        printf("FAIL %s: not in vectors.json\n", name);
        failures++;
        return;
    }
    if (n != len || memcmp(want, got, (size_t)n) != 0) {
        printf("FAIL %s: %d bytes produced, %d expected\n  got  ", name, len, n);
        for (i = 0; i < len; i++) {
            printf("%02x", got[i]);
        }
        printf("\n  want ");
        for (i = 0; i < n; i++) {
            printf("%02x", want[i]);
        }
        printf("\n");
        failures++;
        return;
    }
    printf("ok   %s\n", name);
}

/* a CONFIG slot as its pairs: TYPE, LIGHT, SEQ equal, the same (id, value) pairs in any order, the unused ones (0, 0),
 * a valid CRC (a board takes the pairs in any order) */
static void compare_config(const char *name, const uint8_t *got)
{
    uint8_t want[32];
    const int n = vector(name, want, (int)sizeof want);
    uint32_t i, k, pairs = 0;
    int ok = n == 32 && memcmp(want, got, 3) == 0 && ultra_slot_check(got) == 0 && ultra_slot_check(want) == 0;
    checked++;
    if (ndone < 128) {
        snprintf(done[ndone++], sizeof done[0], "%s", name);
    }
    for (i = 0; ok && i < 9u; i++) {
        const uint8_t *w = want + ULTRA_PAYLOAD + 3u * i;
        if (w[0] == ULTRA_CFG_NONE) {
            ok = w[1] == 0u && w[2] == 0u;
            continue;
        }
        pairs++;
        for (k = 0; k < 9u && memcmp(got + ULTRA_PAYLOAD + 3u * k, w, 3) != 0; k++) {
        }
        ok = k < 9u;
    }
    for (i = 0; ok && i < 9u; i++) {
        const uint8_t *g = got + ULTRA_PAYLOAD + 3u * i;
        pairs -= (uint32_t)(g[0] != ULTRA_CFG_NONE);
        ok = g[0] != ULTRA_CFG_NONE || (g[1] == 0u && g[2] == 0u);
    }
    if (!ok || pairs != 0u) {
        printf("FAIL %s: not the same CONFIG pairs\n  got  ", name);
        for (i = 0; i < 32u; i++) {
            printf("%02x", got[i]);
        }
        printf("\n");
        failures++;
        return;
    }
    printf("ok   %s (%s)\n", name, memcmp(want, got, 32) == 0 ? "byte for byte" : "the same pairs, in id order");
}

/* -------------------------------------------------------------------------------------------- a line */
static void line_new(uint16_t n)
{
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, n, 5);          /* every light index in use: nothing to clear */
    c.seed = 12345;
    if (ultra_sender_init(&line, &c, boards, items, 64) != ULTRA_OK) {
        printf("FAIL init\n");
        exit(1);
    }
}

static void skip_preamble(void)
{
    uint32_t i;
    int64_t t = 1000;
    for (i = 0; i < ultra_preamble_frames(line.n); i++) {
        t += 20000;
        (void)ultra_sender_next_frame(&line, t, buf);
    }
}

/* the next frame, its clock set so TIME = target (a retime moves it without taking more from the queues). TIME counts
 * the frame's own bits, its header's too: a frame measures exactly the TIME it carries, so not every TIME is one a
 * given frame can carry. need_time: the vector compares the header (frames, headers); slot vectors only need the
 * start offsets, which are right to the ms either way. */
static const char *want_frame;                   /* the vector the next frame_at compares, NULL: none */

static uint32_t frame_at_t(int64_t target, int need_time)
{
    uint32_t len = ultra_sender_next_frame(&line, target - 2000, buf);
    int64_t base;
    int i;
    if (want_frame != NULL) {
        /* the start that gives the vector's TIME: its end minus its own bits on the wire, then the neighbours (a
         * frame measures the TIME it carries; where two TIMEs fit, the line picks one) */
        uint8_t want[256];
        const int n = vector(want_frame, want, (int)sizeof want);
        const int64_t s0 = target - (int64_t)((ultra_wire_ns(&line.timing, want, (uint32_t)n) + 500u) / 1000u);
        want_frame = NULL;
        for (i = 0; i <= 80 && ultra_sender_last_time_us(&line) != target; i++) {
            len = ultra_sender_retime(&line, s0 + ((i & 1) ? -(i + 1) / 2 : i / 2), buf);
        }
    }
    for (i = 0; i < 8 && ultra_sender_last_time_us(&line) != target; i++) {
        const int64_t now = line.last_start_us + (target - ultra_sender_last_time_us(&line));
        len = ultra_sender_retime(&line, now, buf);
    }
    base = line.last_start_us;
    for (i = 1; i <= 32 && ultra_sender_last_time_us(&line) != target; i++) {
        len = ultra_sender_retime(&line, base + ((i & 1) ? -(i + 1) / 2 : i / 2), buf);
    }
    if (ultra_sender_last_time_us(&line) != target && need_time) {
        printf("FAIL: TIME %lld instead of %lld\n", (long long)ultra_sender_last_time_us(&line), (long long)target);
        failures++;
    }
    return len;
}

static uint32_t frame_at(int64_t target)
{
    return frame_at_t(target, 1);
}

static uint32_t slot_frame_at(int64_t target)
{
    return frame_at_t(target, 0);
}

/* the next frame once the preamble is out and nothing is queued: forces it past the refresh interval */
static uint32_t idle_frame_at(int64_t target)
{
    line.last_start_us = target - 10000000;
    return frame_at(target);
}

/* a one-board RW frame asking `page` (rate 500 kbaud, POLL SEQ seq) once the chain may poll: its POLL slot */
static void rc_page_slot(const char *name, uint8_t page, const uint8_t *args, uint32_t nargs, uint8_t seq)
{
    ultra_rc_request_t rq;
    int64_t t;
    int i;
    line_new(1);
    skip_preamble();
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_500K);
    ultra_rc_request_add(&rq, 0, page, args, nargs);
    ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_POLL, seq);
    t = line.last_time_us + 300;
    for (i = 0; i < 6; i++) {                    /* a second of refresh frames */
        t += 200000;
        (void)ultra_sender_next_frame(&line, t, buf);
        t = line.last_time_us + 300;
    }
    if (ultra_sender_rc_frame(&line, &rq, t, buf) == 0) {
        memset(buf, 0, 32);                      /* (compare reports it) */
    }
    compare(name, buf, 32);
}

/* -------------------------------------------------------------------------------------------- the vectors */
static void headers(void)
{
    static const char *const receiver_only[] = { "hdr_flag_hint", "hdr_bad_crc", "hdr_version1", "hdr_bad_magic" };
    uint8_t h[8];
    uint32_t t, i;
    line_new(1);
    skip_preamble();
    frame_at(TIME_V);
    compare("hdr_normal", buf + 32, 8);
    idle_frame_at(1LL << 32);                    /* TIME's low 32 bits: 0 */
    compare("hdr_time_zero", buf + 32, 8);
    idle_frame_at(0xFFFFFFFFLL);
    compare("hdr_time_max", buf + 32, 8);
    /* a receiver's vectors: HINT is valid (hosts send FLAGS 0), the others invalid */
    for (i = 0; i < 4u; i++) {
        const int valid = i == 0u;
        checked++;
        if (vector(receiver_only[i], h, 8) != 8 || (ultra_parse_header(h, &t) == 0) != valid) {
            printf("FAIL %s: the parser %s it\n", receiver_only[i], valid ? "rejects" : "accepts");
            failures++;
        } else {
            printf("ok   %s (receiver side: %s)\n", receiver_only[i], valid ? "accepted" : "rejected");
        }
        snprintf(done[ndone++], sizeof done[0], "%s", receiver_only[i]);
    }
}

static void slots(void)
{
    static const uint8_t rgbww[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                      ULTRA_ROLE_COLD_WHITE };
    static const uint16_t a_orange[5] = { 65535, 26214, 0, 0, 0 };
    static const uint16_t a_cct[5] = { 0, 0, 0, 65535, 32768 };
    static const uint16_t a_red[5] = { 65535, 0, 0, 0, 0 };
    ultra_light_def_t d;
    ultra_light_opt_t o;
    ultra_values_t v;
    ultra_strobe_step_t st[2];
    uint8_t p[ULTRA_FX_PARAMS];
    uint32_t n, i;

    /* NOP and DARK */
    line_new(1);
    frame_at(TIME_V);                            /* the preamble's first frame */
    compare("slot_dark", buf, 32);
    skip_preamble();
    idle_frame_at(TIME_V);
    compare("slot_nop", buf, 32);

    /* LIGHT_DEF */
    line_new(1);
    skip_preamble();
    ultra_light_def_init(&d);
    memcpy(d.roles, rgbww, 5);
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_LIGHT_DEF, 0x11);
    ultra_sender_light_def(&line, 0, 0, &d);
    slot_frame_at(TIME_V);
    compare("slot_light_def", buf, 32);

    /* SET, and its frame */
    line_new(1);
    skip_preamble();
    v.m = 49151;
    memcpy(v.a, a_orange, sizeof v.a);
    memcpy(v.b, full, sizeof v.b);
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_SET, 0x22);
    ultra_sender_set_at(&line, 0, 0, &v, 1000, ULTRA_SET_SMOOTH, TIME_V + 30000);
    want_frame = "frame_1board_set";
    n = frame_at(TIME_V);
    compare("slot_set", buf, 32);
    compare("frame_1board_set", buf, (int)n);

    /* EFFECT pulse, and its frame */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_EFFECT, 0x33);
    n = ultra_fx_pulse(p, 500, 500, 1000, 0, 65535);
    ultra_sender_effect_at(&line, 0, 0, ULTRA_FX_PULSE, p, n, TIME_V + 30000);
    want_frame = "frame_effect";
    n = frame_at(TIME_V);
    compare("slot_effect_pulse", buf, 32);
    compare("frame_effect", buf, (int)n);

    /* EFFECT breathe on light 1 */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, 1, ULTRA_EFFECT, 0x34);
    n = ultra_fx_breathe(p, 5000, 6553, 65535);
    ultra_sender_effect_at(&line, 0, 1, ULTRA_FX_BREATHE, p, n, TIME_V + 30000);
    slot_frame_at(TIME_V);
    compare("slot_effect_breathe", buf, 32);

    /* strobe: the EFFECT, then STEP 0 in the next frame */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_EFFECT, 0x35);
    for (i = 0; i < 2u; i++) {
        st[i].duration_ms = 100;
        st[i].transition_ms = 0;
        st[i].v.m = 65535;
        memcpy(st[i].v.a, i == 0u ? a_red : full, sizeof st[i].v.a);
        memcpy(st[i].v.b, full, sizeof st[i].v.b);
    }
    ultra_sender_strobe_at(&line, 0, 0, st, 2, TIME_V + 30000);
    slot_frame_at(TIME_V);
    compare("slot_effect_strobe", buf, 32);
    slot_frame_at(TIME_V + 1000000);
    compare("slot_step", buf, 32);

    /* CONFIG (8 ids in one slot, in id order), and its frame */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_CONFIG, 0x44);
    ultra_sender_config(&line, 0, ULTRA_CFG_PWM_PRESET, 8);
    ultra_sender_config(&line, 0, ULTRA_CFG_STAGGER, 4);
    for (i = 0; i < 5u; i++) {
        ultra_sender_config(&line, 0, (uint8_t)(ULTRA_CFG_CH_CURRENT + i), i < 3u ? 3000 : 5000);
    }
    ultra_sender_config(&line, 0, ULTRA_CFG_BOARD_MAX, 8000);
    want_frame = "frame_config";
    n = frame_at(TIME_V);
    compare("slot_config", buf, 32);
    compare("frame_config", buf, (int)n);

    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_CONFIG, 0x45);
    ultra_sender_config(&line, 0, ULTRA_CFG_SOFT_START, 0);
    slot_frame_at(TIME_V);
    compare("slot_config_soft_start", buf, 32);

    /* 0.2.5: the retry policy (2 retries 3 s apart) and signal loss 3 (scene) */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_CONFIG, 0x46);
    ultra_sender_config(&line, 0, ULTRA_CFG_RETRY, ULTRA_SENDER_RETRY(2, 3));
    ultra_sender_config(&line, 0, ULTRA_CFG_LOSS, 3);
    slot_frame_at(TIME_V);
    compare_config("slot_config_retry_loss", buf);

    /* IDENTIFY */
    {
        static const struct {
            const char *name;
            uint8_t seq, value, arg;
        } ids[] = {
            { "slot_identify_on", 0x55, ULTRA_IDENT_ON, 0 },
            { "slot_identify_cal", 0x56, ULTRA_IDENT_CAL, 0 },
            { "slot_identify_dark", 0x57, ULTRA_IDENT_DARK, 10 },
            { "slot_identify_version", 0x58, ULTRA_IDENT_VERSION, 0 },
        };
        for (i = 0; i < 4u; i++) {
            line_new(1);
            skip_preamble();
            ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_IDENTIFY, ids[i].seq);
            ultra_sender_identify(&line, 0, ids[i].value, ids[i].arg);
            slot_frame_at(TIME_V);
            compare(ids[i].name, buf, 32);
        }
    }

    /* SCENE */
    line_new(1);
    skip_preamble();
    v.m = 32768;
    memcpy(v.a, a_cct, sizeof v.a);
    memcpy(v.b, full, sizeof v.b);
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_SCENE, 0x66);
    ultra_sender_scene(&line, 0, 0, 1, 10, &v);
    slot_frame_at(TIME_V);
    compare("slot_scene", buf, 32);

    /* LIGHT_OPT, without and with set points */
    line_new(1);
    skip_preamble();
    memset(&o, 0, sizeof o);
    o.flags = ULTRA_OPT_DIM_TO_WARM;
    o.cw_mired10 = 1538;
    o.ww_mired10 = 3704;
    o.warm_mired10 = 5556;
    o.red_assist = 19661;
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_LIGHT_OPT, 0x77);
    ultra_sender_light_opt(&line, 0, 0, &o);
    slot_frame_at(TIME_V);
    compare("slot_light_opt", buf, 32);

    line_new(1);
    skip_preamble();
    o.flags = ULTRA_OPT_DIM_TO_WARM | ULTRA_OPT_POINTS;
    o.points = 3;
    o.pt_m[0] = 65535;
    o.pt_mired10[0] = 3704;
    o.pt_m[1] = 16384;
    o.pt_mired10[1] = 4545;
    o.pt_m[2] = 3277;
    o.pt_mired10[2] = 5556;
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_LIGHT_OPT, 0x78);
    ultra_sender_light_opt(&line, 0, 0, &o);
    slot_frame_at(TIME_V);
    compare("slot_light_opt_points", buf, 32);

    /* SCENE_EFFECT candle */
    line_new(1);
    skip_preamble();
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_SCENE_EFFECT, 0x79);
    n = ultra_fx_candle(p, 32768, 100);
    ultra_sender_scene_effect(&line, 0, 0, ULTRA_FX_CANDLE, p, n);
    slot_frame_at(TIME_V);
    compare("slot_scene_effect", buf, 32);

    /* 0.2.8: the Ultra-Digital receiver's local pixel effects */
    {
        ultra_pixel_fx_t fx;
        memset(&fx, 0, sizeof fx);
        fx.effect = 7;
        fx.speed = 100;
        fx.intensity = 150;
        fx.palette = ULTRA_PX_PAL_COLORS;
        fx.c[0][0] = 255, fx.c[0][1] = 128, fx.c[0][2] = 0;
        fx.c[1][0] = 10, fx.c[1][1] = 20, fx.c[1][2] = 30;
        fx.c[2][0] = 40, fx.c[2][1] = 50, fx.c[2][2] = 60;
        fx.brightness = 128;
        fx.transition_ms = 1500;
        fx.seg_start = 5;
        fx.seg_len = 90;
        fx.flags = ULTRA_PXF_REVERSE | (ULTRA_PXW_ACCURATE << ULTRA_PXF_WHITE_SHIFT);
        fx.custom1 = 1;
        fx.custom2 = 2;
        line_new(1);
        skip_preamble();
        ultra_sender_preset_seq(&line, 0, 2, ULTRA_PIXEL_FX, 9);
        ultra_sender_pixel_fx_at(&line, 0, 2, &fx, TIME_V - 1234000);   /* START -1234 ms against the frame's TIME */
        slot_frame_at(TIME_V);
        compare("slot_pixel_fx", buf, 32);

        memset(&fx, 0, sizeof fx);
        fx.effect = ULTRA_PX_FX_STREAM;
        line_new(1);
        skip_preamble();
        ultra_sender_preset_seq(&line, 0, 0, ULTRA_PIXEL_FX, 0x0A);
        ultra_sender_pixel_fx_at(&line, 0, 0, &fx, TIME_V + 500000);
        slot_frame_at(TIME_V);
        compare("slot_pixel_fx_stream", buf, 32);

        line_new(1);
        skip_preamble();
        ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_CONFIG, 0x47);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_CHIP, ULTRA_PXC_SK6812_RGBW);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_ORDER,
                                  ULTRA_PX_ORDER(ULTRA_PXCH_G, ULTRA_PXCH_R, ULTRA_PXCH_B, ULTRA_PXCH_W, 0));
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_PIXELS, 300);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_FPS, 60);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_MA_CH, 200);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_LIMIT_MA, 0);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_WHITE, ULTRA_PXW_ACCURATE);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_VOFFSET, 0);
        ultra_sender_pixel_config(&line, 0, ULTRA_CFG_PX_VLENGTH, 0);
        slot_frame_at(TIME_V);
        compare("slot_config_pixel", buf, 32);
    }
}

static void frames(void)
{
    static const uint8_t rgbww[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                      ULTRA_ROLE_COLD_WHITE };
    ultra_light_def_t d;
    uint8_t p[ULTRA_FX_PARAMS];
    uint32_t n, i;

    /* 0.2.6: the planned power-off notice on 3 boards (each board's IDENTIFY SEQ 0x59) */
    line_new(3);
    skip_preamble();
    for (i = 0; i < 3u; i++) {
        ultra_sender_preset_seq(&line, (uint16_t)i, ULTRA_LIGHT_BOARD, ULTRA_IDENTIFY, 0x59);
    }
    (void)ultra_sender_power_off(&line);
    want_frame = "frame_power_off";
    n = frame_at(TIME_V);
    compare("slot_identify_power_off", buf, 32);
    compare("frame_power_off", buf, (int)n);

    /* 3 boards: LIGHT_DEF, EFFECT breathe on light 1, NOP */
    line_new(3);
    skip_preamble();
    ultra_light_def_init(&d);
    memcpy(d.roles, rgbww, 5);
    ultra_sender_preset_seq(&line, 0, 0, ULTRA_LIGHT_DEF, 0x11);
    ultra_sender_light_def(&line, 0, 0, &d);
    ultra_sender_preset_seq(&line, 1, 1, ULTRA_EFFECT, 0x34);
    n = ultra_fx_breathe(p, 5000, 6553, 65535);
    ultra_sender_effect_at(&line, 1, 1, ULTRA_FX_BREATHE, p, n, TIME_V + 30000);
    want_frame = "frame_3board_mixed";
    n = frame_at(TIME_V);
    compare("frame_3board_mixed", buf, (int)n);

    /* 2 boards idle, and the preamble's DARK frame */
    line_new(2);
    want_frame = "frame_preamble_dark_2boards";
    n = frame_at(TIME_V);
    compare("frame_preamble_dark_2boards", buf, (int)n);
    skip_preamble();
    want_frame = "frame_idle_2boards";
    n = idle_frame_at(TIME_V);
    compare("frame_idle_2boards", buf, (int)n);

    /* the WS config-frame switch sequence for 2 boards */
    line_new(2);
    ultra_sender_switch_to_ultra(&line, 0, 0xFF);
    for (i = 0; i < ULTRA_SWITCH_WS_FRAMES; i++) {
        char name[40];
        n = ultra_sender_next_frame(&line, 1000 + 20000 * (int64_t)i, buf);
        snprintf(name, sizeof name, "ws_switch_2boards_%u", (unsigned)i);
        compare(name, buf, (int)n);
    }
    /* then the preamble */
    n = ultra_sender_next_frame(&line, 1000000, buf);
    checked++;
    if (n != ultra_frame_bytes(2) || ultra_slot_check(buf) == 0 || buf[0] != 0) {
        printf("FAIL the switch sequence isn't followed by the preamble\n");
        failures++;
    } else {
        printf("ok   the switch sequence is followed by the preamble\n");
    }
}

/* the return channel (0.2.4): POLL slots and an RW frame for 3 receivers */
static void return_channel(void)
{
    static const uint8_t pw_ack[9] = { ULTRA_PW_ARG_ACK, 0, 3, 0, 1, 0, 0, 0, 2 };
    ultra_rc_request_t rq;
    uint32_t n;
    int64_t t;

    line_new(3);
    skip_preamble();
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_1M);
    ultra_rc_request_add(&rq, 1, ULTRA_PAGE_STATUS, 0, 0);
    ultra_rc_request_add(&rq, 2, ULTRA_PAGE_POWER, pw_ack, sizeof pw_ack);
    ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_POLL, 0x61);
    ultra_sender_preset_seq(&line, 1, ULTRA_LIGHT_BOARD, ULTRA_POLL, 0x60);
    ultra_sender_preset_seq(&line, 2, ULTRA_LIGHT_BOARD, ULTRA_POLL, 0x62);
    /* not before the chain has seen headers for ULTRA_RW_LOCK_MS after its preamble */
    t = line.last_time_us + 1000;
    checked++;
    if (ultra_sender_rc_frame(&line, &rq, t, buf) != 0) {
        printf("FAIL an RW frame right after the preamble\n");
        failures++;
    } else {
        printf("ok   no RW frame within %u ms of the preamble\n", (unsigned)ULTRA_RW_LOCK_MS);
    }
    t = line.last_time_us + 2000000;
    checked++;
    if (ultra_sender_rc_frame(&line, &rq, t, buf) != 0) {
        printf("FAIL an RW frame without headers since the preamble\n");
        failures++;
    } else {
        printf("ok   no RW frame before the chain has seen headers since the preamble\n");
    }
    {
        int i;                                   /* a second of refresh frames */
        for (i = 0; i < 6; i++) {
            t += 200000;
            (void)ultra_sender_next_frame(&line, t, buf);
            t = line.last_time_us + 300;
        }
    }
    n = ultra_sender_rc_frame(&line, &rq, t, buf);
    /* TIME: moved onto the vectors' value with a retime (the start from the vector's own bits, then neighbours) */
    {
        uint8_t want[256];
        const int nb = vector("frame_rw_poll", want, (int)sizeof want);
        const int64_t s0 = TIME_V - (int64_t)((ultra_wire_ns(&line.timing, want, (uint32_t)nb) + 500u) / 1000u);
        int i;
        for (i = 0; i <= 80 && ultra_sender_last_time_us(&line) != TIME_V; i++) {
            n = ultra_sender_retime(&line, s0 + ((i & 1) ? -(i + 1) / 2 : i / 2), buf);
        }
    }
    compare("slot_poll_relay", buf, 32);
    compare("slot_poll_status", buf + 32, 32);
    compare("slot_poll_power_ack", buf + 64, 32);
    compare("hdr_flag_rw", buf + 96, 8);
    compare("frame_rw_poll", buf, (int)n);
    /* the window: no frame before its end + the hold-off (a command waits) */
    ultra_sender_identify(&line, 0, ULTRA_IDENT_ON, 0);
    checked++;
    if (ultra_sender_next_frame(&line, ultra_sender_last_time_us(&line) + ultra_rw_window_us(2, 0), buf) != 0 ||
        ultra_sender_next_frame(&line, ultra_sender_last_time_us(&line) + ultra_rw_window_us(2, 0) +
                                ULTRA_RW_HOLDOFF_US, buf) == 0) {
        printf("FAIL the response window: a frame before W + hold-off, or none after it\n");
        failures++;
    } else {
        printf("ok   the next frame waits for the response window (W %u us + %u us)\n",
               (unsigned)ultra_rw_window_us(2, 0), (unsigned)ULTRA_RW_HOLDOFF_US);
    }
    /* 0.2.5 pages: LIFETIME (no arguments), DIAG part 1 (the fault memory); 0.2.7: IDENTITY part 1 (the features) */
    {
        static const uint8_t diag_part1[1] = { 1 }, id_part1[1] = { ULTRA_ID_PART_FEATURES };
        rc_page_slot("slot_poll_lifetime", ULTRA_PAGE_LIFETIME, NULL, 0, 0x63);
        rc_page_slot("slot_poll_diag_faults", ULTRA_PAGE_DIAG, diag_part1, sizeof diag_part1, 0x64);
        rc_page_slot("slot_poll_identity_features", ULTRA_PAGE_IDENTITY, id_part1, sizeof id_part1, 0x66);
        {
            static const uint8_t px_effects[1] = { ULTRA_PXG_PART_EFFECTS };
            rc_page_slot("slot_poll_pixel_fx_effects", ULTRA_PAGE_HW_FX, px_effects, sizeof px_effects, 0x67);
        }
    }
    /* 0.2.7: IDENTITY's two answers, built from the header's layout and compared, then read back through
     * ultra_rc_answer and the getters (part 0's fields, part 1's features) */
    {
        uint8_t a[32], rx[33], fe[33], got[32], uid[12];
        int k, ok;
        for (k = 0; k < 12; k++) {
            uid[k] = (uint8_t)(0x11 * k);
        }
        for (k = 0; k < 2; k++) {
            const char *name = k == 0 ? "answer_identity_board" : "answer_identity_features";
            memset(a, 0, sizeof a);
            a[0] = ULTRA_RSP_SYNC;
            a[1] = ULTRA_PAGE_IDENTITY;
            a[3] = (uint8_t)(k == 0 ? 0x67u : 0x66u);
            ultra_put16(a + 4, ultra_crc16(uid, 12));
            if (k == 0) {
                memcpy(a + ULTRA_RSP_DATA + ULTRA_ID_UID, uid, 12);
                ultra_put32(a + ULTRA_RSP_DATA + ULTRA_ID_FW_VERSION, 0x00060BFFu);
                a[ULTRA_RSP_DATA + ULTRA_ID_BL_VERSION] = 2u;
                a[ULTRA_RSP_DATA + ULTRA_ID_HW_ID] = 2u;
                ultra_put32(a + ULTRA_RSP_DATA + ULTRA_ID_CAPS, ULTRA_HWCAP_RETURN | ULTRA_HWCAP_RELAY |
                                                                ULTRA_HWCAP_VBUS | ULTRA_HWCAP_CURRENT);
            } else {
                ultra_put32(a + ULTRA_RSP_DATA + ULTRA_IDF_FEATURES, 0x007FFFFFu);
            }
            ultra_put16(a + 30, ultra_crc16(a, 30));
            compare(name, a, 32);
            rx[0] = ULTRA_RSP_LEAD;
            if (vector(name, rx + 1, 32) != 32) {
                memset(rx + 1, 0, 32);
            }
            memset(fe, 0, sizeof fe);
            ok = ultra_rc_answer(rx, fe, sizeof rx, got) == ULTRA_OK && ultra_rc_page(got) == ULTRA_PAGE_IDENTITY &&
                 ultra_rc_uid16(got) == ultra_crc16(uid, 12);
            if (k == 0) {
                ok = ok && memcmp(got + ULTRA_RSP_DATA + ULTRA_ID_UID, uid, 12) == 0 &&
                     got[ULTRA_RSP_DATA + ULTRA_ID_HW_ID] == 2u && got[ULTRA_RSP_DATA + ULTRA_ID_HW_REV] == 0u &&
                     ultra_get32(got + ULTRA_RSP_DATA + ULTRA_ID_CAPS) == 0x0000000Fu &&
                     ultra_get32(got + ULTRA_RSP_DATA + ULTRA_ID_FW_VERSION) == 0x00060BFFu;
            } else {
                ok = ok && ultra_rc_features(got) == 0x007FFFFFu && (ultra_rc_features(got) & ULTRA_FEAT_LIFE_SYNC) &&
                     ultra_rc_feature(got, 22) && !ultra_rc_feature(got, 23) && !ultra_rc_feature(got, 191);
            }
            checked++;
            if (!ok) {
                printf("FAIL %s: ultra_rc_answer / the IDENTITY getters\n", name);
                failures++;
            } else {
                printf("ok   %s: ultra_rc_answer and the IDENTITY getters\n", name);
            }
        }
    }
    /* 0.2.6: LIFE_SYNC, the hub's ledger (ultra_rc_request_life_sync), and a board's APPLIED answer: built here from
     * the header's layout and compared, then taken apart through ultra_rc_answer and the ultra_rc_ls_* accessors */
    {
        uint8_t lt[16], a[32], rx[33], fe[33], got[32];
        int i, ok;
        memset(lt, 0, sizeof lt);
        ultra_put24(lt + ULTRA_LT_POWERED, 10020u);
        ultra_put24(lt + ULTRA_LT_LIT, 4010u);
        ultra_put24(lt + ULTRA_LT_POWER_UPS, 531u);
        ultra_put16(lt + ULTRA_LT_OVERLOAD_TRIPS, 5u);
        ultra_put16(lt + ULTRA_LT_SHORT_TRIPS, 1u);
        ultra_put24(lt + ULTRA_LT_DIPS, 22u);
        line_new(1);
        skip_preamble();
        ultra_rc_request_init(&rq, ULTRA_RW_RATE_500K);
        ultra_rc_request_life_sync(&rq, 0, 0x1234u, lt, 9050u, 7u);
        ultra_sender_preset_seq(&line, 0, ULTRA_LIGHT_BOARD, ULTRA_POLL, 0x65);
        t = line.last_time_us + 300;
        for (i = 0; i < 6; i++) {                    /* a second of refresh frames */
            t += 200000;
            (void)ultra_sender_next_frame(&line, t, buf);
            t = line.last_time_us + 300;
        }
        if (ultra_sender_rc_frame(&line, &rq, t, buf) == 0) {
            memset(buf, 0, 32);
        }
        compare("slot_poll_life_sync", buf, 32);
        memset(a, 0, sizeof a);
        a[0] = ULTRA_RSP_SYNC;
        a[1] = ULTRA_PAGE_LIFE_SYNC;
        a[3] = 0x65u;
        ultra_put16(a + 4, 0x1234u);
        memcpy(a + ULTRA_RSP_DATA, lt, sizeof lt);
        ultra_put32(a + ULTRA_RSP_DATA + ULTRA_LS_A_TOTAL_WH, 9050u);
        a[ULTRA_RSP_DATA + ULTRA_LS_A_RESULT] = ULTRA_LS_R_APPLIED;
        a[ULTRA_RSP_DATA + ULTRA_LS_A_APPLIED] = ULTRA_LS_F_POWERED | ULTRA_LS_F_LIT | ULTRA_LS_F_UPS |
                                                 ULTRA_LS_F_TRIPS | ULTRA_LS_F_DIPS | ULTRA_LS_F_WH;
        ultra_put16(a + ULTRA_RSP_DATA + ULTRA_LS_A_RECORDS, 7u);
        ultra_put16(a + 30, ultra_crc16(a, 30));
        compare("answer_life_sync_applied", a, 32);
        rx[0] = ULTRA_RSP_LEAD;                      /* the vector as the hub's UART takes it: the lead byte first */
        if (vector("answer_life_sync_applied", rx + 1, 32) != 32) {
            memset(rx + 1, 0, 32);
        }
        memset(fe, 0, sizeof fe);
        ok = ultra_rc_answer(rx, fe, sizeof rx, got) == ULTRA_OK && ultra_rc_page(got) == ULTRA_PAGE_LIFE_SYNC &&
             ultra_rc_index(got) == 0u && ultra_rc_seq(got) == 0x65u && ultra_rc_uid16(got) == 0x1234u &&
             ultra_rc_ls_result(got) == ULTRA_LS_R_APPLIED && ultra_rc_ls_applied(got) == 0x3Fu &&
             ultra_rc_ls_total_wh(got) == 9050u && ultra_rc_ls_records(got) == 7u &&
             ultra_get24(got + ULTRA_RSP_DATA + ULTRA_LT_POWERED) == 10020u &&
             ultra_get16(got + ULTRA_RSP_DATA + ULTRA_LT_OVERLOAD_TRIPS) == 5u;
        checked++;
        if (!ok) {
            printf("FAIL answer_life_sync_applied: ultra_rc_answer / the ultra_rc_ls_* accessors\n");
            failures++;
        } else {
            printf("ok   answer_life_sync_applied: ultra_rc_answer and the ultra_rc_ls_* accessors\n");
        }
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    if (argc != 2) {
        fprintf(stderr, "usage: %s vectors.json\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    json = (char *)malloc((size_t)size + 1u);
    if (json == NULL || fread(json, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "read failed\n");
        return 2;
    }
    json[size] = 0;
    fclose(f);

    headers();
    slots();
    frames();
    return_channel();
    {
        /* every vector of the file was checked */
        const char *p = json;
        int total = 0, i, missing = 0;
        while ((p = strstr(p, "\"name\": \"")) != NULL) {
            char name[64];
            int k = 0;
            p += 9;
            while (p[k] != '"' && k < 63) {
                name[k] = p[k];
                k++;
            }
            name[k] = 0;
            total++;
            for (i = 0; i < ndone && strcmp(done[i], name) != 0; i++) {
            }
            if (i == ndone) {
                printf("FAIL %s: not checked\n", name);
                missing++;
            }
        }
        failures += missing;
        printf("%d vectors in the file, %d not checked\n", total, missing);
    }
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL VECTOR TESTS PASSED", checked, failures);
    return failures ? 1 : 0;
}
