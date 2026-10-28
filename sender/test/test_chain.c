/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The sender library and the Diff-Power-UR-Digital-M receiver's CHAIN mode (protocol 0.2.9): the line mode CONFIG 140 (its value
 * check, the rules for where a mode is allowed, the transport it selects), the CONFIG CRC over thirteen values against an
 * independent CRC, receivers at any position of a CHAIN line (a bound middle board needs FORWARD, every board before the last
 * one asked relays), and a normal Ultra line whose last board is the receiver: the preamble (4 N + 2 DARK frames), PIXEL_FX
 * and the pixel CONFIG in the receiver's own slot (its position) and nowhere else, the 3 copies, the refresh, NOP
 * when idle, a resync, the return channel's RW frame with a page 0x81 part 3 POLL in that slot (the boards in front
 * relay), the part 3 decoder, and that the boards in front send what they sent without the receiver. A digital port is
 * unchanged by it (no preamble, STREAM and AUTO only).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ultra_sender.h"
#include "ultra_sender_rc.h"
#include "ultra_sender_descriptor.h"
#include "../../test-vectors/shared_vectors.h"

/* the transport's LOW after an RW frame is inside the protocol's ULTRA_RW_RELEASE_MIN_US .. ULTRA_RW_RELEASE_US, with 50 us for the release's latency */
typedef char rc_hold_in_spec[(ULTRA_RC_RELEASE_HOLD_US >= ULTRA_RW_RELEASE_MIN_US && ULTRA_RC_RELEASE_HOLD_US + 50u <= ULTRA_RW_RELEASE_US) ? 1 : -1];

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

#define NB 4u
#define NI 64u
static ultra_sender_t L;
static ultra_sender_board_t B[NB];
static ultra_sender_item_t IT[NI];
static uint8_t F[ULTRA_SENDER_FRAME_BYTES(NB)];
static int64_t now;

static void mk(uint16_t n, uint8_t lights, uint16_t items)
{
    ultra_sender_config_t c;
    ultra_sender_config_init(&c, n, lights);
    c.seed = 4242;
    CHECK(ultra_sender_init(&L, &c, B, IT, items) == ULTRA_OK, "init");
    now = 1000000;
}

static uint32_t fr(void)
{
    const uint32_t n = ultra_sender_next_frame(&L, now, F);
    if (n != 0u) {
        now = L.last_start_us + L.frame_us;
    } else {
        memset(F, 0xEE, sizeof F);
        now += 1000;
    }
    return n;
}

static const uint8_t *slot(uint16_t b)
{
    return F + ULTRA_SLOT_BYTES * b;
}

/* ------------------------------------------------------------------------------------------------ the rules */
/* what the rules say, written as a table of cases rather than as the library's conditions */
static int expect_ok(uint8_t mode, int digital, uint16_t board, uint16_t boards)
{
    if (mode > 2u || board >= boards) {
        return 0;
    }
    if (digital) {
        return boards == 1u && (mode == ULTRA_PXLM_AUTO || mode == ULTRA_PXLM_STREAM);
    }
    return mode == ULTRA_PXLM_AUTO || mode == ULTRA_PXLM_CHAIN;   /* any position: it holds its slot, forwards the rest */
}

