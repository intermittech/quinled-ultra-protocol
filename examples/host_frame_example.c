/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra protocol - a minimal, platform-neutral host.
 *
 * Shows the parts every host needs, without any hardware:
 *   - building a frame (N slots + the header trailer) with TIME = the host clock at the frame's last bit;
 *   - start offsets of SET / EFFECT re-stamped against each frame's TIME;
 *   - SEQ numbers per (board, light, type), 3 copies per command, a refresh of the current state;
 *   - turning the bytes into pulses (RMT-style) or into an SPI bit stream.
 * A real host replaces now_us() and the output with its platform's clock and driver.
 *
 * Build: cc -std=c99 -O1 -o host_frame_example host_frame_example.c && ./host_frame_example
 */
#include <stdio.h>
#include <string.h>

#include "../include/ultra_protocol.h"

#define BOARDS        2u
#define COPIES        3u
#define START_DELAY   30000LL               /* us: commands start this long after they are queued */
/* at rest a real host sends a frame at least every 200 ms (the refresh, step 3 below) */

/* bit timing on the wire (ns): '0' = T0H high + T0L low, '1' = T1H + T1L */
#define T0H 340u
#define T0L 960u
#define T1H 700u
#define T1L 650u

/* ------------------------------------------------------------------------------------------- host state */
typedef struct {
    uint8_t slot[ULTRA_SLOT_BYTES];
    int64_t start_us;                       /* absolute start (SET / EFFECT), -1 = none */
    uint8_t copies_left;                    /* > 0: still urgent */
    uint8_t valid;
} item_t;

typedef struct {
    item_t light_def, set;                  /* light 0 only, to keep the example short */
    uint8_t seq[ULTRA_LIGHTS + 1][ULTRA_TYPES];   /* a real host starts these at random values */
    uint8_t rr;                             /* refresh round robin */
} board_t;

static board_t boards[BOARDS];
static int64_t fake_clock_us = 1000000;    /* stands in for the platform's monotonic us clock */

static int64_t now_us(void) { return fake_clock_us; }

/* ------------------------------------------------------------------------------------------- timing */
/* first rising edge to the last bit's falling edge, in ns */
static uint64_t wire_ns(const uint8_t *d, uint32_t n)
{
    uint64_t t = 0;
    uint32_t i, b;
    for (i = 0; i < n; i++) {
        for (b = 0; b < 8u; b++) {
            t += ((d[i] >> (7u - b)) & 1u) ? (T1H + T1L) : (T0H + T0L);
        }
    }
    return n ? t - ((d[n - 1u] & 1u) ? T1L : T0L) : 0;
}

/* the start offset (ms, rounded) of a SET / EFFECT slot relative to this frame's TIME, then the CRC again */
static void stamp(uint8_t *s, int64_t start_us, int64_t time_us)
{
    int64_t d, ms;
    if (start_us < 0) {
        return;
    }
    d = start_us - time_us;
    ms = d >= 0 ? (d + 500) / 1000 : -((-d + 499) / 1000);
    if (s[0] == ULTRA_SET) {
        ms = ms < -32768 ? -32768 : ms > 32767 ? 32767 : ms;
        ultra_put16(s + ULTRA_PAYLOAD + 25, (uint16_t)(int16_t)ms);
    } else if (s[0] == ULTRA_EFFECT) {
        ultra_put32(s + ULTRA_PAYLOAD + 1, (uint32_t)(int32_t)ms);
    }
    ultra_slot_seal(s);
}

/* ------------------------------------------------------------------------------------------- commands */
static void define_light(uint32_t b)
{
    static const uint8_t roles[5] = { ULTRA_ROLE_RED, ULTRA_ROLE_GREEN, ULTRA_ROLE_BLUE, ULTRA_ROLE_WARM_WHITE,
                                      ULTRA_ROLE_COLD_WHITE };
    static const uint16_t min_p[5] = { 0, 0, 0, 0, 0 }, max_p[5] = { 65535, 65535, 65535, 65535, 65535 };
    item_t *it = &boards[b].light_def;
    ultra_slot_light_def(it->slot, 0, ++boards[b].seq[0][ULTRA_LIGHT_DEF], roles, 2800, 0, min_p, max_p);
    it->start_us = -1;
    it->copies_left = COPIES;
    it->valid = 1;
}

/* light 0 of board b: M = state x brightness, a = colour per channel, transition in ms */
static void set_light(uint32_t b, uint16_t m, const uint16_t *a, uint32_t transition_ms)
{
    static const uint16_t full[5] = { 65535, 65535, 65535, 65535, 65535 };
    item_t *it = &boards[b].set;
    ultra_slot_set(it->slot, 0, ++boards[b].seq[0][ULTRA_SET], ULTRA_SET_SMOOTH, m, a, full, transition_ms, 0);
    it->start_us = now_us() + START_DELAY;  /* one absolute start for every copy */
    it->copies_left = COPIES;
    it->valid = 1;
}

/* ------------------------------------------------------------------------------------------- frames */
/* the slot for board b: an urgent copy first, else the next refresh item, else NOP */
static const item_t *pick(uint32_t b)
{
    board_t *bd = &boards[b];
    item_t *items[2] = { &bd->light_def, &bd->set };
    uint32_t i;
    for (i = 0; i < 2u; i++) {
        if (items[i]->valid && items[i]->copies_left) {
            items[i]->copies_left--;
            return items[i];
        }
    }
    for (i = 0; i < 2u; i++) {
        item_t *it = items[bd->rr++ % 2u];
        if (it->valid) {
            return it;
        }
    }
    return NULL;
}

