/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * The ESP32 RMT helper (src/ultra_esp32_rmt.h) on a PC, against a mocked ESP-IDF RMT driver (test/mock_idf/):
 *  - the simple-encoder callback, in chunks, gives the same symbols as ultra_rmt_encode() and then the reset;
 *  - begin(): the effective timing handed back; a failure unwinds what it allocated (review round 7);
 *  - send(): taken and done = OK; not taken = NOT_SENT (nothing waits); taken but not done in time = the channel is
 *    stopped (the driver recycles the transaction) before the buffer is handed back: ABORTED; can't stop = STUCK
 *    (the buffer stays the driver's) (review round 7, finding 12);
 *  - end(): everything released once the channel has stopped; while it can't be stopped (or a transaction is still in
 *    flight) nothing is released and the handles stay (review re-check, N1). The mock refuses what ESP-IDF 5.5
 *    refuses: disabling a disabled channel, deleting an enabled one.
 */
#include <stdio.h>
#include <string.h>

#include "ultra_esp32_rmt.h"
#include "ultra_sender.h"

static int failures, checks;
#define CHECK(c, ...) \
    do { \
        checks++; \
        if (!(c)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        } else { \
            printf("ok   "); \
        } \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } while (0)

/* ------------------------------------------------------------------------------------------------- the mock */
struct mock_rmt_channel {
    int dummy;
};
struct mock_rmt_encoder {
    int dummy;
};
static struct mock_rmt_channel chan_obj;
static struct mock_rmt_encoder enc_obj;

static struct {
    rmt_tx_channel_config_t ch;
    rmt_simple_encoder_config_t enc;
    int channels, encoders, enabled;
    int fail_channel, fail_encoder, fail_enable, fail_transmit, fail_disable;
    int disable_keeps_flight;                    /* a stop that doesn't hand the transaction back */
    int waits_timeout;                           /* the next wait(s) with a timeout > 0 time out */
    int in_flight;                               /* a transaction the driver may still read */
    const uint8_t *payload;
    size_t payload_bytes;
    char log[256];
    uint32_t symbols[4096];
    size_t nsymbols;
} m;

static void log_call(const char *c)
{
    if (strlen(m.log) + strlen(c) + 2u < sizeof m.log) {
        strcat(m.log, c);
        strcat(m.log, " ");
    }
}

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *ret_chan)
{
    log_call("new_channel");
    if (m.fail_channel) {
        return ESP_FAIL;
    }
    m.ch = *config;
    m.channels++;
    *ret_chan = &chan_obj;
    return ESP_OK;
}

esp_err_t rmt_del_channel(rmt_channel_handle_t channel)
{
    (void)channel;
    log_call("del_channel");
    if (m.enabled) {
        return ESP_ERR_INVALID_STATE;            /* (ESP-IDF: only a disabled channel) */
    }
    m.channels--;
    return ESP_OK;
}

esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder)
{
    log_call("new_encoder");
    if (m.fail_encoder) {
        return ESP_FAIL;
    }
    m.enc = *config;
    m.encoders++;
    *ret_encoder = &enc_obj;
    return ESP_OK;
}

esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder)
{
    (void)encoder;
    log_call("del_encoder");
    m.encoders--;
    return ESP_OK;
}

esp_err_t rmt_enable(rmt_channel_handle_t channel)
{
    (void)channel;
    log_call("enable");
    if (m.fail_enable) {
        return ESP_FAIL;
    }
    m.enabled = 1;
    return ESP_OK;
}

esp_err_t rmt_disable(rmt_channel_handle_t channel)
{
    (void)channel;
    log_call("disable");
    if (!m.enabled) {
        return ESP_ERR_INVALID_STATE;            /* (ESP-IDF: only an enabled channel) */
    }
    if (m.fail_disable) {
        return ESP_FAIL;
    }
    m.enabled = 0;
    if (!m.disable_keeps_flight) {
        m.in_flight = 0;                         /* (ESP-IDF 5.5: stops the hardware, recycles the transaction) */
    }
    return ESP_OK;
}

