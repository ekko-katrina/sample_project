#include "tesla_link.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "myble.h"
#include "nvs.h"
#include "tesla_ble.h"

#define TAG "TESLA-LINK"

#define TESLA_COMMAND_QUERY_STATUS 0xC0U
#define TESLA_COMMAND_STATUS 0xC1U
#define TESLA_COMMAND_CONFIGURE 0xC2U
#define TESLA_COMMAND_EVENT 0xC3U

#define TESLA_STAGE_IDLE 0x00U
#define TESLA_STAGE_SAVED 0x01U
#define TESLA_STAGE_SCANNING 0x02U
#define TESLA_STAGE_CONNECTING 0x03U
#define TESLA_STAGE_WAITING_APPROVAL 0x04U
#define TESLA_STAGE_PAIRED 0x05U
#define TESLA_STAGE_AUTHENTICATED 0x06U
#define TESLA_STAGE_FAILED 0x07U

#define TESLA_ERROR_NONE 0x00U
#define TESLA_ERROR_INVALID_VIN 0x02U
#define TESLA_ERROR_SAVE_FAILED 0x03U
#define TESLA_ERROR_SCAN_TIMEOUT 0x04U
#define TESLA_ERROR_PAIRING_FAILED 0x06U
#define TESLA_ERROR_AUTH_FAILED 0x07U
#define TESLA_LINK_ERR_SAVE_FAILED ((esp_err_t)0x7502)

#define EVT_CONNECTED BIT0
#define EVT_SERVICE_DISCOVERED BIT1
#define EVT_AUTHENTICATED BIT2
#define EVT_DISCONNECTED BIT3

#define CONNECT_TIMEOUT_MS 30000U
#define PAIRING_APPROVAL_MS 60000U
#define RECONNECT_DELAY_MS 15000U

static const char *NVS_NAMESPACE = "tesla_link";
static const char *NVS_KEY_VIN = "vin";
static const char *NVS_KEY_PRIVATE = "priv_key";
static const char *NVS_KEY_PAIRED = "paired";

static SemaphoreHandle_t g_lock;
static EventGroupHandle_t g_events;
static TaskHandle_t g_task;
static TaskHandle_t g_drive_log_task;
static tesla_drive_state_t g_drive_log_state;
static TickType_t g_drive_log_updated;
static bool g_drive_log_valid;
static tesla_link_ble_notify_t g_notify_handler;
static char g_vin[18];
static bool g_configured;
static bool g_paired;
static uint8_t g_connection_state;
static uint8_t g_stage;
static uint8_t g_error;
static uint16_t g_request_connection = UINT16_MAX;
static uint32_t g_revision;

static uint8_t frame_checksum(const uint8_t *data, size_t length)
{
    uint8_t checksum = 0U;
    for (size_t i = 0; i < length; ++i) checksum ^= data[i];
    return checksum;
}

static bool validate_frame(const uint8_t *frame, size_t length)
{
    return frame != NULL && length >= 6U && frame[0] == 0xA5U &&
           frame[1] == 0x5AU && length == (size_t)frame[2] + 3U &&
           frame[length - 1U] == 0xAAU &&
           frame[length - 2U] == frame_checksum(frame, length - 2U);
}

static size_t build_frame(uint8_t command, const uint8_t *payload,
                          size_t payload_length, uint8_t *frame, size_t capacity)
{
    size_t length = payload_length + 6U;
    if (frame == NULL || length > capacity || payload_length + 3U > UINT8_MAX) return 0U;
    frame[0] = 0xA5U; frame[1] = 0x5AU; frame[2] = (uint8_t)(payload_length + 3U); frame[3] = command;
    if (payload_length > 0U) memcpy(frame + 4U, payload, payload_length);
    frame[length - 2U] = frame_checksum(frame, length - 2U);
    frame[length - 1U] = 0xAAU;
    return length;
}

static bool is_valid_vin(const char *vin)
{
    if (vin == NULL || strlen(vin) != 17U) return false;
    for (size_t i = 0; i < 17U; ++i) {
        unsigned char value = (unsigned char)vin[i];
        if (!isalnum(value) || value == 'I' || value == 'O' || value == 'Q') return false;
    }
    return true;
}

static esp_err_t save_configuration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, NVS_KEY_VIN, g_vin);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_KEY_PAIRED, g_paired ? 1U : 0U);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t save_paired(bool paired, uint32_t revision)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (revision != g_revision) {
        xSemaphoreGive(g_lock);
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_PAIRED, paired ? 1U : 0U);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) g_paired = paired;
    xSemaphoreGive(g_lock);
    return err;
}