static void test_rules(void)
{
    uint32_t mode, digital, boards, board, ok = 0, bad = 0;
    for (mode = 0; mode < 5u; mode++) {
        for (digital = 0; digital < 2u; digital++) {
            for (boards = 0; boards < 6u; boards++) {
                for (board = 0; board < 7u; board++) {
                    const int got = ultra_pixel_line_mode_ok((uint8_t)mode, (int)digital, (uint16_t)board, (uint16_t)boards);
                    const int want = expect_ok((uint8_t)mode, (int)digital, (uint16_t)board, (uint16_t)boards);
                    ok += got != 0;
                    if ((got != 0) != (want != 0)) {
                        bad++;
                        printf("FAIL mode %u digital %u board %u of %u: %d, expected %d\n", (unsigned)mode, (unsigned)digital,
                               (unsigned)board, (unsigned)boards, got, want);
                    }
                }
            }
        }
    }
    CHECK(bad == 0u && ok > 0u, "the rules over 350 cases (%u allowed)", (unsigned)ok);
    /* the cases the design names */
    CHECK(ultra_pixel_line_mode_ok(ULTRA_PXLM_CHAIN, 0, 2, 3), "chain, last of 3");
    CHECK(ultra_pixel_line_mode_ok(ULTRA_PXLM_CHAIN, 0, 1, 3), "chain, in the middle of 3 (it forwards the rest)");
    CHECK(ultra_pixel_line_mode_ok(ULTRA_PXLM_CHAIN, 0, 0, 3), "chain, the first of 3 (several receivers on one line)");
    CHECK(ultra_pixel_line_mode_ok(ULTRA_PXLM_AUTO, 0, 2, 3) && ultra_pixel_line_mode_ok(ULTRA_PXLM_AUTO, 0, 0, 3),
          "auto on a normal line at any position");
    {                                                   /* the reset a receiver at board 0 needs (500 us; behind another board the default 300 us does) */
        ultra_timing_t t = ULTRA_TIMING_RECOMMENDED;
        CHECK(t.reset_ns == 500000u && ultra_timing_check(&t) == 0, "the recommended timing has a 300 us reset");
        CHECK(ultra_timing_chain_ok(&t, 0) && ultra_timing_chain_ok(&t, 1) && ultra_timing_chain_ok(&t, 2), "300 us: not enough for board 0, fine behind another board");
        t.reset_ns = ULTRA_CHAIN_FIRST_RESET_NS - 1u;
        CHECK(!ultra_timing_chain_ok(&t, 0), "499,999 ns is not enough for board 0");
        t.reset_ns = ULTRA_CHAIN_FIRST_RESET_NS;
        CHECK(ultra_timing_chain_ok(&t, 0) && ultra_timing_check(&t) == 0, "500 us is enough for board 0 and a valid timing");
    }
    CHECK(!ultra_pixel_line_mode_ok(ULTRA_PXLM_STREAM, 0, 0, 1), "stream needs a digital line");
    CHECK(!ultra_pixel_line_mode_ok(ULTRA_PXLM_CHAIN, 1, 0, 1), "chain on a digital line: refused");
    CHECK(ultra_pixel_line_mode_ok(ULTRA_PXLM_STREAM, 1, 0, 1) && ultra_pixel_line_mode_ok(ULTRA_PXLM_AUTO, 1, 0, 1),
          "stream and auto on a digital line");
    /* the transport */
    CHECK(ultra_pixel_transport(ULTRA_PXLM_STREAM, 0) == ULTRA_PXT_DIGITAL && ultra_pixel_transport(ULTRA_PXLM_STREAM, 1) == ULTRA_PXT_DIGITAL,
          "stream: ultra_dport.h");
    CHECK(ultra_pixel_transport(ULTRA_PXLM_CHAIN, 0) == ULTRA_PXT_CHAIN && ultra_pixel_transport(ULTRA_PXLM_CHAIN, 1) == ULTRA_PXT_CHAIN,
          "chain: the line's own transport");
    CHECK(ultra_pixel_transport(ULTRA_PXLM_AUTO, 1) == ULTRA_PXT_DIGITAL && ultra_pixel_transport(ULTRA_PXLM_AUTO, 0) == ULTRA_PXT_CHAIN,
          "auto: what the line is");
    /* CONFIG 140's value check (ultra_sender_pixel_config / ultra_pixel_config_value_ok) */
    CHECK(ultra_pixel_config_value_ok(ULTRA_CFG_PX_LINE_MODE, 0) && ultra_pixel_config_value_ok(ULTRA_CFG_PX_LINE_MODE, 1) &&
              ultra_pixel_config_value_ok(ULTRA_CFG_PX_LINE_MODE, 2),
          "CONFIG 140 takes 0, 1, 2");
    CHECK(!ultra_pixel_config_value_ok(ULTRA_CFG_PX_LINE_MODE, 3) && !ultra_pixel_config_value_ok(ULTRA_CFG_PX_LINE_MODE, 65535),
          "CONFIG 140 refuses 3 and up");
    CHECK(!ultra_pixel_config_value_ok(142, 0) && !ultra_pixel_config_value_ok(128, 0), "142 and 128 are no pixel ids");
    CHECK(ultra_pixel_config_value_ok(ULTRA_CFG_PX_SMOOTH, 3) && !ultra_pixel_config_value_ok(ULTRA_CFG_PX_SMOOTH, 4), "CONFIG 141 takes 0..3");
    {
        const unsigned id = ULTRA_CFG_PX_LINE_MODE, count = ULTRA_CFG_PX_COUNT, m0 = ULTRA_PXLM_AUTO, m1 = ULTRA_PXLM_STREAM,
                       m2 = ULTRA_PXLM_CHAIN;
        CHECK(id == 75u && count == 14u && m0 == 0u && m1 == 1u && m2 == 2u && ULTRA_CFG_PX_SMOOTH == 76u,
              "the header's numbers: CONFIG 140, 141, thirteen values, the three modes");
    }
    printf("ok   line mode rules, transport, CONFIG 140's value\n");
}

