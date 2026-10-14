/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * QuinLED Ultra sender: an ESP32 RMT output for one line, header-only (ESP-IDF >= 5.3, Arduino-ESP32 >= 3.1). Only
 * for the ESP32 family: nothing compiles it unless it is included.
 * The frame bytes go straight into the RMT memory through a simple-encoder callback, so no symbol buffer is needed
 * at any board count.
 *
 *   #include "ultra_esp32_rmt.h"
 *   ultra_esp32_rmt_t out;
 *   ultra_sender_config_init(&cfg, boards, lights);
 *   ultra_esp32_rmt_begin(&out, GPIO_NUM_16, &cfg.timing);    // first: cfg.timing becomes what the RMT sends
 *   ultra_sender_init(&line, &cfg, ...);
 *   ... uint32_t n = ultra_sender_next_frame(&line, esp_timer_get_time(), buf);
 *       if (n) r = ultra_esp32_rmt_send(&out, buf, n);           // returns once the frame and its reset gap are out
 *
 * No gap >= 50 us inside a frame: the RMT memory (ULTRA_RMT_SYMBOLS) is refilled from its interrupt, half a memory at
 * a time (~60-125 us of bits). The callback is in IRAM; keep other interrupts short. Flash writes (OTA, NVS) pause
 * interrupts that aren't IRAM-safe (unless CONFIG_RMT_ISR_IRAM_SAFE): a frame sent meanwhile can break; the boards drop
 * it, and the copies and the refresh make up for it. On an ESP32-S3 / P4, ULTRA_RMT_DMA 1 sends from DMA instead.
 * Not for return-channel RW frames: the hub must release the line within 150 us, this appends 300 us of reset.
 * Level: the dig2analog-ultra wants 5 V data (a 74AHCT125-type buffer).
 */
#ifndef ULTRA_ESP32_RMT_H
#define ULTRA_ESP32_RMT_H

#ifndef ESP_PLATFORM
#error "ultra_esp32_rmt.h is for the ESP32 (ESP-IDF >= 5.3 or Arduino-ESP32 >= 3.1); elsewhere use ultra_sender_bits.h"
#endif

#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "soc/soc_caps.h"

#include "ultra_sender_bits.h"

#ifndef ULTRA_RMT_RESOLUTION_HZ
#define ULTRA_RMT_RESOLUTION_HZ 40000000u      /* 25 ns ticks: 250 / 1050 / 700 / 650 ns = 10 / 42 / 28 / 26 */
#endif
#ifndef ULTRA_RMT_SYMBOLS
#if SOC_RMT_MEM_WORDS_PER_CHANNEL >= 64
#define ULTRA_RMT_SYMBOLS 192u                 /* ESP32 / S2: 3 of 8 blocks of 64 */
#else
#define ULTRA_RMT_SYMBOLS 96u                  /* S3 / C3 / C6 / H2: 2 blocks of 48 */
#endif
#endif
#ifndef ULTRA_RMT_DMA
#define ULTRA_RMT_DMA 0
#endif
#ifndef ULTRA_RMT_WAIT_MS
#define ULTRA_RMT_WAIT_MS 200                  /* a frame (64 boards: ~23 ms with its reset) takes far less */
#endif

/* ultra_esp32_rmt_send() */
#define ULTRA_RMT_OK        0                  /* the frame and its reset gap are out */
#define ULTRA_RMT_NOT_SENT  (-1)               /* not taken: nothing went out, the buffer is free (retime, send again) */
#define ULTRA_RMT_ABORTED   (-2)               /* taken but not done in time: stopped; part of it may have gone out (the
                                                  boards drop it); the buffer is free (retime, send again) */
#define ULTRA_RMT_STUCK     (-3)               /* couldn't be stopped: the driver may still read the buffer; don't touch
                                                  it; ultra_esp32_rmt_end() (which may fail too: see there) */

/* Lifetime: the driver's interrupt reads this struct (the callback's argument) and the frame being sent, so both stay
 * in place (static, or never freed) until ultra_esp32_rmt_end() returns 0. */
typedef struct {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    ultra_rmt_t words;
    uint8_t enabled;                           /* the channel is enabled (rmt_disable() only then) */
} ultra_esp32_rmt_t;

/* the simple encoder's callback (in IRAM, the whole path): the frame's bits, then one reset symbol */
static size_t IRAM_ATTR ultra_esp32_rmt_cb(const void *data, size_t size, size_t written, size_t free_syms,
                                           rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    const ultra_esp32_rmt_t *o = (const ultra_esp32_rmt_t *)arg;
    const uint8_t *bytes = (const uint8_t *)data;
    const size_t end = size * 8u;
    size_t k = 0, pos = written;
    while (k < free_syms) {
        if (pos < end) {
            symbols[k++].val = ((bytes[pos >> 3] >> (7u - (pos & 7u))) & 1u) ? o->words.bit1 : o->words.bit0;
            pos++;
        } else {
            symbols[k++].val = o->words.reset;
            *done = true;
            break;
        }
    }
    return k;
}