/* the driver's encoding: the callback in chunks of 48 symbols until it says done */
esp_err_t rmt_transmit(rmt_channel_handle_t tx_channel, rmt_encoder_handle_t encoder, const void *payload,
                       size_t payload_bytes, const rmt_transmit_config_t *config)
{
    bool done = false;
    rmt_symbol_word_t chunk[48];
    (void)tx_channel;
    (void)encoder;
    (void)config;
    log_call("transmit");
    if (m.fail_transmit || !m.enabled) {
        return ESP_FAIL;
    }
    m.payload = (const uint8_t *)payload;
    m.payload_bytes = payload_bytes;
    m.nsymbols = 0;
    while (!done && m.nsymbols < 4000u) {
        size_t k, n = m.enc.callback(payload, payload_bytes, m.nsymbols, 48, chunk, &done, m.enc.arg);
        for (k = 0; k < n; k++) {
            m.symbols[m.nsymbols++] = chunk[k].val;
        }
    }
    m.in_flight = 1;
    return ESP_OK;
}

esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t tx_channel, int timeout_ms)
{
    (void)tx_channel;
    log_call(timeout_ms == 0 ? "wait0" : "wait");
    if (timeout_ms != 0 && m.waits_timeout > 0) {
        m.waits_timeout--;
        return ESP_ERR_TIMEOUT;
    }
    if (timeout_ms == 0 && m.in_flight) {
        return ESP_ERR_TIMEOUT;
    }
    m.in_flight = 0;
    return ESP_OK;
}

static void mock_reset(void)
{
    memset(&m, 0, sizeof m);
}

