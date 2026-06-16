// #include "key.h"

// void key_init(void)
// {
//     gpio_config_t gpio_init_struct = {0};
//     gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
//     gpio_init_struct.mode = GPIO_MODE_INPUT;
//     gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
//     gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
//     gpio_init_struct.pin_bit_mask = 1ull << BOOT_GPIO_PIN;
//     gpio_config(&gpio_init_struct);
// }

// uint8_t key_scan(uint8_t mode)
// {
//     uint8_t keyval = 0;
//     static uint8_t key_boot = 1;

//     if (mode)
//     {
//         key_boot = 1;
//     }

//     if (key_boot && (BOOT == 0))
//     {
//         vTaskDelay(10);
//         key_boot = 0;

//         if (BOOT == 0)
//         {
//             keyval = BOOT_PRES;
//         }
//     }
//     else if (BOOT == 1)
//     {
//         key_boot = 1;
//     }
//     return keyval;
// }