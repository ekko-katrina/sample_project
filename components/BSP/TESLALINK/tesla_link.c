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

#define EVT_CONNECTED BIT0
#define EVT_SERVICE_DISCOVERED BIT1
#define EVT_AUTHENTICATED BIT2
#define EVT_DISCONNECTED BIT3

#define CONNECT_TIMEOUT_MS 30000U
#define PAIRING_APPROVAL_MS 60000U
#define RECONNECT_DELAY_MS 5000U

static const char *NVS_NAMESPACE = "tesla_link";
static const char *NVS_KEY_VIN = "vin";
static const char *NVS_KEY_PRIVATE = "priv_key";
static const char *NVS_KEY_PAIRED = "paired";

static SemaphoreHandle_t g_lock;
static EventGroupHandle_t g_events;
static TaskHandle_t g_task;
static tesla_link_ble_notify_t g_notify_handler;
static char g_vin[18];
static bool g_configured;
static bool g_paired;
static uint8_t g_connection_state;
static uint8_t g_stage;
static uint8_t g_error;
static uint16_t g_request_id;
static uint16_t g_request_connection;
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

static void save_paired(bool paired)
{
    g_paired = paired;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u8(handle, NVS_KEY_PAIRED, paired ? 1U : 0U) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
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

static esp_err_t load_or_create_key(bool regenerate)
{
    uint8_t private_key[32];
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    size_t length = sizeof(private_key);
    err = regenerate ? ESP_ERR_NVS_NOT_FOUND : nvs_get_blob(handle, NVS_KEY_PRIVATE, private_key, &length);
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

static void notify_status(uint8_t command, uint16_t request_id, uint16_t connection_handle)
{
    if (g_notify_handler == NULL) return;
    uint8_t payload[25], frame[32];
    xSemaphoreTake(g_lock, portMAX_DELAY);
    size_t vin_length = g_configured ? strlen(g_vin) : 0U;
    payload[0] = (uint8_t)(request_id >> 8U); payload[1] = (uint8_t)request_id;
    payload[2] = g_configured ? 1U : 0U; payload[3] = g_paired ? 1U : 0U;
    payload[4] = g_connection_state; payload[5] = g_stage; payload[6] = g_error;
    payload[7] = (uint8_t)vin_length;
    if (vin_length > 0U) memcpy(payload + 8U, g_vin, vin_length);
    xSemaphoreGive(g_lock);
    size_t length = build_frame(command, payload, 8U + vin_length, frame, sizeof(frame));
    if (length > 0U) g_notify_handler(connection_handle, frame, length);
}

static void set_stage(uint8_t stage, uint8_t error)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_stage = stage; g_error = error;
    uint16_t connection_handle = g_request_connection, request_id = g_request_id;
    xSemaphoreGive(g_lock);
    if (connection_handle != 0U) notify_status(TESLA_COMMAND_EVENT, request_id, connection_handle);
}

static void state_changed(tesla_conn_state_t state, void *argument)
{
    (void)argument;
    xSemaphoreTake(g_lock, portMAX_DELAY); g_connection_state = (uint8_t)state; xSemaphoreGive(g_lock);
    if (state == TESLA_STATE_CONNECTED) xEventGroupSetBits(g_events, EVT_CONNECTED);
    else if (state == TESLA_STATE_SERVICE_DISCOVERED) xEventGroupSetBits(g_events, EVT_SERVICE_DISCOVERED);
    else if (state == TESLA_STATE_AUTHENTICATED) xEventGroupSetBits(g_events, EVT_AUTHENTICATED);
    else if (state == TESLA_STATE_DISCONNECTED) xEventGroupSetBits(g_events, EVT_DISCONNECTED);
}

static esp_err_t scan_and_wait_for_service(const char *vin)
{
    xEventGroupClearBits(g_events, EVT_CONNECTED | EVT_SERVICE_DISCOVERED | EVT_AUTHENTICATED | EVT_DISCONNECTED);
    set_stage(TESLA_STAGE_SCANNING, TESLA_ERROR_NONE);
    ble_phone_advertising_pause();
    esp_err_t err = tesla_ble_scan(vin);
    if (err != ESP_OK) { ble_phone_advertising_resume(); return err; }
    EventBits_t bits = xEventGroupWaitBits(g_events, EVT_CONNECTED | EVT_DISCONNECTED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if ((bits & EVT_CONNECTED) == 0U) {
        tesla_ble_scan_stop(); ble_phone_advertising_resume(); return ESP_ERR_TIMEOUT;
    }
    set_stage(TESLA_STAGE_CONNECTING, TESLA_ERROR_NONE);
    ble_phone_advertising_resume();
    bits = xEventGroupWaitBits(g_events, EVT_SERVICE_DISCOVERED | EVT_DISCONNECTED, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    return (bits & EVT_SERVICE_DISCOVERED) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t pair_vehicle(const char *vin, uint32_t revision)
{
    esp_err_t err = scan_and_wait_for_service(vin);
    if (err != ESP_OK) return err;
    err = tesla_ble_add_key_request();
    if (err != ESP_OK) { tesla_ble_disconnect(); return err; }
    set_stage(TESLA_STAGE_WAITING_APPROVAL, TESLA_ERROR_NONE);
    xEventGroupWaitBits(g_events, EVT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(PAIRING_APPROVAL_MS));
    tesla_ble_disconnect();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool superseded = revision != g_revision;
    xSemaphoreGive(g_lock);
    if (superseded) return ESP_ERR_INVALID_STATE;
    save_paired(true);
    set_stage(TESLA_STAGE_PAIRED, TESLA_ERROR_NONE);
    return ESP_OK;
}

static esp_err_t connect_vehicle(const char *vin)
{
    esp_err_t err = scan_and_wait_for_service(vin);
    if (err != ESP_OK) return err;
    err = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
    if (err != ESP_OK) { tesla_ble_disconnect(); return err; }
    set_stage(TESLA_STAGE_AUTHENTICATED, TESLA_ERROR_NONE);
    while (tesla_ble_get_state() == TESLA_STATE_AUTHENTICATED) {
        tesla_ble_get_body_state();
        tesla_ble_get_drive_state();
        if ((xEventGroupWaitBits(g_events, EVT_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000)) & EVT_DISCONNECTED) != 0U) break;
    }
    return ESP_OK;
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
        xSemaphoreTake(g_lock, portMAX_DELAY); bool superseded = revision != g_revision; xSemaphoreGive(g_lock);
        if (!superseded && err != ESP_OK) {
            set_stage(TESLA_STAGE_FAILED, err == ESP_ERR_TIMEOUT ? TESLA_ERROR_SCAN_TIMEOUT
                                                                 : (paired ? TESLA_ERROR_AUTH_FAILED : TESLA_ERROR_PAIRING_FAILED));
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
    err = load_or_create_key(false);
    if (err != ESP_OK) return err;
    load_configuration();
    tesla_ble_set_state_callback(state_changed, NULL);
    if (xTaskCreate(tesla_task, "tesla_link", 8192, NULL, 5, &g_task) != pdPASS) {
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
        if (payload_length == 2U) {
            uint16_t request_id = (uint16_t)(((uint16_t)payload[0] << 8U) | payload[1]);
            notify_status(TESLA_COMMAND_STATUS, request_id, connection_handle);
        }
        return true;
    }
    if (command != TESLA_COMMAND_CONFIGURE) return false;
    uint16_t request_id = payload_length >= 2U ? (uint16_t)(((uint16_t)payload[0] << 8U) | payload[1]) : 0U;
    if (payload_length != 21U || payload[3] != 17U) {
        xSemaphoreTake(g_lock, portMAX_DELAY); g_error = TESLA_ERROR_INVALID_VIN; xSemaphoreGive(g_lock);
        notify_status(TESLA_COMMAND_EVENT, request_id, connection_handle);
        return true;
    }
    char vin[18]; memcpy(vin, payload + 4U, 17U); vin[17] = '\0';
    for (size_t i = 0; i < 17U; ++i) vin[i] = (char)toupper((unsigned char)vin[i]);
    if (!is_valid_vin(vin)) {
        xSemaphoreTake(g_lock, portMAX_DELAY); g_error = TESLA_ERROR_INVALID_VIN; xSemaphoreGive(g_lock);
        notify_status(TESLA_COMMAND_EVENT, request_id, connection_handle);
        return true;
    }
    bool regenerate = (payload[2] & 0x01U) != 0U;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool vin_changed = strcmp(g_vin, vin) != 0;
    strlcpy(g_vin, vin, sizeof(g_vin)); g_configured = true;
    if (vin_changed || regenerate) g_paired = false;
    g_stage = TESLA_STAGE_SAVED; g_error = TESLA_ERROR_NONE;
    g_request_id = request_id; g_request_connection = connection_handle; ++g_revision;
    xSemaphoreGive(g_lock);
    tesla_ble_scan_stop();
    tesla_ble_disconnect();
    if (save_configuration() != ESP_OK || (regenerate && load_or_create_key(true) != ESP_OK)) {
        set_stage(TESLA_STAGE_FAILED, TESLA_ERROR_SAVE_FAILED);
        return true;
    }
    notify_status(TESLA_COMMAND_EVENT, request_id, connection_handle);
    if (g_task != NULL) xTaskNotifyGive(g_task);
    return true;
}

void tesla_link_handle_phone_disconnect(uint16_t connection_handle)
{
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_request_connection == connection_handle) g_request_connection = 0U;
    xSemaphoreGive(g_lock);
}