/* ------------------------------------------------------------------------------------- the CONFIG CRC, thirteen values */
static uint16_t crc16_bitwise(const uint8_t *d, uint32_t n)   /* CRC-16/CCITT-FALSE */
{
    uint16_t c = 0xFFFFu;
    uint32_t i, b;
    for (i = 0; i < n; i++) {
        c = (uint16_t)(c ^ ((uint16_t)d[i] << 8));
        for (b = 0; b < 8u; b++) {
            c = (uint16_t)((c & 0x8000u) ? ((c << 1) ^ 0x1021u) : (c << 1));
        }
    }
    return c;
}

static void test_config_crc(void)
{
    uint16_t v[14]={0},w[14];uint8_t b[56];unsigned i,k;uint32_t other=0;
    for(i=0;i<14;++i){v[i]=(uint16_t)(100+i);b[4*i]=1;b[4*i+1]=(uint8_t)(64+i);ultra_put16(b+4*i+2,v[i]);}
    CHECK(ultra_px_config_crc(1,v)==crc16_bitwise(b,56),"CONFIG CRC includes output TARGET and 14 IDs");
    for(k=0;k<14;++k){memcpy(w,v,sizeof w);++w[k];other+=ultra_px_config_crc(1,w)!=ultra_px_config_crc(1,v);}
    CHECK(other==14 && ultra_px_config_crc(0,v)!=ultra_px_config_crc(1,v),"all IDs and TARGET affect CRC");
}

/* --------------------------------------------------------------------------------------- a normal line, a chain end */
/* board 0, 1: Diff-Power-UR-Analog-M / Dig2Analog-Plus-5ch boards; board 2: the Diff-Power-UR-Digital-M receiver, CHAIN. */
static void ud_setup(uint16_t boards)
{
    mk(boards, 5, NI);                           /* (every light index in use: nothing is cleared) */
    CHECK(ultra_sender_set_lights(&L, (uint16_t)(boards - 1u), ULTRA_SENDER_DEFAULT_LIGHTS) == ULTRA_OK, "the receiver: every light index in use");
}

static void pre(void)
{
    uint32_t i;
    for (i = 0; i < ultra_preamble_frames(L.n); i++) {
        (void)fr();
    }
}

/* a frame of n boards: n slots + the header, valid */
static int frame_ok(uint32_t len, uint16_t boards)
{
    uint32_t t = 0;
    return len == ultra_frame_bytes(boards) && ultra_parse_header(F + ULTRA_SLOT_BYTES * boards, &t) == 0;
}

