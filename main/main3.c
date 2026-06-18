#include <stdio.h>
#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义, 例如 pdMS_TO_TICKS
#include "freertos/task.h"     // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "driver/gpio.h"       // 引入 FreeRTOS 任务延时函数 vTaskDelay
#include "ffs.h"

const char *test_string = "spiffs test";
char read_buf[64];

void app_main(void)
{

    spiffs_init();

    FILE *f = fopen("/storage/test.txt", "w");
    if (f != NULL)
    {
        fprintf(f, test_string);
        fclose(f);
    }

    vTaskDelay(10);

    f = fopen("/storage/test.txt", "r");
    if (f != NULL)
    {
        if (fgets(read_buf, sizeof(read_buf), f) != NULL)
        {
            printf("read data: %s\n", read_buf);
        }
        else
        {
            printf("failed to read data from /storage/test.txt\n");
        }
        fclose(f);
    }
    else
    {
        printf("failed to open /storage/test.txt\n");
    }

    while (1)
    {

        vTaskDelay(500);
    }
}
