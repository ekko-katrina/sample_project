#ifndef LED_TEST_RMT_TX_H
#define LED_TEST_RMT_TX_H
#include "rmt_common.h"
typedef struct {
    int clk_src, gpio_num;
    unsigned int mem_block_symbols, resolution_hz, trans_queue_depth;
} rmt_tx_channel_config_t;
typedef struct {
    int loop_count;
    struct { unsigned int eot_level; } flags;
} rmt_transmit_config_t;
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *, rmt_channel_handle_t *);
esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t, const void *, size_t, const rmt_transmit_config_t *);
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int);
#endif
