#include <stdio.h>
#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义, 例如 pdMS_TO_TICKS
#include "freertos/task.h"     // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "driver/gpio.h"       // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "wifista.h"
#include "nvs_flash.h"
#include "myuart.h"
#include "myntc.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "main";

void app_main(void)
{
    time_t timer;
    struct tm local_time;
    char time_str[20];

    nvs_flash_init();
    wifista_init();
    myntc_init();

    while (1)
    {
        timer = time(NULL);
        if (localtime_r(&timer, &local_time) != NULL)
        {
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &local_time);
            ESP_LOGI(TAG, "current time: %s", time_str);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
