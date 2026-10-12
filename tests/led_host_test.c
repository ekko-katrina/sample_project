#include "led.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static int fail_stage, timeout_ms, transmit_count, wait_count, deleted_channels, deleted_encoders;
static const uint8_t *payload;
void *memcpy(void *destination, const void *source, size_t size)
{
    for (size_t i = 0; i < size; i++) ((uint8_t *)destination)[i] = ((const uint8_t *)source)[i];
    return destination;
}
void *memset(void *destination, int value, size_t size)
{
    for (size_t i = 0; i < size; i++) ((uint8_t *)destination)[i] = (uint8_t)value;
    return destination;
}

esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *handle)
{
    (void)config;
    if (fail_stage == 1) return ESP_ERR_NO_MEM;
    *handle = (void *)1; return ESP_OK;
}
esp_err_t rmt_new_bytes_encoder(const rmt_bytes_encoder_config_t *config, rmt_encoder_handle_t *handle)
{
    (void)config;
    if (fail_stage == 2) return ESP_ERR_NO_MEM;
    *handle = (void *)2; return ESP_OK;
}
esp_err_t rmt_enable(rmt_channel_handle_t channel)
{
    (void)channel; return fail_stage == 3 ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t rmt_del_channel(rmt_channel_handle_t channel) { (void)channel; deleted_channels++; return ESP_OK; }
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder) { (void)encoder; deleted_encoders++; return ESP_OK; }
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t encoder, const void *data, size_t size, const rmt_transmit_config_t *config)
{
    (void)channel; (void)encoder; (void)size; (void)config;
    transmit_count++; payload = data;
    return fail_stage == 4 ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int timeout)
{
    (void)channel; timeout_ms = timeout; wait_count++;
    return fail_stage == 5 ? ESP_ERR_TIMEOUT : ESP_OK;
}

int mainCRTStartup(void)
{
    CHECK(led_set_rgb(1, 2, 3) == ESP_ERR_INVALID_STATE && transmit_count == 0);
    fail_stage = 1;
    CHECK(led_init_checked() == ESP_ERR_NO_MEM);
    fail_stage = 2;
    CHECK(led_init_checked() == ESP_ERR_NO_MEM && deleted_channels == 1);
    fail_stage = 3;
    CHECK(led_init_checked() == ESP_ERR_INVALID_STATE && deleted_channels == 2 && deleted_encoders == 1);
#if defined(LED_TEST_INIT_TIMEOUT)
    fail_stage = 5;
    CHECK(led_init_checked() == ESP_ERR_TIMEOUT);
    CHECK(timeout_ms >= 100);
    CHECK(led_set_rgb(1, 2, 3) == ESP_ERR_INVALID_STATE && transmit_count == 1);
#else
    fail_stage = 0;
    CHECK(led_init_checked() == ESP_OK && timeout_ms >= 100);
    CHECK(led_set_rgb(0x12, 0x34, 0x56) == ESP_OK);
    CHECK(payload[0] == 0x34 && payload[1] == 0x12 && payload[2] == 0x56);
#if defined(LED_TEST_TRANSMIT_FAILURE)
    fail_stage = 4;
    CHECK(led_set_rgb(5, 6, 7) == ESP_ERR_INVALID_STATE);
    CHECK(wait_count == 2);
#else
    fail_stage = 5;
    CHECK(led_set_rgb(5, 6, 7) == ESP_ERR_TIMEOUT);
#endif
    int before = transmit_count;
    CHECK(led_set_rgb(8, 9, 10) == ESP_ERR_INVALID_STATE && transmit_count == before);
    CHECK(payload[0] == 6 && payload[1] == 5 && payload[2] == 7);
#endif
    return 0;
}
