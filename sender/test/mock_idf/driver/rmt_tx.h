/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 QuinLED
 *
 * A mock of the ESP-IDF 5.x RMT TX driver: just what ultra_esp32_rmt.h uses, for test_rmt.c on a PC. The calls are
 * recorded and their results scripted (test_rmt.c implements them). The types follow ESP-IDF 5.5's.
 */
#ifndef MOCK_DRIVER_RMT_TX_H
#define MOCK_DRIVER_RMT_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        (-1)
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107

typedef int gpio_num_t;
typedef int rmt_clock_source_t;
#define RMT_CLK_SRC_DEFAULT 4

typedef union {
    struct {
        uint32_t duration0 : 15;
        uint32_t level0 : 1;
        uint32_t duration1 : 15;
        uint32_t level1 : 1;
    } f;
    uint32_t val;
} rmt_symbol_word_t;

typedef struct mock_rmt_channel *rmt_channel_handle_t;
typedef struct mock_rmt_encoder *rmt_encoder_handle_t;

typedef struct {
    gpio_num_t gpio_num;
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    size_t mem_block_symbols;
    size_t trans_queue_depth;
    int intr_priority;
    struct {
        uint32_t invert_out : 1;
        uint32_t with_dma : 1;
        uint32_t io_loop_back : 1;
        uint32_t io_od_mode : 1;
        uint32_t allow_pd : 1;
    } flags;
} rmt_tx_channel_config_t;

typedef struct {
    int loop_count;
    struct {
        uint32_t eot_level : 1;
        uint32_t queue_nonblocking : 1;
    } flags;
} rmt_transmit_config_t;

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *ret_chan);
esp_err_t rmt_del_channel(rmt_channel_handle_t channel);
esp_err_t rmt_enable(rmt_channel_handle_t channel);
esp_err_t rmt_disable(rmt_channel_handle_t channel);
esp_err_t rmt_transmit(rmt_channel_handle_t tx_channel, rmt_encoder_handle_t encoder, const void *payload,
                       size_t payload_bytes, const rmt_transmit_config_t *config);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t tx_channel, int timeout_ms);

#endif
