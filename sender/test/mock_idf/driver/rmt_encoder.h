/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * A mock of the ESP-IDF 5.x RMT encoder API (the simple encoder) for test_rmt.c.
 */
#ifndef MOCK_DRIVER_RMT_ENCODER_H
#define MOCK_DRIVER_RMT_ENCODER_H

#include "driver/rmt_tx.h"

typedef size_t (*rmt_encode_simple_cb_t)(const void *data, size_t data_size, size_t symbols_written,
                                         size_t symbols_free, rmt_symbol_word_t *symbols, bool *done, void *arg);

typedef struct {
    rmt_encode_simple_cb_t callback;
    void *arg;
    size_t min_chunk_size;
} rmt_simple_encoder_config_t;

esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder);

#endif