static esp_err_t load_configuration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(g_vin);
    err = nvs_get_str(handle, NVS_KEY_VIN, g_vin, &length);
    uint8_t paired = 0U;
    if (err == ESP_OK) nvs_get_u8(handle, NVS_KEY_PAIRED, &paired);
    nvs_close(handle);
    if (err != ESP_OK || !is_valid_vin(g_vin)) {
        memset(g_vin, 0, sizeof(g_vin));
        return ESP_ERR_NOT_FOUND;
    }
    g_configured = true;
    g_paired = paired != 0U;
    return ESP_OK;
}

static esp_err_t load_or_create_key(void)
{
    uint8_t private_key[32];
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(private_key);
    err = nvs_get_blob(handle, NVS_KEY_PRIVATE, private_key, &length);
    if (err == ESP_OK && length == sizeof(private_key)) {
        nvs_close(handle);
        return tesla_ble_load_key(private_key);
    }
    err = tesla_ble_load_key(NULL);
    if (err == ESP_OK) err = tesla_ble_get_private_key(private_key);
    if (err == ESP_OK) err = nvs_set_blob(handle, NVS_KEY_PRIVATE, private_key, sizeof(private_key));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void notify_status(uint8_t command, uint16_t connection_handle)
{
    if (g_notify_handler == NULL) return;
    uint8_t payload[23], frame[29];
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t vin_length = g_configured ? strlen(g_vin) : 0U;
    payload[0] = g_configured ? 1U : 0U; payload[1] = g_paired ? 1U : 0U;
    payload[2] = g_connection_state; payload[3] = g_stage; payload[4] = g_error;
    payload[5] = (uint8_t)vin_length;
    if (vin_length > 0U) memcpy(payload + 6U, g_vin, vin_length);
    xSemaphoreGive(g_lock);
    size_t length = build_frame(command, payload, 6U + vin_length, frame, sizeof(frame));
    if (length > 0U) g_notify_handler(connection_handle, frame, length);
}

static void set_stage(uint8_t stage, uint8_t error)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_stage = stage; g_error = error;
    uint16_t connection_handle = g_request_connection;
    xSemaphoreGive(g_lock);
    if (connection_handle != UINT16_MAX) notify_status(TESLA_COMMAND_EVENT, connection_handle);
}

static void state_changed(tesla_conn_state_t state, void *argument)
{
    (void)argument;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_connection_state = (uint8_t)state;
    if (state == TESLA_STATE_DISCONNECTED || state == TESLA_STATE_CONNECTED) g_drive_log_valid = false;
    xSemaphoreGive(g_lock);
    if (state == TESLA_STATE_CONNECTED) xEventGroupSetBits(g_events, EVT_CONNECTED);
    else if (state == TESLA_STATE_SERVICE_DISCOVERED) xEventGroupSetBits(g_events, EVT_SERVICE_DISCOVERED);
    else if (state == TESLA_STATE_AUTHENTICATED) xEventGroupSetBits(g_events, EVT_AUTHENTICATED);
    else if (state == TESLA_STATE_DISCONNECTED) xEventGroupSetBits(g_events, EVT_DISCONNECTED);
}

static const char *gear_name(tesla_gear_state_t gear)
{
    switch (gear) {
    case TESLA_GEAR_P: return "P";
    case TESLA_GEAR_R: return "R";
    case TESLA_GEAR_N: return "N";
    case TESLA_GEAR_D: return "D";
    case TESLA_GEAR_SNA: return "SNA";
    default: return "未知";
    }
}

static void drive_log_task(void *argument)
{
    (void)argument;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(5000));
        xSemaphoreTake(g_lock, portMAX_DELAY);
        bool connected = g_connection_state == TESLA_STATE_AUTHENTICATED;
        bool valid = g_drive_log_valid;
        tesla_drive_state_t drive = g_drive_log_state;
        TickType_t updated = g_drive_log_updated;
        xSemaphoreGive(g_lock);
        if (!connected) continue;
        if (valid) {
            uint32_t age_sec = (uint32_t)((xTaskGetTickCount() - updated) / pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "车速=%.1f km/h，档位=%s，数据更新于 %lu 秒前",
                     (double)drive.speed_kmh, gear_name(drive.gear), (unsigned long)age_sec);
        } else {
            ESP_LOGW(TAG, "车速/档位：等待车辆驾驶状态数据");
        }
    }
}

