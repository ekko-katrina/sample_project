#include "esp_err.h"
#include "nvs_flash.h"
#include "myble.h"
#include "tmy05_console.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ble_init();
    tmy05_console_init();
}
