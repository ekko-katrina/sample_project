#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义, 例如 pdMS_TO_TICKS
#include "freertos/task.h"     // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "led.h"               // 引入板载 RGB LED 驱动接口
#include "key.h"               // 引入 BOOT 按键驱动接口

void app_main(void) // ESP-IDF 应用入口函数
{
    led_init(); // 初始化板载 RGB LED
    // LED_TOGGLE(); // 测试用: 初始化后立即翻转一次 LED 状态
    key_init(); // 初始化 BOOT 按键 GPIO

    uint8_t key; // 保存本次按键扫描结果

    while (1) // 主循环, 程序会一直在这里运行
    {

        // LED_TOGGLE(); // 测试用: 直接翻转 LED
        // vTaskDelay(pdMS_TO_TICKS(500)); // 测试用: 每 500ms 闪烁一次
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
}
