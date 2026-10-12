// Compile with ambient_stubs first in the include path and ambient_light.c.
#include "ambient_light.h"
#include "nvs.h"

static tmy06_ambient_settings_t stored, pending;
static bool present, fail_commit;
static size_t stored_size = sizeof(stored);
void *memset(void *destination, int value, size_t size)
{
    for (size_t i = 0; i < size; i++) ((uint8_t *)destination)[i] = (uint8_t)value;
    return destination;
}
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static void copy_bytes(void *destination, const void *source, size_t size)
{
    for (size_t i = 0; i < size; i++) ((uint8_t *)destination)[i] = ((const uint8_t *)source)[i];
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    (void)name; *handle = 1;
    return mode == NVS_READONLY && !present ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *size)
{
    (void)handle; (void)key;
    copy_bytes(data, &stored, stored_size); *size = stored_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    (void)handle; (void)key; copy_bytes(&pending, data, size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (fail_commit) return ESP_ERR_NO_MEM;
    copy_bytes(&stored, &pending, sizeof(stored)); present = true; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }

int mainCRTStartup(void)
{
    tmy06_ambient_settings_t config = {.effect = 0, .colors = {{0, 0, 255}, {255, 0, 0}}};
    CHECK(ambient_light_load(&config) == ESP_ERR_NVS_NOT_FOUND);
    CHECK(config.colors[0][2] == 255);
    config.effect = 1; config.colors[0][0] = 42;
    CHECK(ambient_light_save(&config) == ESP_OK);
    // Simulate a fresh boot, then restore both colors and the shared effect.
    tmy06_ambient_settings_t rebooted = {0};
    CHECK(ambient_light_load(&rebooted) == ESP_OK);
    CHECK(rebooted.effect == 1 && rebooted.colors[0][0] == 42 && rebooted.colors[0][2] == 255);
    CHECK(rebooted.colors[1][0] == 255);
    fail_commit = true; config.effect = 0;
    CHECK(ambient_light_save(&config) != ESP_OK);
    CHECK(ambient_light_load(&rebooted) == ESP_OK && rebooted.effect == 1);
    stored.effect = 2;
    CHECK(ambient_light_load(&rebooted) == ESP_ERR_INVALID_STATE && rebooted.effect == 1);
    stored.effect = 0; stored_size = sizeof(stored) - 1;
    CHECK(ambient_light_load(&rebooted) == ESP_ERR_INVALID_STATE && rebooted.effect == 1);
    return 0;
}