static void test_preamble_and_slots(void)
{
    ultra_pixel_fx_t fx;
    uint32_t i, n, seen_fx = 0, seen_cfg0 = 0, seen_cfg1 = 0;
    uint8_t fx_seq = 0, cfg_seq[2] = { 0, 0 };
    ud_setup(3);
    CHECK(ultra_sender_pixel_line_mode(&L, 2, 0, ULTRA_PXLM_CHAIN) == ULTRA_OK, "CHAIN on the last board");
    CHECK(ultra_sender_pixel_line_mode(&L, 2, 0, ULTRA_PXLM_STREAM) == ULTRA_ERR_ARG, "STREAM on a normal line: refused");
    CHECK(ultra_sender_pixel_line_mode(&L, 3, 0, ULTRA_PXLM_CHAIN) == ULTRA_ERR_ARG, "no such board");
    CHECK(ultra_sender_pixel_line_mode(&L, 2, 0, 3) == ULTRA_ERR_ARG, "mode 3");
    CHECK(ultra_sender_pixel_config(&L, 2, 0, ULTRA_CFG_PX_CHIP, ULTRA_PXC_SK6812_RGBW) == ULTRA_OK, "chip");
    CHECK(ultra_sender_pixel_config(&L, 2, 0, ULTRA_CFG_PX_PIXELS, 300) == ULTRA_OK, "pixels");
    memset(&fx, 0, sizeof fx);
    fx.effect = 7;
    fx.speed = 100;
    fx.brightness = 200;
    fx.c[0][0] = 255;
    CHECK(ultra_sender_pixel_fx(&L, 2, 0, &fx, now) == ULTRA_OK, "PIXEL_FX segment 0 of the last board");

    /* the preamble: 4 N + 2 = 14 DARK frames of N = 3 slots, the normal chain transport, before anything else */
    CHECK(ultra_preamble_frames(3) == 14u, "4 N + 2 = 14 frames");
    for (i = 0; i < 14u; i++) {
        uint32_t j, zero = 1;
        n = fr();
        CHECK(frame_ok(n, 3) && n == ULTRA_SENDER_FRAME_BYTES(3) , "preamble frame %u: 3 slots + header", (unsigned)i);
        for (j = 0; j < 3u * ULTRA_SLOT_BYTES; j++) {
            zero = zero && F[j] == 0u;
        }
        CHECK(zero, "preamble frame %u: DARK slots", (unsigned)i);
    }
    /* then the commands: in the receiver's slot (2), 3 copies each, the other boards NOP */
    for (i = 0; i < 60u && ultra_sender_pending(&L) != 0; i++) {
        n = fr();
        CHECK(frame_ok(n, 3), "frame %u", (unsigned)i);
        CHECK(slot(0)[0] == ULTRA_NOP && slot(1)[0] == ULTRA_NOP, "boards 0 and 1: NOP (frame %u)", (unsigned)i);
        if (slot(2)[0] == ULTRA_PIXEL_FX) {
            CHECK(ultra_slot_check(slot(2)) == 0 && (slot(2)[1]) == 0 && slot(2)[ULTRA_PX_EFFECT] == 7, "PIXEL_FX slot");
            CHECK(seen_fx == 0u || slot(2)[2] == fx_seq, "one SEQ in the copies");
            fx_seq = slot(2)[2];
            seen_fx++;
        } else if (slot(2)[0] == ULTRA_CONFIG) {
            const uint32_t page = slot(2)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_FIRST ? 0u : 1u;
            CHECK(ultra_slot_check(slot(2)) == 0 && (slot(2)[1]) == 0, "CONFIG slot");
            if (page == 0u) {
                CHECK(slot(2)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_CHIP && slot(2)[ULTRA_PAYLOAD + 3] == ULTRA_CFG_PX_PIXELS &&
                          ultra_get16(slot(2) + ULTRA_PAYLOAD + 4) == 300,
                      "page 0: chip, pixels");
                seen_cfg0++;
            } else {
                CHECK(slot(2)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_LINE_MODE && ultra_get16(slot(2) + ULTRA_PAYLOAD + 1) == ULTRA_PXLM_CHAIN &&
                          slot(2)[ULTRA_PAYLOAD + 3] == 0u,
                      "page 1: CONFIG 140 = CHAIN, alone");
                seen_cfg1++;
            }
            CHECK(cfg_seq[page] == 0u || cfg_seq[page] == slot(2)[2], "one SEQ in the copies of page %u", (unsigned)page);
            cfg_seq[page] = slot(2)[2];
        }
    }
    CHECK(seen_fx == 3u && seen_cfg0 == 3u && seen_cfg1 == 3u, "3 copies each of PIXEL_FX, CONFIG page 0, CONFIG page 1 (%u %u %u)",
          (unsigned)seen_fx, (unsigned)seen_cfg0, (unsigned)seen_cfg1);
    CHECK(cfg_seq[0] != cfg_seq[1], "each CONFIG page its SEQ");
    CHECK(!ultra_sender_pending(&L), "all sent");

    /* idle: NOP in every slot, a refresh frame at the refresh interval */
    {
        uint32_t idle_nop = 0, refreshed_fx = 0, refreshed_c1 = 0, k;
        now = ultra_sender_last_time_us(&L) + 10000;
        for (k = 0; k < 300u; k++) {
            L.last_start_us = now - 10000000;
            n = fr();
            if (n == 0u) {
                continue;
            }
            CHECK(frame_ok(n, 3) && slot(0)[0] != ULTRA_PIXEL_FX && slot(1)[0] != ULTRA_PIXEL_FX, "the refresh: only slot 2 carries pixel items");
            idle_nop += slot(0)[0] == ULTRA_NOP && slot(1)[0] == ULTRA_NOP;
            refreshed_fx += slot(2)[0] == ULTRA_PIXEL_FX;
            refreshed_c1 += slot(2)[0] == ULTRA_CONFIG && slot(2)[ULTRA_PAYLOAD] == ULTRA_CFG_PX_LINE_MODE;
            CHECK(!(slot(0)[0] == ULTRA_CONFIG && slot(0)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST) &&
                      !(slot(1)[0] == ULTRA_CONFIG && slot(1)[ULTRA_PAYLOAD] >= ULTRA_CFG_PX_FIRST),
                  "no pixel CONFIG for the boards in front");
        }
        CHECK(idle_nop > 100u, "boards 0 and 1 idle: NOP (%u)", (unsigned)idle_nop);
        CHECK(refreshed_fx >= 3u && refreshed_c1 >= 3u, "the refresh repeats PIXEL_FX and CONFIG 140 (%u, %u)", (unsigned)refreshed_fx,
              (unsigned)refreshed_c1);
    }

    /* a resync: the preamble again (4 N + 2), then PIXEL_FX and the CONFIG again, same START */
    {
        const int64_t start = ultra_sender_pixel_fx_start(&L, 2, 0);
        uint32_t again = 0;
        ultra_sender_resync(&L, now);
        pre();
        for (i = 0; i < 60u && ultra_sender_pending(&L) != 0; i++) {
            (void)fr();
            again += slot(2)[0] == ULTRA_PIXEL_FX || slot(2)[0] == ULTRA_CONFIG;
        }
        CHECK(again >= 9u, "resync: PIXEL_FX and both CONFIG pages again (%u)", (unsigned)again);
        CHECK(ultra_sender_pixel_fx_start(&L, 2, 0) == start, "the effect's START kept");
    }
    printf("ok   a chain end: preamble 4 N + 2, PIXEL_FX and CONFIG in the receiver's slot only, copies, refresh, NOP, resync\n");
}

