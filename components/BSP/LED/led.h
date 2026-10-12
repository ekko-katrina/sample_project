#ifndef __LED_H_ // 防止头文件被重复包含
#define __LED_H_ // 定义 LED 头文件保护宏

#include <stdint.h> // 引入 uint32_t 等标准整数类型
#include "esp_err.h"

#include "hal/gpio_types.h" // 引入 gpio_num_t 和 GPIO_NUM_x 定义

#define LED_GPIO_PIN GPIO_NUM_48 // 板载 RGB LED 的数据引脚

void led_init(void);        // 初始化 RGB LED
esp_err_t led_init_checked(void);
void led_set(uint32_t on);  // 设置 LED 开关状态, 非 0 表示亮
void led_toggle(void);      // 翻转 LED 当前亮灭状态
esp_err_t led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);

#define LED(X) led_set(X)          // 兼容宏: 设置 LED 状态
#define LED_TOGGLE() led_toggle()  // 兼容宏: 翻转 LED 状态

#endif // __LED_H_
