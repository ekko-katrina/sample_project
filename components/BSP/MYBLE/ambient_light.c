#include "ambient_light.h"
#include "sdkconfig.h"
#include "nvs.h"
#if CONFIG_TMY06_AMBIENT_BOARD_PREVIEW
#include "myble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "esp_log.h"

// Apply brightness only to the output; saved RGB values keep their full range.
#define AMBIENT_MAX_SCALE 153U // 60% of 255
#define AMBIENT_MIN_SCALE 51U  // 20% of 255
#endif

esp_err_t ambient_light_load(tmy06_ambient_settings_t *settings)
{
    if (settings == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("y06_light", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    tmy06_ambient_settings_t saved = {0};
    size_t size = sizeof(saved);
    err = nvs_get_blob(handle, "settings_v1", &saved, &size);
    nvs_close(handle);
    if (err != ESP_OK) return err;
    // Legacy settings contain only effect + six RGB bytes; default to enabled.
    if ((size != sizeof(saved) && size != offsetof(tmy06_ambient_settings_t, disabled)) ||
        saved.effect > 1U || saved.disabled > 1U) return ESP_ERR_INVALID_STATE;
    *settings = saved;
    return ESP_OK;
}

esp_err_t ambient_light_save(const tmy06_ambient_settings_t *settings)
{
    if (settings == NULL || settings->effect > 1U || settings->disabled > 1U) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("y06_light", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "settings_v1", settings, sizeof(*settings));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

#if CONFIG_TMY06_AMBIENT_BOARD_PREVIEW
static void ambient_light_task(void *arg)
{
    (void)arg;
    uint8_t previous[3] = {0, 0, 0};
    unsigned int phase = 0;
    for (;;) {
        tmy06_state_t state;
        if (ble_tmy06_get_state(&state)) {
            const uint8_t *rgb = tmy06_protocol_active_ambient_color(&state);
            // One breathing cycle is 3 seconds; steady output ignores the phase.
            unsigned int ramp = phase < 50U ? phase : 100U - phase;
            unsigned int scale = state.ambient.effect == 0U ? AMBIENT_MAX_SCALE
                : AMBIENT_MIN_SCALE + ramp * (AMBIENT_MAX_SCALE - AMBIENT_MIN_SCALE) / 50U;
            if (state.power == TMY06_POWER_OFF || state.ambient.disabled) scale = 0;
            uint8_t output[3];
            bool changed = false;
            for (size_t i = 0; i < 3U; ++i) {
                output[i] = (uint8_t)(rgb[i] * scale / 255U);
                changed |= output[i] != previous[i];
                previous[i] = output[i];
            }
            if (changed) {
                esp_err_t err = led_set_rgb(output[0], output[1], output[2]);
                if (err != ESP_OK) {
                    ESP_LOGE("AMBIENT-LIGHT", "Board preview stopped: %s; BLE settings remain available", esp_err_to_name(err));
                    vTaskDelete(NULL);
                    return;
                }
            }
        }
        phase = (phase + 1U) % 100U;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
#endif

esp_err_t ambient_light_start(void)
{
#if CONFIG_TMY06_AMBIENT_BOARD_PREVIEW
    if (ble_sim_profile_info(ble_sim_get_active_profile())->has_ambient_light) {
        esp_err_t err = led_init_checked();
        if (err != ESP_OK) return err;
        if (xTaskCreate(ambient_light_task, "y06_light", 3072, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}