static void test_single_receiver_line(void)
{
    ud_setup(1);
    CHECK(ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_CHAIN) == ULTRA_OK, "a normal line of one receiver: CHAIN");
    CHECK(ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_STREAM) == ULTRA_ERR_ARG, "... STREAM needs the digital line");
    CHECK(ultra_preamble_frames(1) == 6u, "4 N + 2 = 6 frames for one board");
    CHECK(!L.digital, "not a digital port");
    printf("ok   a normal line of one receiver\n");
}

static void test_digital_unchanged(void)
{
    ud_setup(1);
    CHECK(ultra_sender_digital(&L, 1) == ULTRA_OK, "digital port");
    CHECK(ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_CHAIN) == ULTRA_ERR_ARG, "CHAIN on a digital port: refused");
    CHECK(ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_STREAM) == ULTRA_OK && ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_AUTO) == ULTRA_OK,
          "STREAM and AUTO");
    CHECK(L.preamble_left == 0u, "a digital port has no preamble");
    CHECK(ultra_pixel_transport(ULTRA_PXLM_AUTO, L.digital) == ULTRA_PXT_DIGITAL, "its transport is ultra_dport.h");
    /* a line of more boards cannot be digital */
    mk(2, 2, NI);
    CHECK(ultra_sender_digital(&L, 1) == ULTRA_ERR_ARG, "a digital port has one receiver");
    printf("ok   a digital port: STREAM / AUTO only, no preamble\n");
}

