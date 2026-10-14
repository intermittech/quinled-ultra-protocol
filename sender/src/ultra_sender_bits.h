/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: bit timing and output encoders. Independent of the scheduler (ultra_sender.h): it turns the
 * bytes of a frame into what a peripheral sends.
 *
 * The line (wiki: Physical layer): WS28xx-style pulses, MSB first, idle LOW. Every bit starts with a rising edge, and
 * the length of the HIGH pulse is the bit value; the LOW after it only separates the bits.
 *
 *              allowed               recommended (ULTRA_TIMING_RECOMMENDED)
 *   '0' high   220-380 ns            250 ns   (centred on the boards' 472 ns decision: long chains keep their margin)
 *   '0' low    0.45-10 us            1050 ns
 *   '1' high   580 ns - 2 us         700 ns
 *   '1' low    0.45-10 us            650 ns
 *   reset      >= 280 us             300 us   (LOW after every frame)
 *
 * NO GAP INSIDE A FRAME: a LOW of 50 us or more inside a frame ends the board's capture early (its slot is dropped),
 * one of 100-150 us ends the frame. Send each frame in one go: DMA, a peripheral FIFO refilled well within 50 us
 * (ESP32 RMT: enough memory blocks, short interrupts), or SPI / I2S with DMA. Never bit-bang with interrupts on.
 *
 * Encoders here:
 *  - ultra_encode_pulses: {high_ns, low_ns} per bit, for any timer / PIO / bit-bang driver;
 *  - ultra_rmt_*: 32-bit words in the ESP32 RMT symbol layout (rmt_symbol_word_t, Arduino-ESP32's rmt_data_t), in
 *    chunks, so an RMT encoder callback can refill from the frame bytes without a symbol buffer;
 *  - ultra_spi_encode: 5 SPI bits per frame bit at 4 MHz (MOSI only, DMA), for SPI / I2S / PIO byte streams.
 *
 * THE SCHEDULER'S TIMING = THE WIRE'S: the line computes every frame's TIME from its bits with the timing in
 * ultra_sender_config_t, so that timing must be the one the output really produces. ultra_rmt_init() and
 * ultra_spi_timing() return it: set it in the config before ultra_sender_init().
 */
#ifndef ULTRA_SENDER_BITS_H
#define ULTRA_SENDER_BITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* bit timing on the wire, ns (reset: the LOW after the frame) */
typedef struct {
    uint16_t t0h_ns, t0l_ns;          /* '0': high, low */
    uint16_t t1h_ns, t1l_ns;          /* '1': high, low */
    uint32_t reset_ns;                /* >= 280000 */
} ultra_timing_t;

#define ULTRA_TIMING_RECOMMENDED { 250u, 1050u, 700u, 650u, 300000u }
/* the spec's limits (wiki: Physical layer) */
#define ULTRA_T0H_MIN_NS   220u
#define ULTRA_T0H_MAX_NS   380u
#define ULTRA_T1H_MIN_NS   580u
#define ULTRA_T1H_MAX_NS   2000u
#define ULTRA_TL_MIN_NS    450u
#define ULTRA_TL_MAX_NS    10000u
#define ULTRA_RESET_MIN_NS 280000u
#define ULTRA_GAP_MAX_US   50u         /* a LOW inside a frame must stay below this */

/* 0 = every value inside the spec's ranges, else -1 */
int      ultra_timing_check(const ultra_timing_t *t);
/* the frame on the wire: from the first bit's rising edge to the last bit's falling edge, ns (the content decides:
 * '0' and '1' have different lengths). The header's TIME is the frame's start + this. */
uint64_t ultra_wire_ns(const ultra_timing_t *t, const uint8_t *bytes, uint32_t n);
/* the longest a frame of `bytes` can take, every bit at the longer bit period, plus the reset gap, us */
uint32_t ultra_frame_max_us(const ultra_timing_t *t, uint32_t bytes);

/* ------------------------------------------------------------------------------------------- generic pulses */
typedef struct {
    uint32_t high_ns, low_ns;
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

/* the three symbols for an RMT channel at resolution_hz (10-80 MHz typical), each duration rounded to whole ticks;
 * 0 = ok, -1 = a duration doesn't fit 15 bits, or the rounded timing leaves the spec's ranges (e.g. 220 ns at 10 MHz
 * is 200 ns: pick a timing that ticks represent, or a higher resolution). r->effective = the timing sent. */
int      ultra_rmt_init(ultra_rmt_t *r, const ultra_timing_t *t, uint32_t resolution_hz);
/* Symbols from symbol `pos` of the frame on (pos = symbols already written: the frame's bits, then one reset
 * symbol), up to `max`; returns how many it wrote and sets *done once the reset symbol is out. The shape of
 * ESP-IDF's rmt_simple_encoder callback (symbols_written, symbols_free, symbols, done). */
uint32_t ultra_rmt_encode(const ultra_rmt_t *r, const uint8_t *bytes, uint32_t nbytes, uint32_t pos, uint32_t *out,
                          uint32_t max, int *done);

/* ------------------------------------------------------------------------------------------- SPI / I2S */
/* 5 SPI bits per frame bit, MSB first: '0' = 10000, '1' = 11100. At 4 MHz (250 ns per SPI bit): '0' 250 / 1000 ns,
 * '1' 750 / 500 ns. Any SPI clock from 2.7 to 4.4 MHz stays inside the spec. Each frame byte becomes 5 SPI bytes.
 * MOSI must idle LOW. The reset: keep MOSI LOW >= 280 us after the frame (ULTRA_SPI_RESET_BYTES(hz) zero bytes, or
 * wait). The scheduler's timing for it: ultra_spi_timing(). */
#define ULTRA_SPI_HZ             4000000u
#define ULTRA_SPI_BYTES(n)       ((n) * 5u)
#define ULTRA_SPI_RESET_BYTES(hz) ((uint32_t)(((uint64_t)(hz) * 300u + 7999999u) / 8000000u))   /* >= 300 us */
/* frame bytes -> SPI bytes (out: ULTRA_SPI_BYTES(nbytes)); returns the SPI byte count */
uint32_t ultra_spi_encode(const uint8_t *bytes, uint32_t nbytes, uint8_t *out);
/* the timing ultra_spi_encode() produces at an SPI clock of spi_hz (the real one, after the peripheral's divider),
 * reset 300 us: 0 = inside the spec, -1 = outside it (the clock outside about 2.7-4.45 MHz; *t unchanged) */
int      ultra_spi_timing(uint32_t spi_hz, ultra_timing_t *t);

#ifdef __cplusplus
}
#endif

#endif
