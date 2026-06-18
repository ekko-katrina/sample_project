#include "exit.h"

void IRAM_ATTR exit_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t) arg;

    if (gpio_num == BOOT_INT_GPIO_PIN)
    {
        LED_TOGGLE();
    }
}

void exit_init(void)
{
    gpio_config_t gpio_init_struct = {0};                      // 创建 GPIO 配置结构体并清零
    gpio_init_struct.intr_type = GPIO_INTR_NEGEDGE;            // 下降沿触发中断，通常对应按键按下
    gpio_init_struct.mode = GPIO_MODE_INPUT;                   // 设置 GPIO 为输入模式
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;          // 使能内部上拉, 松开按键时为高电平
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;     // 禁用内部下拉
    gpio_init_struct.pin_bit_mask = 1ull << BOOT_INT_GPIO_PIN; // 选择 BOOT 按键所在的 GPIO0

    gpio_config(&gpio_init_struct); // 应用 GPIO 配置

    gpio_install_isr_service(0);

    gpio_isr_handler_add(BOOT_INT_GPIO_PIN, exit_gpio_isr_handler, BOOT_INT_GPIO_PIN);
}