/* ------------------------------------------------------------------------------------------- the return channel */
static void test_return_channel(void)
{
    ultra_rc_request_t rq;
    uint8_t a[32], rx[33], fe[33], got[32];
    ultra_pixel_line_t pl;
    static const uint8_t arg3[1] = { 0u };
    uint32_t n, i, k;
    int64_t t;
    ud_setup(3);
    pre();
    for (i = 0; i < 10u; i++) {
        (void)fr();
    }
    /* a second of refresh frames (the chain lock), then an RW frame: the receiver, the last board, answers index 0; the
     * boards in front are asked to relay only */
    t = now;
    for (i = 0; i < 6u; i++) {
        t += 200000;
        (void)ultra_sender_next_frame(&L, t, F);
        t = L.last_start_us + L.frame_us;
    }
    ultra_rc_request_init(&rq, ULTRA_RW_RATE_500K);
    CHECK(ultra_rc_request_add(&rq, 2, ULTRA_PAGE_PIXEL, arg3, sizeof arg3) == ULTRA_OK, "ask board 2 for page 0x81 part 3");
    n = ultra_sender_rc_frame(&L, &rq, t, F);
    CHECK(n == ultra_frame_bytes(3), "an RW frame of 3 slots + header (%u)", (unsigned)n);
    CHECK(slot(0)[0] == ULTRA_POLL && slot(1)[0] == ULTRA_POLL && slot(2)[0] == ULTRA_POLL, "a POLL in every slot");
    CHECK(slot(0)[ULTRA_POLL_INDEX] == ULTRA_POLL_RELAY_ONLY && slot(1)[ULTRA_POLL_INDEX] == ULTRA_POLL_RELAY_ONLY,
          "the boards in front relay only");
    CHECK(ultra_poll_valid(slot(2)) == 0 && slot(2)[ULTRA_POLL_INDEX] == 0u && slot(2)[ULTRA_POLL_SLOTS] == 1u &&
              slot(2)[ULTRA_POLL_PAGE] == ULTRA_PAGE_PIXEL && slot(2)[ULTRA_POLL_ARG] == 0u,
          "the receiver's POLL: index 0 of 1, page 0x81, argument 3");
    CHECK(ultra_rc_slot_start_us(ULTRA_RW_RATE_500K, 0) >= ULTRA_RW_FIRST_US, "the window's first answer slot");

    /* a part 3 answer as the receiver sends it (the vector answer_pixel_line's layout) and its decoder */
    memset(a, 0, sizeof a);
    a[0] = ULTRA_RSP_SYNC;
    a[1] = ULTRA_PAGE_PIXEL;
    a[2] = 0u << 4;
    a[3] = 0x68u;
    ultra_put16(a + 4, 0x5678u);
    a[ULTRA_RSP_DATA + ULTRA_PXL_CONFIG] = ULTRA_PXLM_CHAIN;
    a[ULTRA_RSP_DATA + ULTRA_PXL_MODE] = ULTRA_PXLM_CHAIN;
    a[ULTRA_RSP_DATA + ULTRA_PXL_FLAGS] = ULTRA_PXL_F_LOCKED | ULTRA_PXL_F_LIT | ULTRA_PXL_F_LONG | ULTRA_PXL_F_NONULTRA;
    a[ULTRA_RSP_DATA + ULTRA_PXL_SLOTS] = 4u;
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_FRAMES, 41234u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_NONULTRA, 2u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_LONG, 7u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_WINDOWS, 512u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_ANSWERS, 511u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_MISSED, 1u);
    ultra_put24(a + ULTRA_RSP_DATA + ULTRA_PXL_BITS, 4u * 256u + 64u);
    ultra_put16(a + ULTRA_RSP_DATA + ULTRA_PXL_MARKERS, 9u);
    ultra_put16(a + 30, ultra_crc16(a, 30));
    rx[0] = ULTRA_RSP_LEAD;
    memcpy(rx + 1, a, 32);
    memset(fe, 0, sizeof fe);
    CHECK(ultra_rc_answer(rx, fe, sizeof rx, got) == ULTRA_OK, "ultra_rc_answer takes it");
    CHECK(ultra_rc_pixel_line(got, &pl) == ULTRA_OK, "decoded");
    CHECK(pl.config == ULTRA_PXLM_CHAIN && pl.mode_in_effect == ULTRA_PXLM_CHAIN &&
              pl.flags == (ULTRA_PXL_F_LOCKED | ULTRA_PXL_F_LIT | ULTRA_PXL_F_LONG | ULTRA_PXL_F_NONULTRA) && pl.slots == 4u,
          "config, mode, flags, slots");
    CHECK(pl.frames == 41234u && pl.non_ultra == 2u && pl.long_frames == 7u && pl.windows == 512u && pl.answers == 511u &&
              pl.missed == 1u && pl.bits == 1088u && pl.markers == 9u,
          "counters (%u %u %u %u %u %u %u %u)", (unsigned)pl.frames, (unsigned)pl.non_ultra, (unsigned)pl.long_frames,
          (unsigned)pl.windows, (unsigned)pl.answers, (unsigned)pl.missed, (unsigned)pl.bits, (unsigned)pl.markers);
    /* not page 0x81 */
    got[1] = ULTRA_PAGE_STATUS;
    CHECK(ultra_rc_pixel_line(got, &pl) == ULTRA_ERR_ARG, "another page is refused");
    /* every field at its offset: one byte pattern, each field read back from its own place */
    memset(a, 0, sizeof a);
    a[1] = ULTRA_PAGE_PIXEL;
    for (k = 0; k < 24u; k++) {
        a[ULTRA_RSP_DATA + k] = (uint8_t)(0x10u + k);
    }
    CHECK(ultra_rc_pixel_line(a, &pl) == ULTRA_OK && pl.config == 0x10 && pl.mode_in_effect == 0x11 && pl.flags == 0x12 && pl.slots == 0x13 &&
              pl.frames == 0x1415 && pl.non_ultra == 0x1617 && pl.long_frames == 0x1819 && pl.windows == 0x1A1B && pl.answers == 0x1C1D &&
              pl.missed == 0x1E1F && pl.bits == 0x202122u && pl.markers == 0x2324 && pl.busy == 0x2526,
          "the offsets of ULTRA_PXL_*");
    printf("ok   return channel: the RW frame with the receiver last, page 0x81 part 3's decoder\n");
}

