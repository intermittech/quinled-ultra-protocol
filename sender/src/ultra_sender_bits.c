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

int ultra_timing_chain_ok(const ultra_timing_t *t, uint16_t board)
{
    (void)board;
    return t && t->reset_ns >= ULTRA_RESET_MIN_NS;
}

int ultra_timing_profile(ultra_timing_t *t, uint8_t profile, uint32_t hz, uint32_t reset_us)
{
    ultra_timing_t v={0};
    const ultra_line_profile_t *p=ultra_line_profile(profile);
    ultra_line_rmt_t r;
    if (!t || !p || reset_us < 500u || reset_us > ULTRA_RESET_MAX_NS/1000u) return -1;
    if (hz==9600000u) { v.bit_ticks=p->bit_samples; v.zero_ticks=p->zero_samples; v.one_ticks=p->one_samples; }
    else {
        if (ultra_line_rmt(profile,hz,&r)) return -1;
        v.bit_ticks=r.bit_ticks; v.zero_ticks=r.zero_ticks; v.one_ticks=r.one_ticks;
    }
    v.clock_hz=hz; v.profile=profile; v.reset_ns=reset_us*1000u;
    v.t0h_ns=(uint16_t)(((uint64_t)v.zero_ticks*1000000000u+hz/2u)/hz);
    v.t1h_ns=(uint16_t)(((uint64_t)v.one_ticks*1000000000u+hz/2u)/hz);
    v.t0l_ns=(uint16_t)(((uint64_t)(v.bit_ticks-v.zero_ticks)*1000000000u+hz/2u)/hz);
    v.t1l_ns=(uint16_t)(((uint64_t)(v.bit_ticks-v.one_ticks)*1000000000u+hz/2u)/hz);
    *t=v; return 0;
}

int ultra_timing_check(const ultra_timing_t *t)
{
    ultra_timing_t expected;
    if (!t || ultra_timing_profile(&expected,t->profile,t->clock_hz,t->reset_ns/1000u+(t->reset_ns%1000u!=0u))) return -1;
    return t->t0h_ns==expected.t0h_ns && t->t0l_ns==expected.t0l_ns &&
        t->t1h_ns==expected.t1h_ns && t->t1l_ns==expected.t1l_ns && t->zero_ticks==expected.zero_ticks && t->one_ticks==expected.one_ticks &&
        t->bit_ticks==expected.bit_ticks && t->reset_ns>=500000u && t->reset_ns<=ULTRA_RESET_MAX_NS ? 0:-1;
}

/* Round only the final edge, never individual fractional HIGH/LOW durations. */
uint64_t ultra_wire_ns(const ultra_timing_t *t, const uint8_t *bytes, uint32_t n)
{
    uint64_t ticks;
    if (!n || !t || !t->clock_hz) return 0;
    ticks=((uint64_t)n*8u-1u)*t->bit_ticks+((bytes[n-1u]&1u)?t->one_ticks:t->zero_ticks);
    return ultra_udiv64_(ticks*1000000000u+t->clock_hz/2u,t->clock_hz);
}
int64_t ultra_wire_end_us(const ultra_timing_t *t,const uint8_t *bytes,uint32_t n,int64_t start)
{
    uint64_t ticks;
    if(!n || !t || !t->clock_hz)return start;
    ticks=((uint64_t)n*8u-1u)*t->bit_ticks+((bytes[n-1u]&1u)?t->one_ticks:t->zero_ticks);
    return start+(int64_t)ultra_udiv64_(ticks*1000000u+t->clock_hz-1u,t->clock_hz);
}

uint32_t ultra_frame_max_us(const ultra_timing_t *t, uint32_t bytes)
{
    uint64_t ticks=(uint64_t)bytes*8u*t->bit_ticks;
    return (uint32_t)ultra_udiv64_(ticks*1000000u+t->clock_hz-1u,t->clock_hz)+t->reset_ns/1000u+(t->reset_ns%1000u!=0u);
}

uint32_t ultra_encode_pulses(const ultra_timing_t *t, const uint8_t *bytes, uint32_t nbytes, uint32_t *pos,
                             ultra_pulse_t *out, uint32_t max)
{
    const uint32_t end = nbytes * 8u;
    uint32_t p = *pos, k = 0;
    while (k < max && p < end) {
        const int one = (bytes[p >> 3] >> (7u - (p & 7u))) & 1;
        out[k].high_ticks = one ? t->one_ticks : t->zero_ticks;
        out[k].low_ticks = t->bit_ticks - out[k].high_ticks;
        if (p + 1u == end) {
            out[k].low_ticks += (uint32_t)(((uint64_t)t->reset_ns*t->clock_hz+999999999u)/1000000000u);                /* the reset gap after the last bit */
        }
        k++;
        p++;
    }
    *pos = p;
    return k;
}

int ultra_rmt_init(ultra_rmt_t *r, const ultra_timing_t *t, uint32_t resolution_hz)
{
    ultra_rmt_t v={0};
    uint32_t rst, a, b;
    if (!r || !t || ultra_timing_check(t) ||
        ultra_timing_profile(&v.effective,t->profile,resolution_hz,t->reset_ns/1000u+(t->reset_ns%1000u!=0u))) return -1;
    if (resolution_hz!=40000000u && resolution_hz!=80000000u) return -1;
    rst=(uint32_t)(((uint64_t)v.effective.reset_ns*resolution_hz+999999999u)/1000000000u);
    a=rst/2u; b=rst-a;
    if (!a || b>32767u) return -1;
    v.bit0=ULTRA_RMT_WORD(v.effective.zero_ticks,1,v.effective.bit_ticks-v.effective.zero_ticks,0);
    v.bit1=ULTRA_RMT_WORD(v.effective.one_ticks,1,v.effective.bit_ticks-v.effective.one_ticks,0);
    v.reset=ULTRA_RMT_WORD(a,0,b,0);
    *r=v; return 0;
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
uint32_t ultra_rmt_encode_rw(const ultra_rmt_t *r,const uint8_t *bytes,uint32_t n,uint32_t pos,
                             uint32_t *out,uint32_t max,int *done)
{
    uint32_t end=n*8u,k=0;
    *done=0;
    while(k<max && pos<end){
        uint32_t word=(bytes[pos>>3]&(0x80u>>(pos&7u)))?r->bit1:r->bit0;
        if(++pos==end){word=(word&65535u)|((r->effective.clock_hz/12500u)<<16);*done=1;}
        out[k++]=word;
    }
    if(pos==end)*done=1;
    return k;
}

int ultra_spi_timing(uint32_t hz, ultra_timing_t *t)
{
    return hz==9600000u ? ultra_timing_profile(t,0,hz,500) : -1;
}

uint32_t ultra_spi_encode(const uint8_t *bytes, uint32_t nbytes, uint8_t *out)
{
    uint32_t i, bit, j, pos=0;
    for(i=0;i<nbytes*12u;++i) out[i]=0;
    for(i=0;i<nbytes*8u;++i) {
        bit=(bytes[i>>3]>>(7u-(i&7u)))&1u;
        for(j=0;j<12u;++j,++pos)
            if(j<(bit?9u:3u)) out[pos>>3]|=(uint8_t)(0x80u>>(pos&7u));
    }
    return nbytes*12u;
}