static esp_err_t scan_and_wait_for_service(const char *vin)
{
    xEventGroupClearBits(g_events, EVT_CONNECTED | EVT_SERVICE_DISCOVERED | EVT_AUTHENTICATED | EVT_DISCONNECTED);
    set_stage(TESLA_STAGE_SCANNING, TESLA_ERROR_NONE);
    /* Keep the phone peripheral discoverable while the central searches for the car. */
    esp_err_t err = tesla_ble_scan(vin);
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(g_events, EVT_CONNECTED | EVT_DISCONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & EVT_CONNECTED) == 0U) {
        tesla_ble_scan_stop(); return ESP_ERR_TIMEOUT;
    }
    set_stage(TESLA_STAGE_CONNECTING, TESLA_ERROR_NONE);
    bits = xEventGroupWaitBits(g_events, EVT_SERVICE_DISCOVERED | EVT_DISCONNECTED, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    return (bits & EVT_SERVICE_DISCOVERED) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t monitor_vehicle(void)
{
    while (tesla_ble_get_state() == TESLA_STATE_AUTHENTICATED) {
        tesla_ble_get_body_state();
        if (tesla_ble_get_drive_state() == ESP_OK) {
            tesla_vehicle_state_t state;
            if (tesla_ble_get_vehicle_state(&state) == ESP_OK) {
                xSemaphoreTake(g_lock, portMAX_DELAY);
                if (g_connection_state == TESLA_STATE_AUTHENTICATED) {
                    g_drive_log_state = state.drive;
                    g_drive_log_updated = xTaskGetTickCount();
                    g_drive_log_valid = true;
                }
                xSemaphoreGive(g_lock);
            }
        }
        if ((xEventGroupWaitBits(g_events, EVT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)) & EVT_DISCONNECTED) != 0U) break;
    }
    return ESP_OK;
}

static esp_err_t pair_vehicle(const char *vin, uint32_t revision)
{
    esp_err_t err = scan_and_wait_for_service(vin);
    if (err != ESP_OK) return err;
    err = tesla_ble_add_key_request();
    if (err != ESP_OK) { tesla_ble_disconnect(); return err; }
    set_stage(TESLA_STAGE_WAITING_APPROVAL, TESLA_ERROR_NONE);
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(PAIRING_APPROVAL_MS);
    while ((xTaskGetTickCount() - started) < timeout) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        bool superseded = revision != g_revision;
        xSemaphoreGive(g_lock);
        if (superseded) return ESP_ERR_INVALID_STATE;
        if (tesla_ble_get_state() == TESLA_STATE_DISCONNECTED) {
            err = scan_and_wait_for_service(vin);
            if (err != ESP_OK) return err;
            set_stage(TESLA_STAGE_WAITING_APPROVAL, TESLA_ERROR_NONE);
        }
        /* Probe approval now; a successful session is the confirmation. */
        err = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
        if (err == ESP_OK) {
            err = save_paired(true, revision);
            if (err != ESP_OK) {
                tesla_ble_disconnect();
                return err == ESP_ERR_INVALID_STATE ? err : TESLA_LINK_ERR_SAVE_FAILED;
            }
            ESP_LOGI(TAG, "Pairing confirmed by authenticated vehicle session");
            set_stage(TESLA_STAGE_PAIRED, TESLA_ERROR_NONE);
            set_stage(TESLA_STAGE_AUTHENTICATED, TESLA_ERROR_NONE);
            return monitor_vehicle();
        }
        if (err != TESLA_ERR_KEY_NOT_ON_WHITELIST && err != ESP_ERR_TIMEOUT) {
            tesla_ble_disconnect();
            return err;
        }
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) break;
        TickType_t remaining = timeout - elapsed;
        TickType_t interval = pdMS_TO_TICKS(2000);
        if (interval > remaining) interval = remaining;
        /* A new App request wakes the approval loop early. */
        ulTaskNotifyTake(pdTRUE, interval);
    }
    tesla_ble_disconnect();
    ESP_LOGW(TAG, "Pairing approval window expired; keeping paired=false");
    return err == ESP_ERR_TIMEOUT ? ESP_FAIL : err;
}

static esp_err_t connect_vehicle(const char *vin)
{
    esp_err_t err = scan_and_wait_for_service(vin);
    if (err != ESP_OK) return err;
    err = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
    if (err != ESP_OK) {
        tesla_ble_disconnect();
        /* Session timeouts are authentication errors, not scan failures. */
        return err == ESP_ERR_TIMEOUT ? ESP_FAIL : err;
    }
    set_stage(TESLA_STAGE_AUTHENTICATED, TESLA_ERROR_NONE);
    return monitor_vehicle();
}