/* ------------------------------------------------------------------------------ the boards in front are unchanged */
static void test_front_boards_unchanged(void)
{
    static uint8_t trace[2][220][sizeof F];
    static uint32_t len[2][220];
    uint32_t run, i;
    for (run = 0; run < 2u; run++) {
        ultra_light_def_t d;
        ultra_values_t v;
        ultra_pixel_fx_t f;
        mk(3, 5, NI);
        ultra_light_def_init(&d);
        d.roles[0] = ULTRA_ROLE_BRIGHTNESS;
        (void)ultra_sender_light_def(&L, 0, 0, &d);
        (void)ultra_sender_light_def(&L, 1, 1, &d);
        memset(&v, 0, sizeof v);
        v.m = 30000;
        for (i = 0; i < 5u; i++) {
            v.a[i] = v.b[i] = 65535;
        }
        (void)ultra_sender_set(&L, 0, 0, &v, 500, 0, now);
        (void)ultra_sender_set(&L, 1, 1, &v, 200, 0, now);
        (void)ultra_sender_config(&L, 1, ULTRA_CFG_PWM_PRESET, 3);
        if (run == 1u) {                         /* board 2 is the receiver in chain mode; boards 0, 1 are as before */
            memset(&f, 0, sizeof f);
            f.effect = 9;
            f.brightness = 128;
            (void)ultra_sender_set_lights(&L, 2, ULTRA_SENDER_DEFAULT_LIGHTS);
            (void)ultra_sender_pixel_line_mode(&L, 2, 0, ULTRA_PXLM_CHAIN);
            (void)ultra_sender_pixel_config(&L, 2, 0, ULTRA_CFG_PX_PIXELS, 100);
            (void)ultra_sender_pixel_fx(&L, 2, 0, &f, now);
        }
        for (i = 0; i < 220u; i++) {
            len[run][i] = fr();
            memcpy(trace[run][i], F, sizeof F);
        }
    }
    for (i = 0; i < 220u; i++) {
        CHECK(len[0][i] == len[1][i], "frame %u length (%u / %u)", (unsigned)i, (unsigned)len[0][i], (unsigned)len[1][i]);
        if (len[0][i] != 0u && len[1][i] != 0u) {
            CHECK(memcmp(trace[0][i], trace[1][i], 2u * ULTRA_SLOT_BYTES) == 0, "frame %u: the slots of boards 0 and 1", (unsigned)i);
        }
    }
    printf("ok   the boards in front of a chain-mode receiver send what they sent without it\n");
}

#include "test_chain_update.inc"
/* ------------------------------------------------------------------------------------------- mixed chains */
static uint8_t DBUF[NB][2048];
static ultra_descriptor_t DD[NB];

/* bind board b a shared descriptor vector with an adaptive LINK 0 that answers (RETURN), its flags replaced and the CRC re-sealed */
static int bind_link(uint16_t b, uint8_t flags)
{
    static const char *const names[] = { "descriptor_single_pixel", "descriptor_dual_pixel", "descriptor_analog20", "descriptor_nano2",
                                         "descriptor_partial7", "descriptor_sense_10a_board" };
    unsigned i, k;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        const ultra_shared_vector_t *v = NULL;
        ultra_descriptor_t d;
        ultra_tlv_t r;
        ultra_desc_link_t l;
        for (k = 0; k < ULTRA_SHARED_VECTOR_COUNT; k++) {
            if (strcmp(names[i], ultra_shared_vectors[k].name) == 0) {
                v = &ultra_shared_vectors[k];
            }
        }
        if (!v || v->size > sizeof DBUF[0] || ultra_descriptor_decode(&d, v->bytes, v->size) || ultra_descriptor_find(&d, 1, 0, &r) ||
            ultra_descriptor_link(&d, 0, &l) || l.phy != ULTRA_PHY_ADAPTIVE || !(l.flags & ULTRA_LINK_RETURN) || !l.rates) {
            continue;
        }
        memcpy(DBUF[b], v->bytes, v->size);
        DBUF[b][(size_t)(r.value - v->bytes) + 1u] = flags;
        if (ultra_descriptor_find(&d, 4, 0, &r) == 0) {
            DBUF[b][(size_t)(r.value - v->bytes) + 3u] |= 4u;   /* the pixel output takes a line mode (CONFIG 75) */
        }
        ultra_put16(DBUF[b] + 4, ultra_crc16(DBUF[b] + 6, (uint32_t)v->size - 6u));
        if (ultra_descriptor_decode(&DD[b], DBUF[b], v->size)) {
            return -2;
        }
        return ultra_sender_descriptor(&L, b, &DD[b]);
    }
    return -1;
}

