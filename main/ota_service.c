#include "ota_service.h"
#include "mqtt_service.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "myble.h"
#include "sdkconfig.h"
#include "wifi_provisioning.h"

#define OTA_NETWORK_TIMEOUT_MS 20000
#define OTA_SELF_TEST_DELAY_MS 10000
#define OTA_VALID_TIME_EPOCH 1704067200
#define OTA_MANIFEST_CAPACITY 2048U
#define OTA_URL_CAPACITY 384U
#define OTA_RELEASE_ID_CAPACITY 48U
#define OTA_HASH_SIZE 32U
#define OTA_HASH_BUFFER_SIZE 4096U

typedef struct {
    char release_id[OTA_RELEASE_ID_CAPACITY];
    char url[OTA_URL_CAPACITY];
    uint8_t model_id;
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint32_t size;
    uint8_t sha256[OTA_HASH_SIZE];
} ota_manifest_t;

static const char *TAG = "OTA";
static portMUX_TYPE g_ota_lock = portMUX_INITIALIZER_UNLOCKED;
static bool g_ota_active;
static tmy05_ota_request_t g_ota_request;

static int compare_version(uint8_t left_major,
                           uint8_t left_minor,
                           uint8_t left_patch,
                           uint8_t right_major,
                           uint8_t right_minor,
                           uint8_t right_patch)
{
    if (left_major != right_major) {
        return left_major > right_major ? 1 : -1;
    }
    if (left_minor != right_minor) {
        return left_minor > right_minor ? 1 : -1;
    }
    if (left_patch != right_patch) {
        return left_patch > right_patch ? 1 : -1;
    }
    return 0;
}

static void get_running_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    unsigned int parsed_major;
    unsigned int parsed_minor;
    unsigned int parsed_patch;
    const esp_app_desc_t *description = esp_app_get_description();
    if (sscanf(description->version, "%u.%u.%u",
               &parsed_major, &parsed_minor, &parsed_patch) == 3 &&
        parsed_major <= UINT8_MAX && parsed_minor <= UINT8_MAX &&
        parsed_patch <= UINT8_MAX) {
        *major = (uint8_t)parsed_major;
        *minor = (uint8_t)parsed_minor;
        *patch = (uint8_t)parsed_patch;
        return;
    }

    *major = CONFIG_TMY05_FW_VERSION_MAJOR;
    *minor = CONFIG_TMY05_FW_VERSION_MINOR;
    *patch = CONFIG_TMY05_FW_VERSION_PATCH;
}

