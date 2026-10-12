#ifndef LED_TEST_RMT_COMMON_H
#define LED_TEST_RMT_COMMON_H
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *rmt_channel_handle_t;
typedef void *rmt_encoder_handle_t;
#define RMT_CLK_SRC_DEFAULT 0
esp_err_t rmt_enable(rmt_channel_handle_t);
esp_err_t rmt_del_channel(rmt_channel_handle_t);
#endif
