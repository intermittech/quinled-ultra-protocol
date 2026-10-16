/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: bit timing and output encoders (ultra_sender_bits.h).
 */
#include "ultra_sender_bits.h"
#include "ultra_sender_internal.h"

uint64_t ultra_udiv64_(uint64_t x, uint64_t d)
{
    uint64_t q = 0, r = 0;
    uint32_t i;
    if ((x >> 32) == 0u && (d >> 32) == 0u) {
        return (uint32_t)x / (uint32_t)d;
    }
    if (d <= 0xFFFFu) {                                  /* 16 bits at a time: each step's dividend fits 32 */
        const uint32_t dd = (uint32_t)d, hi = (uint32_t)(x >> 32), lo = (uint32_t)x;
        uint32_t a = ((hi % dd) << 16) | (lo >> 16), q1, q0;
        q1 = a / dd;
        a = ((a - q1 * dd) << 16) | (lo & 0xFFFFu);
        q0 = a / dd;
        return ((uint64_t)(hi / dd) << 32) | ((uint64_t)q1 << 16) | q0;
    }
    for (i = 0; i < 64u; i++) {                          /* (constant shifts only: no libgcc on 32-bit targets) */
        const uint64_t carry = r >> 63;
        r = (r << 1) | (x >> 63);
        x <<= 1;
        q <<= 1;
        if (carry != 0u || r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
}

int ultra_timing_check(const ultra_timing_t *t)
{
    if (t->t0h_ns < ULTRA_T0H_MIN_NS || t->t0h_ns > ULTRA_T0H_MAX_NS || t->t1h_ns < ULTRA_T1H_MIN_NS ||
        t->t1h_ns > ULTRA_T1H_MAX_NS || t->t0l_ns < ULTRA_TL_MIN_NS || t->t0l_ns > ULTRA_TL_MAX_NS ||
        t->t1l_ns < ULTRA_TL_MIN_NS || t->t1l_ns > ULTRA_TL_MAX_NS || t->reset_ns < ULTRA_RESET_MIN_NS ||
        t->reset_ns > ULTRA_RESET_MAX_NS) {
        return -1;
    }
    return 0;
}

uint64_t ultra_wire_ns(const ultra_timing_t *t, const uint8_t *bytes, uint32_t n)
{
    uint32_t i, ones = 0;
    uint64_t bits, all;
    if (n == 0u) {
        return 0;
    }
    if ((uint32_t)t->t0h_ns + t->t0l_ns == (uint32_t)t->t1h_ns + t->t1l_ns) {   /* equal periods (SPI): no counting */
        return (uint64_t)n * 8u * ((uint32_t)t->t0h_ns + t->t0l_ns) - ((bytes[n - 1u] & 1u) ? t->t1l_ns : t->t0l_ns);
    }
    for (i = 0; i < n; i++) {
        uint32_t v;
        for (v = bytes[i]; v != 0u; v &= v - 1u) {
            ones++;
        }
    }
    bits = (uint64_t)n * 8u;
    all = (uint64_t)ones * ((uint32_t)t->t1h_ns + t->t1l_ns) + (bits - ones) * ((uint32_t)t->t0h_ns + t->t0l_ns);
    return all - ((bytes[n - 1u] & 1u) ? t->t1l_ns : t->t0l_ns);   /* the last bit ends with its falling edge */
}

uint32_t ultra_frame_max_us(const ultra_timing_t *t, uint32_t bytes)
{
    const uint32_t p0 = (uint32_t)t->t0h_ns + t->t0l_ns, p1 = (uint32_t)t->t1h_ns + t->t1l_ns;
    const uint32_t worst = p0 > p1 ? p0 : p1;
    return (uint32_t)ultra_udiv64_((uint64_t)bytes * 8u * worst, 1000u) + t->reset_ns / 1000u;
}

uint32_t ultra_encode_pulses(const ultra_timing_t *t, const uint8_t *bytes, uint32_t nbytes, uint32_t *pos,
                             ultra_pulse_t *out, uint32_t max)
{
    const uint32_t end = nbytes * 8u;
    uint32_t p = *pos, k = 0;
    while (k < max && p < end) {
        const int one = (bytes[p >> 3] >> (7u - (p & 7u))) & 1;
        out[k].high_ns = one ? t->t1h_ns : t->t0h_ns;
        out[k].low_ns = one ? t->t1l_ns : t->t0l_ns;
        if (p + 1u == end) {
            out[k].low_ns += t->reset_ns;                /* the reset gap after the last bit */
        }
        k++;
        p++;
    }
    *pos = p;
    return k;
}

/* ns -> ticks at hz, rounded */
static uint32_t ticks(uint32_t ns, uint32_t hz)
{
    return (uint32_t)ultra_udiv64_((uint64_t)ns * hz + 500000000u, 1000000000u);
}

/* ticks at hz -> ns, rounded */
static uint64_t tick_ns(uint32_t n, uint32_t hz)
{
    return ultra_udiv64_((uint64_t)n * 1000000000u + hz / 2u, hz);
}

/* whole ticks at hz -> a timing (*t only when every duration fits its field: else -1, far outside the spec anyway;
 * the range check comes after, at full width) */
static int ticks_to_timing(ultra_timing_t *t, uint32_t hz, uint32_t t0h, uint32_t t0l, uint32_t t1h, uint32_t t1l,
                           uint32_t rst)
{
    const uint64_t a = tick_ns(t0h, hz), b = tick_ns(t0l, hz), c = tick_ns(t1h, hz), d = tick_ns(t1l, hz);
    const uint64_t e = tick_ns(rst, hz);
    if (a > 0xFFFFu || b > 0xFFFFu || c > 0xFFFFu || d > 0xFFFFu || e > 0xFFFFFFFFu) {
        return -1;
    }
    t->t0h_ns = (uint16_t)a;
    t->t0l_ns = (uint16_t)b;
    t->t1h_ns = (uint16_t)c;
    t->t1l_ns = (uint16_t)d;
    t->reset_ns = (uint32_t)e;
    return 0;
}

int ultra_rmt_init(ultra_rmt_t *r, const ultra_timing_t *t, uint32_t resolution_hz)
{
    uint32_t t0h, t0l, t1h, t1l, rst, rst0, rst1;
    if (resolution_hz == 0u) {
        return -1;
    }
    t0h = ticks(t->t0h_ns, resolution_hz);
    t0l = ticks(t->t0l_ns, resolution_hz);
    t1h = ticks(t->t1h_ns, resolution_hz);
    t1l = ticks(t->t1l_ns, resolution_hz);
    rst = ticks(t->reset_ns, resolution_hz);
    rst0 = rst / 2u;
    rst1 = rst - rst0;
    if (t0h == 0u || t0l == 0u || t1h == 0u || t1l == 0u || rst0 == 0u || t0h > 0x7FFFu || t0l > 0x7FFFu ||
        t1h > 0x7FFFu || t1l > 0x7FFFu || rst1 > 0x7FFFu) {
        return -1;
    }
    if (ticks_to_timing(&r->effective, resolution_hz, t0h, t0l, t1h, t1l, rst) != 0 ||
        ultra_timing_check(&r->effective) != 0) {
        return -1;                                       /* rounded out of the spec */
    }
    r->bit0 = ULTRA_RMT_WORD(t0h, 1u, t0l, 0u);
    r->bit1 = ULTRA_RMT_WORD(t1h, 1u, t1l, 0u);
    r->reset = ULTRA_RMT_WORD(rst0, 0u, rst1, 0u);
    return 0;
}

uint32_t ultra_rmt_encode(const ultra_rmt_t *r, const uint8_t *bytes, uint32_t nbytes, uint32_t pos, uint32_t *out,
                          uint32_t max, int *done)
{
    const uint32_t end = nbytes * 8u;
    uint32_t k = 0;
    while (k < max) {
        if (pos < end) {
            out[k++] = ((bytes[pos >> 3] >> (7u - (pos & 7u))) & 1u) ? r->bit1 : r->bit0;
            pos++;
        } else {
            out[k++] = r->reset;
            *done = 1;
            break;
        }
    }
    return k;
}

int ultra_spi_timing(uint32_t spi_hz, ultra_timing_t *t)
{
    ultra_timing_t s;
    if (spi_hz == 0u || ticks_to_timing(&s, spi_hz, 1u, 4u, 3u, 2u, 0u) != 0) {   /* 10000 / 11100 */
        return -1;
    }
    s.reset_ns = 300000u;
    if (ultra_timing_check(&s) != 0) {
        return -1;
    }
    *t = s;
    return 0;
}

uint32_t ultra_spi_encode(const uint8_t *bytes, uint32_t nbytes, uint8_t *out)
{
    /* a nibble's 4 bits as 4 x 5 SPI bits ('1' 11100, '0' 10000), MSB first */
    static const uint32_t nib[16] = { 0x84210u, 0x8421Cu, 0x84390u, 0x8439Cu, 0x87210u, 0x8721Cu, 0x87390u,
                                      0x8739Cu, 0xE4210u, 0xE421Cu, 0xE4390u, 0xE439Cu, 0xE7210u, 0xE721Cu,
                                      0xE7390u, 0xE739Cu };
    uint32_t i;
    for (i = 0; i < nbytes; i++) {
        const uint32_t hi = nib[bytes[i] >> 4], lo = nib[bytes[i] & 15u];   /* 20 SPI bits each */
        uint8_t *o = out + 5u * i;
        o[0] = (uint8_t)(hi >> 12);
        o[1] = (uint8_t)(hi >> 4);
        o[2] = (uint8_t)((hi << 4) | (lo >> 16));
        o[3] = (uint8_t)(lo >> 8);
        o[4] = (uint8_t)lo;
    }
    return nbytes * 5u;
}