/* Stop and release the output. 0 = released: o and the frame buffer are free (begin() sets it up again). -1 = the
 * channel couldn't be stopped, or a transaction is still in flight: nothing that may still be in use is released and
 * the handles stay, as do o and the frame buffer (the interrupt may still read them): don't send, call again later
 * (it continues where it stopped), or restart. */
static inline int ultra_esp32_rmt_end(ultra_esp32_rmt_t *o)
{
    if (o->chan != NULL) {
        if (o->enabled) {
            if (rmt_disable(o->chan) != ESP_OK) {
                return -1;
            }
            o->enabled = 0;
        }
        if (rmt_tx_wait_all_done(o->chan, 0) != ESP_OK) {    /* the stop handed every transaction back */
            return -1;
        }
    }
    if (o->enc != NULL) {
        if (rmt_del_encoder(o->enc) != ESP_OK) {
            return -1;
        }
        o->enc = NULL;
    }
    if (o->chan != NULL) {
        if (rmt_del_channel(o->chan) != ESP_OK) {
            return -1;
        }
        o->chan = NULL;
    }
    return 0;
}

/* 0 = ok. *t: the timing asked for; on return the timing the RMT really sends (whole ticks): give it to the line
 * (ultra_sender_config_t.timing) before ultra_sender_init(). -1: nothing is left allocated. o: not set up, or
 * released by ultra_esp32_rmt_end() (begin() overwrites it). */
static inline int ultra_esp32_rmt_begin(ultra_esp32_rmt_t *o, int gpio, ultra_timing_t *t)
{
    rmt_tx_channel_config_t ch;
    rmt_simple_encoder_config_t ec;
    memset(o, 0, sizeof *o);
    memset(&ch, 0, sizeof ch);
    memset(&ec, 0, sizeof ec);
    if (ultra_rmt_init(&o->words, t, ULTRA_RMT_RESOLUTION_HZ) != 0) {
        return -1;
    }
    ch.gpio_num = (gpio_num_t)gpio;
    ch.clk_src = RMT_CLK_SRC_DEFAULT;
    ch.resolution_hz = ULTRA_RMT_RESOLUTION_HZ;
    ch.mem_block_symbols = ULTRA_RMT_SYMBOLS;
    ch.trans_queue_depth = 1;
    ch.flags.with_dma = ULTRA_RMT_DMA;
    ec.callback = ultra_esp32_rmt_cb;
    ec.arg = o;
    ec.min_chunk_size = 1;
    if (rmt_new_tx_channel(&ch, &o->chan) != ESP_OK) {
        o->chan = NULL;
        return -1;
    }
    if (rmt_new_simple_encoder(&ec, &o->enc) != ESP_OK) {
        o->enc = NULL;
        (void)ultra_esp32_rmt_end(o);
        return -1;
    }
    if (rmt_enable(o->chan) != ESP_OK) {
        (void)ultra_esp32_rmt_end(o);            /* never enabled, nothing sent: releases both */
        return -1;
    }
    o->enabled = 1;
    *t = o->words.effective;
    return 0;
}

/* Send one frame (the bytes stay untouched until it is out) and wait for it and its reset gap: ULTRA_RMT_*. */
static inline int ultra_esp32_rmt_send(ultra_esp32_rmt_t *o, const uint8_t *frame, uint32_t len)
{
    rmt_transmit_config_t tx;
    memset(&tx, 0, sizeof tx);
    tx.loop_count = 0;
    tx.flags.eot_level = 0;                    /* idle LOW */
    if (o->chan == NULL || !o->enabled || rmt_transmit(o->chan, o->enc, frame, len, &tx) != ESP_OK) {
        return ULTRA_RMT_NOT_SENT;
    }
    if (rmt_tx_wait_all_done(o->chan, ULTRA_RMT_WAIT_MS) == ESP_OK) {
        return ULTRA_RMT_OK;
    }
    /* still in flight after far longer than a frame: stop the channel (the driver recycles the transaction), take it
     * back, start again; only then is the buffer the caller's again */
    if (rmt_disable(o->chan) != ESP_OK) {
        return ULTRA_RMT_STUCK;
    }
    o->enabled = 0;
    if (rmt_tx_wait_all_done(o->chan, 0) != ESP_OK || rmt_enable(o->chan) != ESP_OK) {
        return ULTRA_RMT_STUCK;
    }
    o->enabled = 1;
    return ULTRA_RMT_ABORTED;
}

#endif
