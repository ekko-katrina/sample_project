#include <stdio.h>
#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义, 例如 pdMS_TO_TICKS
#include "freertos/task.h"     // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "driver/gpio.h"       // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "wifiap.h"
#include "nvs_flash.h"

void app_main(void)
{
    nvs_flash_init();
    wifiap_init();

    while (1)
    {
        vTaskDelay(500);
    }
}
