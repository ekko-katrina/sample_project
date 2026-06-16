#ifndef __KEY_H_ // 防止头文件被重复包含
#define __KEY_H_ // 定义 KEY 头文件保护宏

#include "freertos/FreeRTOS.h" // 引入 FreeRTOS 基础定义
#include "freertos/task.h"     // 引入 vTaskDelay 延时函数
#include "driver/gpio.h"       // 引入 GPIO 驱动接口

#define BOOT_GPIO_PIN GPIO_NUM_0 // BOOT 按键连接到 GPIO0

#define BOOT gpio_get_level(BOOT_GPIO_PIN) // 读取 BOOT 按键电平

#define BOOT_PRES 1 // BOOT 按键按下时返回的键值

void key_init(void);             // 初始化按键 GPIO
uint8_t key_scan(uint8_t mode);  // 扫描按键, 返回按键状态

#endif // __KEY_H_
