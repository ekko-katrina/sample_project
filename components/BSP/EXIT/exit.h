#ifndef __EXIT_H_ // 防止头文件被重复包含
#define __EXIT_H_ // 定义 KEY 头文件保护宏

#include "driver/gpio.h"
#include "esp_system.h"
#include "led.h"

#define BOOT_INT_GPIO_PIN GPIO_NUM_0

#define BOOT gpio_get_level(BOOT_INT_GPIO_PIN) // 读取 BOOT 按键电平

void exit_init(void);

#endif