static bool ensure_time_synchronized(void)
{
    time_t now;
    time(&now);
    if (now >= OTA_VALID_TIME_EPOCH) {
        return true;
    }

    ESP_LOGI(TAG, "Synchronizing time before HTTPS certificate validation");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        esp_netif_sntp_deinit();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP synchronization failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool parse_sha256(const char *text, uint8_t output[OTA_HASH_SIZE])
{
    if (text == NULL || strlen(text) != OTA_HASH_SIZE * 2U) {
        return false;
    }
    for (size_t i = 0; i < OTA_HASH_SIZE; ++i) {
        int high = hex_value(text[i * 2U]);
        int low = hex_value(text[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool parse_version(const char *text,
                          uint8_t *major,
                          uint8_t *minor,
                          uint8_t *patch)
{
    unsigned int parsed_major;
    unsigned int parsed_minor;
    unsigned int parsed_patch;
    char trailing;
    if (text == NULL ||
        sscanf(text, "%u.%u.%u%c", &parsed_major, &parsed_minor,
               &parsed_patch, &trailing) != 3 ||
        parsed_major > UINT8_MAX || parsed_minor > UINT8_MAX ||
        parsed_patch > UINT8_MAX) {
        return false;
    }
    *major = (uint8_t)parsed_major;
    *minor = (uint8_t)parsed_minor;
    *patch = (uint8_t)parsed_patch;
    return true;
}

static esp_err_t fetch_manifest_json(const char *url,
                                     char *buffer,
                                     size_t capacity,
                                     size_t *output_length)
{
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_NETWORK_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Manifest connection failed: %s", esp_err_to_name(result));
        esp_http_client_cleanup(client);
        return result;
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length >= (int64_t)capacity) {
        ESP_LOGE(TAG, "Manifest HTTP response invalid: status=%d length=%" PRId64,
                 status, content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0U;
    while (total < capacity - 1U) {
        int read = esp_http_client_read(client, buffer + total,
                                        capacity - 1U - total);
        if (read < 0) {
            result = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        total += (size_t)read;
    }
    if (result == ESP_OK && total == capacity - 1U) {
        char extra;
        if (esp_http_client_read(client, &extra, 1U) > 0) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || total == 0U) {
        return result == ESP_OK ? ESP_FAIL : result;
    }
    buffer[total] = '\0';
    *output_length = total;
    return ESP_OK;
}

static bool parse_manifest(const char *json,
                           size_t json_length,
                           ota_manifest_t *manifest)
{
    bool valid = false;
    cJSON *root = cJSON_ParseWithLength(json, json_length);
    if (root == NULL) {
        return false;
    }

    const cJSON *release_id = cJSON_GetObjectItemCaseSensitive(root, "releaseId");
    const cJSON *model_id = cJSON_GetObjectItemCaseSensitive(root, "modelId");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    const cJSON *sha256 = cJSON_GetObjectItemCaseSensitive(root, "sha256");

    if (!cJSON_IsString(release_id) || release_id->valuestring == NULL ||
        strlen(release_id->valuestring) >= sizeof(manifest->release_id) ||
        !cJSON_IsNumber(model_id) || model_id->valuedouble < 1.0 ||
        model_id->valuedouble > 255.0 ||
        (double)(uint8_t)model_id->valuedouble != model_id->valuedouble ||
        !cJSON_IsString(version) || version->valuestring == NULL ||
        !cJSON_IsString(url) || url->valuestring == NULL ||
        strncmp(url->valuestring, "https://", 8U) != 0 ||
        strlen(url->valuestring) >= sizeof(manifest->url) ||
        !cJSON_IsNumber(size) || size->valuedouble <= 0.0 ||
        size->valuedouble > (double)UINT32_MAX ||
        (double)(uint32_t)size->valuedouble != size->valuedouble ||
        !cJSON_IsString(sha256) || sha256->valuestring == NULL) {
        goto cleanup;
    }

    memset(manifest, 0, sizeof(*manifest));
    manifest->model_id = (uint8_t)model_id->valuedouble;
    manifest->size = (uint32_t)size->valuedouble;
    if (!parse_version(version->valuestring, &manifest->major,
                       &manifest->minor, &manifest->patch) ||
        !parse_sha256(sha256->valuestring, manifest->sha256)) {
        goto cleanup;
    }
    snprintf(manifest->release_id, sizeof(manifest->release_id), "%s",
             release_id->valuestring);
    snprintf(manifest->url, sizeof(manifest->url), "%s", url->valuestring);
    valid = true;

cleanup:
    cJSON_Delete(root);
    return valid;
}

static bool validate_manifest_target(const ota_manifest_t *manifest,
                                     const tmy05_ota_request_t *request,
                                     uint8_t current_model,
                                     uint8_t current_major,
                                     uint8_t current_minor,
                                     uint8_t current_patch)
{
    char expected_release_id[OTA_RELEASE_ID_CAPACITY];
    snprintf(expected_release_id, sizeof(expected_release_id), "%u-%u.%u.%u",
             manifest->model_id, manifest->major,
             manifest->minor, manifest->patch);
    if (strcmp(manifest->release_id, expected_release_id) != 0 ||
        manifest->model_id != current_model) {
        return false;
    }
    if (manifest->model_id != request->model_id ||
        manifest->major != request->major ||
        manifest->minor != request->minor ||
        manifest->patch != request->patch) {
        return false;
    }
    return compare_version(manifest->major, manifest->minor, manifest->patch,
                           current_major, current_minor, current_patch) > 0;
}

static bool calculate_partition_sha256(const esp_partition_t *partition,
                                       uint32_t image_size,
                                       uint8_t output[OTA_HASH_SIZE])
{
    uint8_t *buffer = malloc(OTA_HASH_BUFFER_SIZE);
    if (buffer == NULL) {
        return false;
    }

    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    int result = mbedtls_sha256_starts(&context, 0);
    uint32_t offset = 0U;
    while (result == 0 && offset < image_size) {
        size_t chunk = image_size - offset;
        if (chunk > OTA_HASH_BUFFER_SIZE) {
            chunk = OTA_HASH_BUFFER_SIZE;
        }
        esp_err_t read_result = esp_partition_read(partition, offset,
                                                   buffer, chunk);
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "Could not read OTA partition: %s",
                     esp_err_to_name(read_result));
            result = -1;
            break;
        }
        result = mbedtls_sha256_update(&context, buffer, chunk);
        offset += (uint32_t)chunk;
    }
    if (result == 0) {
        result = mbedtls_sha256_finish(&context, output);
    }
    mbedtls_sha256_free(&context);
    free(buffer);
    return result == 0;
}

static void finish_ota_task(void)
{
    portENTER_CRITICAL(&g_ota_lock);
    g_ota_active = false;
    portEXIT_CRITICAL(&g_ota_lock);
    vTaskDelete(NULL);
}

static void report_error_and_finish(tmy05_ota_error_t error)
{
    ble_ota_report_status(TMY05_OTA_STATUS_ERROR, error);
    finish_ota_task();
}

static void ota_update_task(void *argument)
{
    (void)argument;
    tmy05_ota_request_t request;
    portENTER_CRITICAL(&g_ota_lock);
    request = g_ota_request;
    portEXIT_CRITICAL(&g_ota_lock);

    ble_ota_report_status(TMY05_OTA_STATUS_PREPARING, TMY05_OTA_ERROR_NONE);

    if (!wifi_provisioning_wait_connected(
            pdMS_TO_TICKS(OTA_NETWORK_TIMEOUT_MS))) {
        ESP_LOGE(TAG, "OTA aborted: Wi-Fi is not connected");
        report_error_and_finish(TMY05_OTA_ERROR_NETWORK);
        return;
    }

    if (!ensure_time_synchronized()) {
        report_error_and_finish(TMY05_OTA_ERROR_TIMEOUT);
        return;
    }

    const ble_sim_profile_info_t *profile =
        ble_sim_profile_info(ble_sim_get_active_profile());
    uint8_t current_major;
    uint8_t current_minor;
    uint8_t current_patch;
    get_running_version(&current_major, &current_minor, &current_patch);

    char manifest_url[OTA_URL_CAPACITY];
    int url_length = snprintf(manifest_url, sizeof(manifest_url),
                              "%s/%u/%u.%u.%u.json",
                              CONFIG_TMY05_OTA_RELEASE_BASE_URL,
                              request.model_id, request.major,
                              request.minor, request.patch);
    if (url_length <= 0 || (size_t)url_length >= sizeof(manifest_url)) {
        ESP_LOGE(TAG, "Manifest URL is invalid or too long");
        report_error_and_finish(TMY05_OTA_ERROR_DOWNLOAD);
        return;
    }

    ESP_LOGI(TAG, "Fetching OTA manifest: %s", manifest_url);
    char manifest_json[OTA_MANIFEST_CAPACITY];
    size_t manifest_length = 0U;
    ota_manifest_t manifest;
    esp_err_t err = fetch_manifest_json(manifest_url, manifest_json,
                                        sizeof(manifest_json), &manifest_length);
    if (err != ESP_OK ||
        !parse_manifest(manifest_json, manifest_length, &manifest) ||
        !validate_manifest_target(&manifest, &request, profile->model_id,
                                  current_major, current_minor, current_patch)) {
        ESP_LOGE(TAG, "OTA manifest is unavailable, invalid, or mismatched");
        report_error_and_finish(err == ESP_OK ? TMY05_OTA_ERROR_VERSION
                                              : TMY05_OTA_ERROR_DOWNLOAD);
        return;
    }

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL || manifest.size > update_partition->size) {
        ESP_LOGE(TAG, "OTA image does not fit the update partition");
        report_error_and_finish(TMY05_OTA_ERROR_SPACE);
        return;
    }

    ESP_LOGI(TAG, "Release %s: %" PRIu32 " bytes -> %s at 0x%08" PRIx32,
             manifest.release_id, manifest.size, update_partition->label,
             update_partition->address);
    ble_ota_report_status(TMY05_OTA_STATUS_DOWNLOADING,
                          TMY05_OTA_ERROR_NONE);

    esp_http_client_config_t http_config = {
        .url = manifest.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_NETWORK_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS OTA begin failed: %s", esp_err_to_name(err));
        report_error_and_finish(TMY05_OTA_ERROR_DOWNLOAD);
        return;
    }

    do {
        err = esp_https_ota_perform(ota_handle);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    int image_length = esp_https_ota_get_image_len_read(ota_handle);
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(ota_handle) ||
        image_length < 0 || (uint32_t)image_length != manifest.size) {
        ESP_LOGE(TAG, "OTA download incomplete: err=%s actual=%d expected=%" PRIu32,
                 esp_err_to_name(err), image_length, manifest.size);
        esp_https_ota_abort(ota_handle);
        report_error_and_finish(TMY05_OTA_ERROR_DOWNLOAD);
        return;
    }

    ble_ota_report_status(TMY05_OTA_STATUS_VERIFYING,
                          TMY05_OTA_ERROR_NONE);
    uint8_t actual_sha256[OTA_HASH_SIZE];
    if (!calculate_partition_sha256(update_partition, manifest.size,
                                    actual_sha256) ||
        memcmp(actual_sha256, manifest.sha256, OTA_HASH_SIZE) != 0) {
        ESP_LOGE(TAG, "Downloaded firmware SHA-256 does not match manifest");
        esp_https_ota_abort(ota_handle);
        report_error_and_finish(TMY05_OTA_ERROR_VERIFY);
        return;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image validation/activation failed: %s",
                 esp_err_to_name(err));
        report_error_and_finish(err == ESP_ERR_OTA_VALIDATE_FAILED
                                    ? TMY05_OTA_ERROR_VERIFY
                                    : TMY05_OTA_ERROR_FLASH);
        return;
    }

    ESP_LOGI(TAG, "OTA image verified; restarting into the new partition");
    ble_ota_report_status(TMY05_OTA_STATUS_RESTARTING,
                          TMY05_OTA_ERROR_NONE);
    vTaskDelay(pdMS_TO_TICKS(750));
    mqtt_service_prepare_restart();
    esp_restart();
}

static bool start_ota_from_ble(const tmy05_ota_request_t *request)
{
    if (request == NULL || request->command != TMY05_OTA_COMMAND_START) {
        return false;
    }

    bool can_start;
    portENTER_CRITICAL(&g_ota_lock);
    can_start = !g_ota_active;
    if (can_start) {
        g_ota_active = true;
        g_ota_request = *request;
    }
    portEXIT_CRITICAL(&g_ota_lock);

    if (!can_start) {
        ESP_LOGW(TAG, "Ignoring duplicate OTA request");
        return false;
    }

    if (xTaskCreate(ota_update_task, "https_ota", 12288, NULL, 5, NULL) !=
        pdPASS) {
        portENTER_CRITICAL(&g_ota_lock);
        g_ota_active = false;
        portEXIT_CRITICAL(&g_ota_lock);
        return false;
    }
    return true;
}

static bool basic_self_test(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint32_t flash_size = 0;
    if (running == NULL || running->type != ESP_PARTITION_TYPE_APP ||
        esp_flash_get_size(NULL, &flash_size) != ESP_OK ||
        running->address + running->size > flash_size) {
        ESP_LOGE(TAG, "Self-test failed: invalid running partition");
        return false;
    }

    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "Self-test failed: heap corruption detected");
        return false;
    }
    return true;
}

static void ota_self_test_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(OTA_SELF_TEST_DELAY_MS));

    if (!basic_self_test()) {
        ESP_LOGE(TAG, "New image self-test failed; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New image self-test passed and image was confirmed");
    } else {
        ESP_LOGE(TAG, "Could not confirm new image: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t ota_service_init(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware version: %s", app_desc->version);
    ESP_LOGI(TAG, "OTA release manifest base: %s",
             CONFIG_TMY05_OTA_RELEASE_BASE_URL);

    ble_ota_set_start_handler(start_ota_from_ble);

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "New OTA image is pending verification; starting self-test");
        if (xTaskCreate(ota_self_test_task, "ota_self_test", 4096,
                        NULL, 4, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}