/* builds one frame starting on the wire at start_us; returns its length, *time_out = TIME */
static uint32_t build_frame(uint8_t *f, int64_t start_us, int dark, int64_t *time_out)
{
    const uint32_t len = ultra_frame_bytes(BOARDS);
    int64_t starts[BOARDS], t;
    uint32_t b;
    int pass;
    for (b = 0; b < BOARDS; b++) {
        uint8_t *s = f + ULTRA_SLOT_BYTES * b;
        const item_t *it = dark ? NULL : pick(b);
        starts[b] = -1;
        if (dark) {
            ultra_slot_dark(s);
        } else if (it) {
            memcpy(s, it->slot, ULTRA_SLOT_BYTES);
            starts[b] = it->start_us;
        } else {
            ultra_slot_nop(s);
        }
    }
    /* TIME depends on the bits, and the start offsets are bits: stamp against an estimate, then once more */
    t = start_us + (int64_t)((uint64_t)len * 8u * ((T0H + T0L + T1H + T1L) / 2u) / 1000u);
    for (pass = 0; pass < 2; pass++) {
        for (b = 0; b < BOARDS; b++) {
            stamp(f + ULTRA_SLOT_BYTES * b, starts[b], t);
        }
        ultra_build_header(f + ultra_header_offset(BOARDS), (uint32_t)t);
        t = start_us + (int64_t)((wire_ns(f, len) + 500u) / 1000u);
    }
    ultra_build_header(f + ultra_header_offset(BOARDS), (uint32_t)t);
    *time_out = t;
    return len;
}

/* ------------------------------------------------------------------------------------------- output */
/* RMT-style: one (high ns, low ns) pair per bit; the reset gap (>= 280 us LOW) follows the frame */
static uint32_t to_pulses(const uint8_t *f, uint32_t len, uint16_t *high_ns, uint16_t *low_ns, uint32_t max)
{
    uint32_t i, b, n = 0;
    for (i = 0; i < len && n < max; i++) {
        for (b = 0; b < 8u && n < max; b++, n++) {
            const int one = (f[i] >> (7u - b)) & 1u;
            high_ns[n] = (uint16_t)(one ? T1H : T0H);
            low_ns[n] = (uint16_t)(one ? T1L : T0L);
        }
    }
    return n;
}

/* SPI at 3.0 MHz (333 ns per SPI bit), MOSI idle LOW, 4 SPI bits per protocol bit:
 * '0' = 1000 (333 ns high), '1' = 1100 (667 ns high), 1.33 us per bit. out: 4 bytes per input byte. */
static uint32_t to_spi(const uint8_t *f, uint32_t len, uint8_t *out)
{
    uint32_t i, b;
    for (i = 0; i < len; i++) {
        uint32_t w = 0;
        for (b = 0; b < 8u; b++) {
            w = (w << 4) | (((f[i] >> (7u - b)) & 1u) ? 0xCu : 0x8u);
        }
        ultra_put32(out + 4u * i, w);
    }
    return 4u * len;
}

static void dump(const char *what, const uint8_t *f, uint32_t len, int64_t t)
{
    uint32_t i;
    printf("%s: %u bytes, TIME 0x%08X\n", what, (unsigned)len, (unsigned)(uint32_t)t);
    for (i = 0; i < len; i++) {
        printf("%02X%s", f[i], (i + 1u) % 32u == 0u || i + 1u == len ? "\n" : " ");
    }
}

int main(void)
{
    static const uint16_t warm_white[5] = { 0, 0, 0, 65535, 0 };
    uint8_t f[ULTRA_SLOT_BYTES * BOARDS + ULTRA_HDR_BYTES], spi[4u * sizeof f];
    uint16_t hi[8u * sizeof f], lo[8u * sizeof f];
    uint32_t i, len, n;
    int64_t t;

    /* 1. start-up preamble: 4 N + 2 frames of DARK slots */
    for (i = 0; i < ultra_preamble_frames(BOARDS); i++) {
        len = build_frame(f, now_us(), 1, &t);
        fake_clock_us = t + 300;            /* frame on the wire, then the reset gap */
    }
    dump("last preamble frame", f, len, t);

    /* 2. define the lights, then a state: board 0 warm white at 50 %, board 1 at 100 %, 1 s transition */
    for (i = 0; i < BOARDS; i++) {
        define_light(i);
    }
    set_light(0, 32768, warm_white, 1000);
    set_light(1, 65535, warm_white, 1000);

    /* 3. frames until the copies are out (3 x LIGHT_DEF, then 3 x SET per board), then the refresh */
    for (i = 0; i < 7u; i++) {
        len = build_frame(f, now_us(), 0, &t);
        if (i == 3u) {
            dump("first frame with the SET slots", f, len, t);
            printf("board 0 SET starts %d ms after this TIME\n", (int)(int16_t)ultra_get16(f + ULTRA_PAYLOAD + 25));
        }
        fake_clock_us = t + 300;
    }

    /* 4. the same frame as pulses or SPI */
    n = to_pulses(f, len, hi, lo, (uint32_t)(sizeof hi / sizeof hi[0]));
    printf("as pulses: %u bits, first bit %u ns high / %u ns low\n", (unsigned)n, hi[0], lo[0]);
    printf("as SPI at 3 MHz: %u bytes\n", (unsigned)to_spi(f, len, spi));
    return 0;
}
