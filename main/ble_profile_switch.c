#include "ble_profile_switch.h"
#include "mqtt_service.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#define TAG "PROFILE-SWITCH"
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define PROFILE_NVS_NAMESPACE "ble_sim"
#define PROFILE_NVS_KEY "profile"
#define BUTTON_POLL_MS 20U
#define LONG_PRESS_MS 2000U

static ble_sim_profile_t g_button_profile;

ble_sim_profile_t ble_profile_load(void)
{
    nvs_handle_t handle;
    uint8_t stored_profile = BLE_SIM_PROFILE_LEGACY_Y05;
    esp_err_t err = nvs_open(PROFILE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "尚未保存档位，默认使用旧版 Y05");
        return BLE_SIM_PROFILE_LEGACY_Y05;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取档位失败：%s，默认使用旧版 Y05", esp_err_to_name(err));
        return BLE_SIM_PROFILE_LEGACY_Y05;
    }

    err = nvs_get_u8(handle, PROFILE_NVS_KEY, &stored_profile);
    nvs_close(handle);
    if (err != ESP_OK || stored_profile >= BLE_SIM_PROFILE_COUNT) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "保存的档位无效，默认使用旧版 Y05");
        }
        return BLE_SIM_PROFILE_LEGACY_Y05;
    }
    return (ble_sim_profile_t)stored_profile;
}

static bool store_profile(ble_sim_profile_t profile)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PROFILE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法打开档位存储：%s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_u8(handle, PROFILE_NVS_KEY, (uint8_t)profile);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存档位失败：%s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    bool switch_pending = false;
    bool long_press_handled = false;
    TickType_t pressed_at = 0;

    while (true) {
        bool pressed = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
        if (pressed && !was_pressed) {
            pressed_at = xTaskGetTickCount();
            was_pressed = true;
        } else if (pressed && !long_press_handled &&
                   xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
            ble_sim_profile_t next = ble_sim_profile_next(g_button_profile);
            long_press_handled = true;
            if (store_profile(next)) {
                ESP_LOGW(TAG,
                         "BOOT 长按：准备切换 %s -> %s，请松开按键",
                         ble_sim_profile_info(g_button_profile)->display_name,
                         ble_sim_profile_info(next)->display_name);
                g_button_profile = next;
                switch_pending = true;
            }
        } else if (!pressed && was_pressed) {
            if (switch_pending) {
                /* GPIO0 is a strapping pin. Require a stable release before reset. */
                vTaskDelay(pdMS_TO_TICKS(100));
                if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                    continue;
                }
                ESP_LOGW(TAG, "BOOT 已松开，正在安全重启并应用 %s",
                          ble_sim_profile_info(g_button_profile)->display_name);
                mqtt_service_prepare_restart();
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_restart();
            }
            was_pressed = false;
            switch_pending = false;
            long_press_handled = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void ble_profile_switch_button_start(ble_sim_profile_t active_profile)
{
    g_button_profile = active_profile;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    BaseType_t created = xTaskCreate(button_task,
                                    "profile_button",
                                    3072,
                                    NULL,
                                    5,
                                    NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "长按 BOOT 2 秒可切换设备档位");
}
