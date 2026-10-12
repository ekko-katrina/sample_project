#ifndef LED_TEST_RMT_ENCODER_H
#define LED_TEST_RMT_ENCODER_H
#include "rmt_common.h"
typedef struct { unsigned int duration0, level0, duration1, level1; } test_rmt_bit_t;
typedef struct {
    test_rmt_bit_t bit0, bit1;
    struct { unsigned int msb_first; } flags;
} rmt_bytes_encoder_config_t;
esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *, rmt_encoder_handle_t *);
esp_err_t rmt_del_encoder(rmt_encoder_handle_t);
#endif
