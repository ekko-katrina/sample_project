#include "key.h" // 引入按键驱动头文件

void key_init(void) // 初始化 BOOT 按键
{
    gpio_config_t gpio_init_struct = {0}; // 创建 GPIO 配置结构体并清零
    gpio_init_struct.intr_type = GPIO_INTR_DISABLE; // 禁用 GPIO 中断
    gpio_init_struct.mode = GPIO_MODE_INPUT; // 设置 GPIO 为输入模式
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE; // 使能内部上拉, 松开按键时为高电平
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE; // 禁用内部下拉
    gpio_init_struct.pin_bit_mask = 1ull << BOOT_GPIO_PIN; // 选择 BOOT 按键所在的 GPIO0
    gpio_config(&gpio_init_struct); // 应用 GPIO 配置
}

uint8_t key_scan(uint8_t mode) // 扫描按键, mode 非 0 时允许连续触发
{
    uint8_t keyval = 0; // 默认没有按键按下
    static uint8_t key_boot = 1; // 按键释放标志, 用来防止长按反复触发

    if (mode) // 如果允许连续触发
    {
        key_boot = 1; // 每次扫描前都重新允许触发
    }

    if (key_boot && (BOOT == 0)) // 如果允许触发且检测到 BOOT 被按下
    {
        vTaskDelay(10); // 延时 10 个 tick 做消抖
        key_boot = 0; // 标记本次按下已经处理, 防止长按重复触发

        if (BOOT == 0) // 消抖后仍然是低电平, 确认按键真的按下
        {
            keyval = BOOT_PRES; // 返回 BOOT 按键按下事件
        }
    }
    else if (BOOT == 1) // 如果 BOOT 已经松开
    {
        key_boot = 1; // 重新允许下一次按下触发
    }


    return keyval; // 返回本次扫描到的按键值
}