/* ------------------------------------------------------------------------------------------------- tests */
int main(void)
{
    static const ultra_timing_t rec = ULTRA_TIMING_RECOMMENDED;
    static ultra_sender_t line;
    static ultra_sender_board_t boards[3];
    static uint8_t frame[ULTRA_SENDER_FRAME_BYTES(3)];
    ultra_esp32_rmt_t out;
    ultra_timing_t t = rec;
    ultra_sender_config_t cfg;
    uint32_t ref[4096], n, len;
    int done = 0, r;

    /* begin */
    mock_reset();
    r = ultra_esp32_rmt_begin(&out, 16, &t);
    CHECK(r == 0 && m.channels == 1 && m.encoders == 1 && m.enabled, "begin: a channel, an encoder, enabled");
    CHECK(m.ch.gpio_num == 16 && m.ch.resolution_hz == ULTRA_RMT_RESOLUTION_HZ && m.ch.mem_block_symbols == 192u &&
          m.ch.trans_queue_depth == 1u, "begin: GPIO 16, 40 MHz, 192 symbols, one transaction");
    CHECK(memcmp(&t, &rec, sizeof t) == 0, "begin: the recommended timing is exact at 40 MHz (handed back unchanged)");

    /* a frame: the callback's symbols = ultra_rmt_encode(), then the reset */
    ultra_sender_config_init(&cfg, 3, 1);
    cfg.timing = t;
    (void)ultra_sender_init(&line, &cfg, boards, 0, 0);
    len = ultra_sender_next_frame(&line, 1000000, frame);
    m.log[0] = 0;
    r = ultra_esp32_rmt_send(&out, frame, len);
    n = ultra_rmt_encode(&out.words, frame, len, 0, ref, 4096, &done);
    CHECK(r == ULTRA_RMT_OK && m.nsymbols == len * 8u + 1u && n == m.nsymbols &&
          memcmp(ref, m.symbols, n * sizeof ref[0]) == 0 && m.symbols[n - 1] == out.words.reset,
          "send: %u symbols in chunks of 48, as ultra_rmt_encode, the reset last; OK", (unsigned)m.nsymbols);
    CHECK(strcmp(m.log, "transmit wait ") == 0, "send: transmit, wait (%s)", m.log);

    /* not taken: nothing waits, the buffer is free */
    m.fail_transmit = 1;
    m.log[0] = 0;
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_NOT_SENT && strcmp(m.log, "transmit ") == 0, "send: not taken: NOT_SENT, no wait (%s)", m.log);
    m.fail_transmit = 0;

    /* taken, not done in time: stopped before the buffer is handed back */
    m.waits_timeout = 1;
    m.log[0] = 0;
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_ABORTED && strcmp(m.log, "transmit wait disable wait0 enable ") == 0 && !m.in_flight &&
          m.enabled, "send: a stall: disable (the transaction recycled), enable: ABORTED, nothing in flight (%s)", m.log);
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_OK, "send: and the next frame goes out");

    /* can't be stopped: STUCK, the buffer stays the driver's */
    m.waits_timeout = 1;
    m.fail_disable = 1;
    m.log[0] = 0;
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_STUCK && m.in_flight, "send: a stall that can't be stopped: STUCK, still in flight (%s)", m.log);
    /* end() while it still can't be stopped: nothing released, the handles kept (the interrupt may still use them) */
    m.log[0] = 0;
    r = ultra_esp32_rmt_end(&out);
    CHECK(r != 0 && m.in_flight && m.encoders == 1 && m.channels == 1 && out.chan != NULL && out.enc != NULL,
          "end while the stop fails: -1, encoder, channel and handles kept (%s)", m.log);
    r = ultra_esp32_rmt_end(&out);
    CHECK(r != 0 && m.encoders == 1 && out.enc != NULL, "end again, still failing: still kept");
    m.fail_disable = 0;

    /* end: everything released */
    m.log[0] = 0;
    r = ultra_esp32_rmt_end(&out);
    CHECK(r == 0 && m.channels == 0 && m.encoders == 0 && !m.in_flight && out.chan == NULL && out.enc == NULL,
          "end once it stops: 0, channel and encoder released (%s)", m.log);
    CHECK(ultra_esp32_rmt_end(&out) == 0, "end again: nothing left, 0");
    CHECK(ultra_esp32_rmt_send(&out, frame, len) == ULTRA_RMT_NOT_SENT, "send after end: NOT_SENT");

    /* begin failures unwind */
    mock_reset();
    m.fail_encoder = 1;
    t = rec;
    CHECK(ultra_esp32_rmt_begin(&out, 4, &t) != 0 && m.channels == 0 && m.encoders == 0 && out.chan == NULL,
          "begin: the encoder fails: the channel is released again (%s)", m.log);
    mock_reset();
    m.fail_enable = 1;
    CHECK(ultra_esp32_rmt_begin(&out, 4, &t) != 0 && m.channels == 0 && m.encoders == 0,
          "begin: enable fails: both released (%s)", m.log);
    mock_reset();
    m.fail_channel = 1;
    CHECK(ultra_esp32_rmt_begin(&out, 4, &t) != 0 && m.encoders == 0 && out.chan == NULL,
          "begin: the channel fails: nothing allocated");
    mock_reset();
    t = rec;
    t.reset_ns = 2000000u;                       /* 2 ms: 80000 ticks, more than two 15-bit halves */
    CHECK(ultra_esp32_rmt_begin(&out, 4, &t) != 0 && m.channels == 0 && strlen(m.log) == 0u,
          "begin: a timing the RMT can't send: refused before allocating");

    /* a stop that leaves the transaction in flight: STUCK; end() keeps everything (and doesn't disable the disabled
     * channel again) until the transaction is handed back */
    mock_reset();
    t = rec;
    (void)ultra_esp32_rmt_begin(&out, 4, &t);
    m.waits_timeout = 1;
    m.disable_keeps_flight = 1;
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_STUCK && m.in_flight && !m.enabled, "send: stopped, still in flight: STUCK");
    m.log[0] = 0;
    r = ultra_esp32_rmt_end(&out);
    CHECK(r != 0 && m.encoders == 1 && m.channels == 1 && out.enc != NULL && strcmp(m.log, "wait0 ") == 0,
          "end while in flight: -1, nothing deleted, no second disable (%s)", m.log);
    m.in_flight = 0;                             /* the transaction comes back */
    m.log[0] = 0;
    r = ultra_esp32_rmt_end(&out);
    CHECK(r == 0 && m.encoders == 0 && m.channels == 0 && strcmp(m.log, "wait0 del_encoder del_channel ") == 0,
          "end then: released (%s)", m.log);

    /* stopped and handed back, but enable fails: STUCK, nothing in flight; end() releases without a second disable */
    mock_reset();
    t = rec;
    (void)ultra_esp32_rmt_begin(&out, 4, &t);
    m.waits_timeout = 1;
    m.fail_enable = 1;
    r = ultra_esp32_rmt_send(&out, frame, len);
    CHECK(r == ULTRA_RMT_STUCK && !m.in_flight, "send: stopped but not enabled again: STUCK, nothing in flight");
    m.log[0] = 0;
    r = ultra_esp32_rmt_end(&out);
    CHECK(r == 0 && m.encoders == 0 && m.channels == 0 && strcmp(m.log, "wait0 del_encoder del_channel ") == 0,
          "end: released, the disabled channel not disabled again (%s)", m.log);
    CHECK(ultra_esp32_rmt_send(&out, frame, len) == ULTRA_RMT_NOT_SENT, "send after that: NOT_SENT");

    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "ALL RMT HELPER TESTS PASSED", checks, failures);
    return failures ? 1 : 0;
}