static void test_mixed_positions(void)
{
    ultra_rc_request_t rq;
    ultra_desc_link_t l;
    uint32_t i, k, n;
    uint8_t rate;
    int64_t t;
    /* stateless: a receiver at any position of a CHAIN line, several on one line */
    ud_setup(3);
    CHECK(ultra_sender_pixel_line_mode(&L, 1, 0, ULTRA_PXLM_CHAIN) == ULTRA_OK, "CHAIN on board 1 of 3 (it forwards the rest)");
    CHECK(ultra_sender_pixel_line_mode(&L, 0, 0, ULTRA_PXLM_AUTO) == ULTRA_OK, "AUTO on board 0 of 3: a second receiver on the line");
    CHECK(ultra_sender_pixel_line_mode(&L, 1, 0, ULTRA_PXLM_STREAM) == ULTRA_ERR_ARG, "STREAM on a normal line: refused");
    /* bound boards: one in the middle forwards (FORWARD, not CHAIN_END); the last may be a chain end */
    mk(3, 0, NI);
    CHECK(bind_link(1, ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_MARKER) == 0 &&
          bind_link(2, ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_MARKER) == 0, "two chain-end descriptors bound");
    CHECK(ultra_sender_pixel_line_mode(&L, 1, 0, ULTRA_PXLM_CHAIN) == ULTRA_ERR_ARG, "a bound chain end in the middle: refused");
    CHECK(ultra_sender_pixel_line_mode(&L, 2, 0, ULTRA_PXLM_CHAIN) == ULTRA_OK, "a bound chain end last: CHAIN");
    mk(3, 0, NI);
    CHECK(bind_link(1, ULTRA_LINK_RETURN | ULTRA_LINK_RELAY | ULTRA_LINK_FORWARD | ULTRA_LINK_MARKER | ULTRA_LINK_UPDATE_FWD) == 0,
          "a forwarding receiver bound");
    CHECK(ultra_sender_pixel_line_mode(&L, 1, 0, ULTRA_PXLM_CHAIN) == ULTRA_OK && ultra_sender_pixel_line_mode(&L, 1, 0, ULTRA_PXLM_AUTO) == ULTRA_OK,
          "a bound forwarding receiver in the middle: CHAIN, AUTO");
    /* the RW plan: every board before the last one asked relays (RELAY), an asked one answers too (RETURN) */
    for (i = 0; i < 3u; i++) {
        static const uint8_t first[3] = { ULTRA_LINK_RETURN | ULTRA_LINK_FORWARD | ULTRA_LINK_MARKER,
                                          ULTRA_LINK_RETURN | ULTRA_LINK_RELAY | ULTRA_LINK_FORWARD | ULTRA_LINK_MARKER | ULTRA_LINK_UPDATE_FWD,
                                          ULTRA_LINK_RELAY | ULTRA_LINK_FORWARD };
        mk(3, 0, NI);
        CHECK(bind_link(0, first[i]) == (i == 2u ? -2 : 0) || i == 2u, "board 0 bound (%u)", (unsigned)i);
        if (i == 2u) {
            continue;                                       /* (RELAY without RETURN: not a valid adaptive answering LINK) */
        }
        CHECK(bind_link(2, ULTRA_LINK_RETURN | ULTRA_LINK_CHAIN_END | ULTRA_LINK_MARKER) == 0, "board 2 bound");
        CHECK(ultra_descriptor_link(&DD[0], 0, &l) == 0, "LINK 0");
        for (rate = 0; rate < ULTRA_RW_RATES && !(l.rates & (1u << rate)); rate++) {
        }
        pre();
        for (k = 0; k < 10u; k++) {
            (void)fr();
        }
        t = now;
        for (k = 0; k < 6u; k++) {
            t += 200000;
            (void)ultra_sender_next_frame(&L, t, F);
            t = L.last_start_us + L.frame_us;
        }
        ultra_rc_request_init(&rq, rate);
        CHECK(ultra_rc_request_add(&rq, 0, ULTRA_PAGE_STATUS, NULL, 0) == ULTRA_OK &&
              ultra_rc_request_add(&rq, 2, ULTRA_PAGE_STATUS, NULL, 0) == ULTRA_OK, "ask boards 0 and 2");
        n = ultra_sender_rc_frame(&L, &rq, t, F);
        if (i == 0u) {
            CHECK(n == 0u, "board 0 asked but without RELAY in front of board 2: the window refused (%u)", (unsigned)n);
        } else {
            CHECK(n == ultra_frame_bytes(3) && slot(0)[ULTRA_POLL_INDEX] == 0u && slot(1)[ULTRA_POLL_INDEX] == ULTRA_POLL_RELAY_ONLY &&
                  slot(2)[ULTRA_POLL_INDEX] == 1u, "board 0 answers and relays, board 1 relays, board 2 answers (%u)", (unsigned)n);
        }
    }
    printf("ok   receivers at any position of a CHAIN line: a bound middle board forwards, the boards before the last one asked relay\n");
}

int main(int argc,char **argv)
{
    test_rules();
    test_mixed_positions();
    test_config_crc();
    test_preamble_and_slots();
    test_single_receiver_line();
    test_digital_unchanged();
    test_return_channel();
    test_front_boards_unchanged();
    test_chain_update_bound();
    test_chain_update_abort_gap();
    test_chain_update_abandon();
    test_chain_update_bound_waits();
    test_chain_update_refused();
    test_chain_update_silent();
    test_chain_update_recover();
    test_chain_update_install_gate();
    test_chain_update_mixed_versions();
    test_chain_update_end_lost();
    test_chain_update_recover_refused();
    test_chain_update_traces(argc>1?argv[1]:NULL);
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL CHAIN TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
