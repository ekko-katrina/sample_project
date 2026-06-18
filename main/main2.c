#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义, 例如 pdMS_TO_TICKS
#include "freertos/task.h"     // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "led.h"               // 引入板载 RGB LED 驱动接口
#include "key.h"               // 引入 BOOT 按键驱动接口
#include "exit.h"
#include "driver/uart.h"
#include "uart.h"
#include "myuart.h"

void app_main(void)
{
    int length = 0;
    uint8_t receive_data[1024] = {0};
    myuart_init();
    while (1)
    {
        length = uart_read_bytes(UART_NUM_0, receive_data, 1024, 100);
        if (length > 0)
        {
            uart_write_bytes(UART_NUM_0, receive_data, length);
            uart_flush(UART_NUM_0);
        }
        vTaskDelay(5);
    }
}

/* void app_main(void)
{


    size_t len = 0;
    uint16_t times = 0;
    unsigned char data[RX_BUF_SIZE] = {0};

    led_init();
    usart_init(115200);

    while (1)
    {
        uart_get_buffered_data_len(USART_UX, (size_t *)&len);

        if (len > 0)
        {
            int rx_len = 0;

            memset(data, 0, RX_BUF_SIZE);
            printf("\n发送的消息为：\n");
            rx_len = uart_read_bytes(USART_UX, data, len, pdMS_TO_TICKS(100));
            if (rx_len > 0)
            {
                uart_write_bytes(USART_UX, (const char *)data, rx_len);
            }
        }
        else
        {
            times++;

            if (times % 5000 == 0)
            {
                printf("\nESP32-S3串口实验\n");
            }
            if (times % 200 == 0)
            {
                printf("\n请输入数据，以回车键结束\n");
            }
            if (times % 20 == 0)
            {
                LED_TOGGLE();
            }
            vTaskDelay(10);
        }
    }
} */

/* void app_main(void)
{
    led_init();
    exit_init();
    while (1)
    {
        vTaskDelay(10);
    }
} */

/* void app_main(void) // ESP-IDF 应用入口函数
{
    led_init(); // 初始化板载 RGB LED


    while (1) // 主循环, 程序会一直在这里运行
    {

        LED_TOGGLE(); // 测试用: 直接翻转 LED
        vTaskDelay(pdMS_TO_TICKS(500)); // 测试用: 每 500ms 闪烁一次
    }
} */

/* void app_main(void) // ESP-IDF 应用入口函数
{
    led_init(); // 初始化板载 RGB LED
    key_init(); // 初始化 BOOT 按键 GPIO

    uint8_t key; // 保存本次按键扫描结果

    while (1) // 主循环, 程序会一直在这里运行
    {

        key = key_scan(0); // 扫描 BOOT 按键, 参数 0 表示不支持连续触发

        switch (key) // 根据按键扫描结果执行对应动作
        {
        case BOOT_PRES: // 检测到 BOOT 按键被按下
            LED_TOGGLE(); // 翻转 LED 亮灭状态
            break; // 处理完当前按键后跳出 switch

        default: // 没有检测到有效按键
            break; // 不执行任何动作
        }
        vTaskDelay(10); // 延时 10 个 tick, 避免循环过快占满 CPU
    }
} */
