/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: bit timing and output encoders. Independent of the scheduler (ultra_sender.h): it turns the
 * bytes of a frame into what a peripheral sends.
 *
 * Exact profiles 800/600/400/200 use 9.6 MHz sample counts or qualified
 * 40/80 MHz RMT ticks. TIME uses the actual rational clock and final falling
 * edge. Keep HIGH/LOW constant for every bit; no dither. See docs/timing.md.
 * pulse outputs contain ticks, not independently rounded nanoseconds.
 */
#ifndef ULTRA_SENDER_BITS_H
#define ULTRA_SENDER_BITS_H

#include <stdint.h>
#include "ultra_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exact backend recipe. The ns fields are rounded display values only. */
typedef struct {
    uint16_t t0h_ns, t0l_ns;          /* '0': high, low */
    uint16_t t1h_ns, t1l_ns;          /* '1': high, low */
    uint32_t reset_ns;                /* 500000 .. ULTRA_RESET_MAX_NS */
    uint32_t clock_hz;                /* authoritative physical clock */
    uint16_t zero_ticks, one_ticks, bit_ticks;
    uint8_t profile;                   /* 0..3, one immutable recipe per frame */
} ultra_timing_t;
static inline uint32_t ultra_timing_gap_us(const ultra_timing_t *t)
{ return t->reset_ns/1000u+(t->reset_ns%1000u!=0u); }

#define ULTRA_TIMING_RECOMMENDED {313u,938u,938u,313u,500000u,9600000u,3u,9u,12u,0u}
/* Receiver acquisition envelope is specified per profile in docs/timing.md. */
#define ULTRA_RESET_MIN_NS 500000u
#define ULTRA_RESET_MAX_NS (0xFFFFFFFFu - 10000u)
#define ULTRA_GAP_MAX_US   10u         /* 10..280 us is invalid, not reset */

/* 0 = every value inside the spec's ranges, else -1 */
int ultra_timing_profile(ultra_timing_t *t, uint8_t profile, uint32_t clock_hz, uint32_t reset_us);
int      ultra_timing_check(const ultra_timing_t *t);
/* Every position needs at least 500 us processing LOW. */
#define ULTRA_CHAIN_FIRST_RESET_NS 500000u
int      ultra_timing_chain_ok(const ultra_timing_t *t, uint16_t board);
/* the frame on the wire: from the first bit's rising edge to the last bit's falling edge, ns (exact backend ticks). The header's TIME is the frame's start + this. */
uint64_t ultra_wire_ns(const ultra_timing_t *t, const uint8_t *bytes, uint32_t n);
/* Physical end rounded UP, for minimum gaps/deadlines. The encoded integer
 * TIME may round differently; never shorten a guard using that timestamp. */
int64_t ultra_wire_end_us(const ultra_timing_t *,const uint8_t *,uint32_t n,int64_t start_us);
/* the longest a frame of `bytes` can take, every bit at the longer bit period, plus the reset gap, us */
uint32_t ultra_frame_max_us(const ultra_timing_t *t, uint32_t bytes);

/* ------------------------------------------------------------------------------------------- generic pulses */
typedef struct {
    uint32_t high_ticks, low_ticks; /* divide by timing.clock_hz; no ns rounding */
} ultra_pulse_t;

/* Up to `max` pulses (one per bit, MSB first) from bit *pos of the frame on; advances *pos and returns the count
 * (0 when the frame is done). The last bit's low time includes the reset gap, so the pulses describe the whole line.
 * Call it in a loop to fill a DMA buffer chunk by chunk. */
uint32_t ultra_encode_pulses(const ultra_timing_t *t, const uint8_t *bytes, uint32_t nbytes, uint32_t *pos,
                             ultra_pulse_t *out, uint32_t max);

/* ------------------------------------------------------------------------------------------- ESP32 RMT */
/* An RMT symbol as uint32_t in the ESP32 layout: duration0 (15 bits) | level0 << 15 | duration1 << 16 | level1 << 31
 * (ESP-IDF rmt_symbol_word_t.val, Arduino-ESP32 3.x rmt_data_t.val, IDF 4.x rmt_item32_t.val) */
#define ULTRA_RMT_WORD(d0, l0, d1, l1) \
    ((uint32_t)((d0) & 0x7FFFu) | ((uint32_t)((l0) & 1u) << 15) | ((uint32_t)((d1) & 0x7FFFu) << 16) | \
     ((uint32_t)((l1) & 1u) << 31))

typedef struct {
    uint32_t bit0, bit1;              /* HIGH then LOW */
    uint32_t reset;                   /* LOW for the reset gap (two halves) */
    ultra_timing_t effective;         /* what the words send: whole ticks (give it to the scheduler) */
} ultra_rmt_t;

/* the three symbols for an RMT channel at resolution_hz (40 or 80 MHz only), each duration rounded to whole ticks;
 * 0 = ok, -1 = a duration doesn't fit 15 bits, or the rounded timing leaves the spec's ranges (e.g. 220 ns at 10 MHz
 * is 200 ns: pick a timing that ticks represent, or a higher resolution). r->effective = the timing sent. */
int      ultra_rmt_init(ultra_rmt_t *r, const ultra_timing_t *t, uint32_t resolution_hz);
/* Symbols from symbol `pos` of the frame on (pos = symbols already written: the frame's bits, then one reset
 * symbol), up to `max`; returns how many it wrote and sets *done once the reset symbol is out. The shape of
 * ESP-IDF's rmt_simple_encoder callback (symbols_written, symbols_free, symbols, done). */
uint32_t ultra_rmt_encode(const ultra_rmt_t *r, const uint8_t *bytes, uint32_t nbytes, uint32_t pos, uint32_t *out,
                          uint32_t max, int *done);
/* RW has no reset symbol: last falling edge then precisely 80 us LOW. Release
 * DE from an ISR/hardware event at completion, always within 60..150 us. */
uint32_t ultra_rmt_encode_rw(const ultra_rmt_t *,const uint8_t *,uint32_t nbytes,uint32_t pos,
                             uint32_t *out,uint32_t max,int *done);

/* ------------------------------------------------------------------------------------------- SPI / I2S */
/* Single-lane exact 800 profile, 12 samples per bit at actual 9.6 MHz.
 * For all profiles and parallel lanes use ultra_sender_profile.h. */
#define ULTRA_SPI_HZ             9600000u
#define ULTRA_SPI_BYTES(n)       ((n) * 12u)
#define ULTRA_SPI_RESET_BYTES(hz) ((uint32_t)(((uint64_t)(hz) * 500u + 7999999u) / 8000000u))   /* >= 500 us */
/* frame bytes -> SPI bytes (out: ULTRA_SPI_BYTES(nbytes)); returns the SPI byte count */
uint32_t ultra_spi_encode(const uint8_t *bytes, uint32_t nbytes, uint8_t *out);
/* Only actual 9.6 MHz is accepted; 500 us reset. */
int      ultra_spi_timing(uint32_t spi_hz, ultra_timing_t *t);

#ifdef __cplusplus
}
#endif

#endif
