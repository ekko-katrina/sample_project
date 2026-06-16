#include "led.h"

#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"

#define WS2812_RESOLUTION_HZ 10000000
#define WS2812_RESET_MS 1

static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_encoder;
static uint8_t led_pixels[3];
static uint32_t led_is_on;

static void led_refresh(uint8_t red, uint8_t green, uint8_t blue)
{
    led_pixels[0] = green;
    led_pixels[1] = red;
    led_pixels[2] = blue;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_pixels, sizeof(led_pixels), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, WS2812_RESET_MS));
}

void led_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .duration0 = 3,
            .level0 = 1,
            .duration1 = 9,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9,
            .level0 = 1,
            .duration1 = 3,
            .level1 = 0,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    led_set(0);
}

void led_set(uint32_t on)
{
    led_is_on = on;
    led_refresh(on ? 16 : 0, 0, 0);
}

void led_toggle(void)
{
    led_set(!led_is_on);
}
