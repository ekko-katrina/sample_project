#include "led.h" // 引入 LED 驱动头文件

#include "driver/rmt_common.h"  // 引入 RMT 通用接口定义
#include "driver/rmt_encoder.h" // 引入 RMT 编码器接口
#include "driver/rmt_tx.h"      // 引入 RMT 发送接口
#include "esp_err.h"            // 引入 ESP_ERROR_CHECK 错误检查宏

#define WS2812_RESOLUTION_HZ 10000000 // RMT 分辨率 10MHz, 每个计数单位为 0.1us
#define WS2812_RESET_MS 1             // 等待 WS2812 发送结束的超时时间

static rmt_channel_handle_t led_chan;     // RMT 发送通道句柄
static rmt_encoder_handle_t led_encoder;  // RMT 字节编码器句柄
static uint8_t led_pixels[3];             // 单颗 WS2812 的颜色数据, 顺序为 GRB
static uint32_t led_is_on;                // 记录当前 LED 是否处于点亮状态

static void led_refresh(uint8_t red, uint8_t green, uint8_t blue) // 向 RGB LED 写入一次颜色
{
    led_pixels[0] = green; // WS2812 数据顺序第 1 字节是绿色
    led_pixels[1] = red;   // WS2812 数据顺序第 2 字节是红色
    led_pixels[2] = blue;  // WS2812 数据顺序第 3 字节是蓝色

    rmt_transmit_config_t tx_config = { // 配置本次 RMT 发送参数
        .loop_count = 0,                // 只发送一次, 不循环发送
        .flags.eot_level = 0,           // 发送结束后保持低电平
    };

    ESP_ERROR_CHECK(rmt_transmit(led_chan, led_encoder, led_pixels, sizeof(led_pixels), &tx_config)); // 启动 RMT 发送颜色数据
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, WS2812_RESET_MS)); // 等待发送完成
}

void led_init(void) // 初始化 RGB LED 使用的 RMT 外设
{
    rmt_tx_channel_config_t tx_chan_config = { // 配置 RMT 发送通道
        .clk_src = RMT_CLK_SRC_DEFAULT,        // 使用默认 RMT 时钟源
        .gpio_num = LED_GPIO_PIN,              // RMT 输出到板载 RGB LED 数据引脚
        .mem_block_symbols = 64,               // RMT 内部符号缓存大小
        .resolution_hz = WS2812_RESOLUTION_HZ, // 设置 RMT 计时分辨率
        .trans_queue_depth = 4,                // 最多缓存 4 次发送请求
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan)); // 创建 RMT 发送通道

    rmt_bytes_encoder_config_t encoder_config = { // 配置 WS2812 的 0/1 位时序编码
        .bit0 = {                                // WS2812 发送 bit0 的波形
            .duration0 = 3,                      // bit0 高电平持续 0.3us
            .level0 = 1,                         // bit0 第一段为高电平
            .duration1 = 9,                      // bit0 低电平持续 0.9us
            .level1 = 0,                         // bit0 第二段为低电平
        },
        .bit1 = {           // WS2812 发送 bit1 的波形
            .duration0 = 9, // bit1 高电平持续 0.9us
            .level0 = 1,    // bit1 第一段为高电平
            .duration1 = 3, // bit1 低电平持续 0.3us
            .level1 = 0,    // bit1 第二段为低电平
        },
        .flags.msb_first = 1, // 每个字节从最高位开始发送
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &led_encoder)); // 创建 RMT 字节编码器
    ESP_ERROR_CHECK(rmt_enable(led_chan)); // 使能 RMT 发送通道

    led_set(0); // 初始化完成后先关闭 LED
}

void led_set(uint32_t on) // 设置 LED 亮灭
{
    led_is_on = on; // 保存当前 LED 状态
    led_refresh(on ? 16 : 0, 0, 0); // 点亮时显示低亮度红色, 关闭时三色都为 0
}

void led_toggle(void) // 翻转 LED 亮灭状态
{
    led_set(!led_is_on); // 当前亮则关, 当前灭则亮
}
