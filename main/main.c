#include "esp_err.h"
#include "nvs_flash.h"
#include "myble.h"
#include "tmy05_console.h"
#include "ble_profile_switch.h"
#include "ota_service.h"
#include "wifi_provisioning.h"
#include "tesla_link.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ble_sim_profile_t profile = ble_profile_load();
    ble_init(profile);
    ESP_ERROR_CHECK(tesla_link_start());
    ESP_ERROR_CHECK(wifi_provisioning_start());
    tmy05_console_init();
    ble_profile_switch_button_start(profile);
    ESP_ERROR_CHECK(ota_service_init());
}