static void tesla_task(void *argument)
{
    (void)argument;
    while (true) {
        char vin[18];
        xSemaphoreTake(g_lock, portMAX_DELAY);
        bool configured = g_configured, paired = g_paired;
        uint32_t revision = g_revision;
        strlcpy(vin, g_vin, sizeof(vin));
        xSemaphoreGive(g_lock);
        if (!configured) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); continue; }
        esp_err_t err = paired ? connect_vehicle(vin) : pair_vehicle(vin, revision);
        if (paired && err == TESLA_ERR_KEY_NOT_ON_WHITELIST) {
            set_stage(TESLA_STAGE_FAILED, TESLA_ERROR_AUTH_FAILED);
            ESP_LOGW(TAG, "Vehicle rejected saved key; clearing paired flag and requesting approval again");
            esp_err_t saved = save_paired(false, revision);
            if (saved == ESP_OK) {
                paired = false;
                /* Wait for the previous connection to close before resetting scan events. */
                EventBits_t bits = xEventGroupWaitBits(g_events, EVT_DISCONNECTED, pdFALSE, pdFALSE,
                                                       pdMS_TO_TICKS(3000));
                if ((bits & EVT_DISCONNECTED) != 0U ||
                    tesla_ble_get_state() == TESLA_STATE_DISCONNECTED) {
                    err = pair_vehicle(vin, revision);
                }
            } else {
                err = saved == ESP_ERR_INVALID_STATE ? saved : TESLA_LINK_ERR_SAVE_FAILED;
            }
        }
        xSemaphoreTake(g_lock, portMAX_DELAY); bool superseded = revision != g_revision; xSemaphoreGive(g_lock);
        if (!superseded && err != ESP_OK) {
            uint8_t error = err == TESLA_LINK_ERR_SAVE_FAILED ? TESLA_ERROR_SAVE_FAILED
                : err == ESP_ERR_TIMEOUT ? TESLA_ERROR_SCAN_TIMEOUT
                : err == TESLA_ERR_KEY_NOT_ON_WHITELIST || paired ? TESLA_ERROR_AUTH_FAILED
                : TESLA_ERROR_PAIRING_FAILED;
            set_stage(TESLA_STAGE_FAILED, error);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

esp_err_t tesla_link_start(void)
{
    g_lock = xSemaphoreCreateMutex(); g_events = xEventGroupCreate();
    if (g_lock == NULL || g_events == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = tesla_ble_init();
    if (err != ESP_OK) return err;
    err = load_or_create_key();
    if (err != ESP_OK) return err;
    load_configuration();
    tesla_ble_set_state_callback(state_changed, NULL);
    if (xTaskCreate(drive_log_task, "tesla_drive_log", 3072, NULL, 4, &g_drive_log_task) != pdPASS) {
        g_drive_log_task = NULL; return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(tesla_task, "tesla_link", 8192, NULL, 5, &g_task) != pdPASS) {
        vTaskDelete(g_drive_log_task); g_drive_log_task = NULL;
        g_task = NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void tesla_link_set_ble_notify_handler(tesla_link_ble_notify_t handler) { g_notify_handler = handler; }

bool tesla_link_handle_ble_frame(uint16_t connection_handle, const uint8_t *frame, size_t length)
{
    if (!validate_frame(frame, length)) return false;
    uint8_t command = frame[3];
    const uint8_t *payload = frame + 4U;
    size_t payload_length = length - 6U;
    if (command == TESLA_COMMAND_QUERY_STATUS) {
        if (payload_length == 0U) {
            xSemaphoreTake(g_lock, portMAX_DELAY);
            g_request_connection = connection_handle;
            xSemaphoreGive(g_lock);
            notify_status(TESLA_COMMAND_STATUS, connection_handle);
        }
        return true;
    }
    if (command != TESLA_COMMAND_CONFIGURE) return false;
    if (payload_length != 18U || payload[0] != 17U) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_stage = TESLA_STAGE_FAILED; g_error = TESLA_ERROR_INVALID_VIN;
        xSemaphoreGive(g_lock);
        notify_status(TESLA_COMMAND_EVENT, connection_handle);
        return true;
    }
    char vin[18]; memcpy(vin, payload + 1U, 17U); vin[17] = '\0';
    for (size_t i = 0; i < 17U; ++i) vin[i] = (char)toupper((unsigned char)vin[i]);
    if (!is_valid_vin(vin)) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_stage = TESLA_STAGE_FAILED; g_error = TESLA_ERROR_INVALID_VIN;
        xSemaphoreGive(g_lock);
        notify_status(TESLA_COMMAND_EVENT, connection_handle);
        return true;
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool vin_changed = strcmp(g_vin, vin) != 0;
    strlcpy(g_vin, vin, sizeof(g_vin)); g_configured = true;
    if (vin_changed) g_paired = false;
    g_stage = TESLA_STAGE_SAVED; g_error = TESLA_ERROR_NONE;
    g_request_connection = connection_handle; ++g_revision;
    xSemaphoreGive(g_lock);
    tesla_ble_scan_stop();
    tesla_ble_disconnect();
    if (save_configuration() != ESP_OK) {
        set_stage(TESLA_STAGE_FAILED, TESLA_ERROR_SAVE_FAILED);
        return true;
    }
    notify_status(TESLA_COMMAND_EVENT, connection_handle);
    if (g_task != NULL) xTaskNotifyGive(g_task);
    return true;
}

void tesla_link_handle_phone_disconnect(uint16_t connection_handle)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_request_connection == connection_handle) g_request_connection = UINT16_MAX;
    xSemaphoreGive(g_lock);
}
