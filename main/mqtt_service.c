#include "mqtt_service.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "myble.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_provisioning.h"
#include "remote_frame.h"

#define TAG "FRIDGE-MQTT"
#define MQTT_SN_NAMESPACE "mqtt_cfg"
#define MQTT_SN_KEY "sn"
#define MQTT_SN_MAX_LENGTH 64U
#define MQTT_TOPIC_CAPACITY 96U
#define MQTT_COMMAND_MAX_LENGTH 1024U
#define MQTT_WORK_QUEUE_LENGTH 12U
#define MQTT_STATE_PERIOD_SECONDS 300U
#define MQTT_VIEWER_TIMEOUT_SECONDS 90U
#define MQTT_RECONNECT_INITIAL_MS 2000U
#define MQTT_RECONNECT_MAX_MS 60000U

typedef enum {
    MQTT_WORK_TRANSPORT_CONNECTED,
    MQTT_WORK_DISCONNECTED,
    MQTT_WORK_READY,
    MQTT_WORK_COMMAND,
    MQTT_WORK_VIEWER_STATUS,
    MQTT_WORK_STATE_CHANGED,
} mqtt_work_type_t;

typedef struct {
    mqtt_work_type_t type;
    union {
        struct {
            char *json;
            size_t length;
        } command;
        struct {
            tmy06_state_t before;
            tmy06_state_t after;
        } state;
    } data;
} mqtt_work_item_t;

static QueueHandle_t g_work_queue;
static SemaphoreHandle_t g_offline_ack;
static TaskHandle_t g_service_task;
static esp_mqtt_client_handle_t g_client;
static bool g_mqtt_ready;
static bool g_viewer_online;
static bool g_applying_mqtt_command;
static bool g_cmd_subscribed;
static bool g_viewer_subscribed;
static int g_cmd_subscribe_message_id = -1;
static int g_viewer_subscribe_message_id = -1;
static int g_offline_message_id = -1;
static char g_sn[MQTT_SN_MAX_LENGTH + 1U];
static char g_topic_cmd[MQTT_TOPIC_CAPACITY];
static char g_topic_online[MQTT_TOPIC_CAPACITY];
static char g_topic_state[MQTT_TOPIC_CAPACITY];
static char g_topic_viewer[MQTT_TOPIC_CAPACITY];
static char g_topic_event[MQTT_TOPIC_CAPACITY];
static SemaphoreHandle_t g_remote_lock;
static char g_remote_session[65];
static char g_recent_requests[16][129];
static size_t g_recent_request_index;
static const ble_sim_profile_info_t *g_profile_info;
static char g_rx_buffer[MQTT_COMMAND_MAX_LENGTH + 1U];
static size_t g_rx_expected;
typedef enum {
    MQTT_RX_NONE,
    MQTT_RX_COMMAND,
    MQTT_RX_VIEWER_STATUS,
} mqtt_rx_type_t;
static mqtt_rx_type_t g_rx_type;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);

static const char *test_sn_for_profile(ble_sim_profile_t profile)
{
    static const char *const test_sns[BLE_SIM_PROFILE_COUNT] = {
        [BLE_SIM_PROFILE_LEGACY_Y06] = "TMY06-LEGACY-001",
        [BLE_SIM_PROFILE_NEW_Y17] = "TMY17-TEST-001",
        [BLE_SIM_PROFILE_NEW_Y16] = "TMY16-TEST-001",
        [BLE_SIM_PROFILE_NEW_TM310] = "TM310-TEST-001",
        [BLE_SIM_PROFILE_NEW_Y06] = "TMY06-TEST-001",
    };
    return (unsigned int)profile < BLE_SIM_PROFILE_COUNT
               ? test_sns[profile]
               : NULL;
}

static bool sn_is_valid(const char *sn)
{
    size_t length = sn == NULL ? 0U : strlen(sn);
    if (length == 0U || length > MQTT_SN_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        char character = sn[i];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
}

static esp_err_t load_device_sn(ble_sim_profile_t profile)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(MQTT_SN_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t length = sizeof(g_sn);
        err = nvs_get_str(handle, MQTT_SN_KEY, g_sn, &length);
        nvs_close(handle);
        if (err == ESP_OK) {
            if (!sn_is_valid(g_sn)) {
                ESP_LOGE(TAG, "NVS SN is invalid");
                return ESP_ERR_INVALID_ARG;
            }
            ESP_LOGI(TAG, "Using device SN from NVS: %s", g_sn);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "Cannot read NVS SN: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Cannot open MQTT NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_TMY06_MQTT_ALLOW_TEST_SN
    const char *test_sn = test_sn_for_profile(profile);
    if (test_sn == NULL || !sn_is_valid(test_sn)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(g_sn, test_sn, sizeof(g_sn));
    ESP_LOGW(TAG, "NVS SN is absent; using test SN: %s", g_sn);
    return ESP_OK;
#else
    ESP_LOGE(TAG, "NVS SN is absent; MQTT will not start");
    return ESP_ERR_NOT_FOUND;
#endif
}

static bool make_topics(void)
{
    return snprintf(g_topic_cmd, sizeof(g_topic_cmd), "fridge/%s/cmd", g_sn) > 0 &&
           snprintf(g_topic_online, sizeof(g_topic_online), "fridge/%s/online", g_sn) > 0 &&
           snprintf(g_topic_state, sizeof(g_topic_state), "fridge/%s/state", g_sn) > 0 &&
           snprintf(g_topic_viewer, sizeof(g_topic_viewer), "fridge/%s/viewer", g_sn) > 0 &&
           snprintf(g_topic_event, sizeof(g_topic_event), "fridge/%s/event", g_sn) > 0;
}

static int mqtt_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!g_mqtt_ready || g_client == NULL) {
        return -1;
    }
    int message_id = esp_mqtt_client_publish(g_client, topic, payload, 0,
                                              qos, retain);
    if (message_id < 0) {
        ESP_LOGW(TAG, "Publish failed: %s", topic);
    }
    return message_id;
}

static void publish_online(void)
{
    mqtt_publish(g_topic_online, "{\"online\":true}", 1, 1);
}

static const char *unit_name(tmy06_temperature_unit_t unit)
{
    return unit == TMY06_UNIT_FAHRENHEIT ? "FAHRENHEIT" : "CELSIUS";
}

static const char *mode_name(tmy06_mode_t mode)
{
    switch (mode) {
    case TMY06_MODE_MAX:
        return "MAX";
    case TMY06_MODE_ECO:
        return "ECO";
    case TMY06_MODE_HEATING:
        return "HEAT";
    case TMY06_MODE_COOLING:
    default:
        return "COOL";
    }
}

static void publish_state(void)
{
    tmy06_state_t state;
    if (!ble_tmy06_get_state(&state)) {
        ESP_LOGW(TAG, "Cannot read state for MQTT publish");
        return;
    }

    wifi_ap_record_t access_point;
    bool have_rssi = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject(root, "powerOn", state.power == TMY06_POWER_ON);
    cJSON_AddNumberToObject(root, "protocolFrames", 1);
    cJSON_AddStringToObject(root, "unit", unit_name(state.unit));
    cJSON_AddStringToObject(root, "mode", mode_name(state.mode));
    if (g_profile_info->has_child_lock) {
        cJSON_AddBoolToObject(root, "childLock",
                              state.child_lock == TMY06_CHILD_LOCK_ON);
    }
    cJSON_AddNumberToObject(root, "targetTemp", state.target_temperature);
    cJSON_AddNumberToObject(root, "currentTemp", state.actual_temperature);
    if (g_profile_info->has_voltage) {
        cJSON_AddNumberToObject(root, "voltage",
                                (double)state.actual_voltage_centi_volts / 100.0);
    }
    cJSON_AddNumberToObject(root, "fault", state.fault_code);
    if (have_rssi) {
        cJSON_AddNumberToObject(root, "rssi", access_point.rssi);
    }
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000LL);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload != NULL) {
        mqtt_publish(g_topic_state, payload, 1, 1);
        cJSON_free(payload);
    }
}

static void publish_state_if_viewer_online(void)
{
    if (g_viewer_online) {
        publish_state();
    }
}

static bool json_string_is_valid(const cJSON *item, size_t maximum_length)
{
    return cJSON_IsString(item) && item->valuestring != NULL &&
           item->valuestring[0] != '\0' &&
           strlen(item->valuestring) <= maximum_length;
}

static bool json_integer_to_int16(const cJSON *item, int16_t *value)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < INT16_MIN ||
        item->valuedouble > INT16_MAX) {
        return false;
    }
    int16_t converted = (int16_t)item->valuedouble;
    if ((double)converted != item->valuedouble) {
        return false;
    }
    *value = converted;
    return true;
}

static cJSON *required_params_object(cJSON *root)
{
    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    return cJSON_IsObject(params) ? params : NULL;
}

static bool utf8_is_valid(const uint8_t *data, size_t length)
{
    size_t i = 0U;
    while (i < length) {
        uint8_t first = data[i++];
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7FU) {
            continue;
        }
        size_t continuation_count;
        uint8_t second_min = 0x80U;
        uint8_t second_max = 0xBFU;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            if (first == 0xE0U) {
                second_min = 0xA0U;
            } else if (first == 0xEDU) {
                second_max = 0x9FU;
            }
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            if (first == 0xF0U) {
                second_min = 0x90U;
            } else if (first == 0xF4U) {
                second_max = 0x8FU;
            }
        } else {
            return false;
        }
        if (i + continuation_count > length || data[i] < second_min ||
            data[i] > second_max) {
            return false;
        }
        ++i;
        for (size_t continuation = 1U; continuation < continuation_count;
             ++continuation, ++i) {
            if (data[i] < 0x80U || data[i] > 0xBFU) {
                return false;
            }
        }
    }
    return true;
}

static void publish_remote_json(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        mqtt_publish(g_topic_event, json, 1, 0);
        cJSON_free(json);
    }
    cJSON_Delete(root);
}

static void remote_frame_notification(const uint8_t *frame, size_t length)
{
    if (length == 0U || length > 160U || g_remote_lock == NULL) return;
    char session[sizeof(g_remote_session)];
    xSemaphoreTake(g_remote_lock, portMAX_DELAY);
    strlcpy(session, g_remote_session, sizeof(session));
    xSemaphoreGive(g_remote_lock);
    if (session[0] == '\0') return;
    char hex[321];
    const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        hex[i * 2U] = digits[frame[i] >> 4];
        hex[i * 2U + 1U] = digits[frame[i] & 15];
    }
    hex[length * 2U] = '\0';
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON_AddStringToObject(root, "type", "frame");
    cJSON_AddStringToObject(root, "sessionId", session);
    cJSON_AddStringToObject(root, "frame", hex);
    publish_remote_json(root);
}

static void handle_remote_frame(cJSON *root)
{
    cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "sessionId");
    cJSON *request = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    if (!json_string_is_valid(session, 64U) || !json_string_is_valid(request, 128U)) return;
    cJSON *params = required_params_object(root);
    cJSON *hex = cJSON_GetObjectItemCaseSensitive(params, "frame");
    uint8_t frame[160];
    size_t length = cJSON_IsString(hex) ? remote_decode_frame(hex->valuestring, frame, sizeof(frame)) : 0U;
    bool duplicate = false;
    for (size_t i = 0; i < 16U; ++i) {
        if (strcmp(g_recent_requests[i], request->valuestring) == 0) duplicate = true;
    }
    xSemaphoreTake(g_remote_lock, portMAX_DELAY);
    strlcpy(g_remote_session, session->valuestring, sizeof(g_remote_session));
    xSemaphoreGive(g_remote_lock);
    bool accepted = duplicate || (length > 0U && app_remote_frame_supported(frame, length));
    cJSON *reply = cJSON_CreateObject();
    if (reply == NULL) return;
    cJSON_AddStringToObject(reply, "type", "result");
    cJSON_AddStringToObject(reply, "sessionId", session->valuestring);
    cJSON_AddStringToObject(reply, "requestId", request->valuestring);
    cJSON_AddBoolToObject(reply, "accepted", accepted);
    if (!accepted) cJSON_AddStringToObject(reply, "error", "Unsupported or invalid remote command");
    publish_remote_json(reply);
    // Acceptance is distinct from operation success, reported by device frames.
    // Publish acceptance before a network change can take MQTT offline.
    if (accepted && !duplicate) {
        strlcpy(g_recent_requests[g_recent_request_index++ % 16U], request->valuestring, 129U);
        app_process_remote_frame(frame, length);
    }
}

static void handle_command(char *json, size_t json_length)
{
    if (!utf8_is_valid((const uint8_t *)json, json_length)) {
        ESP_LOGW(TAG, "Ignoring non-UTF-8 MQTT command");
        return;
    }
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_length + 1U,
                                            NULL, true);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Ignoring invalid MQTT command JSON");
        cJSON_Delete(root);
        return;
    }

    cJSON *command_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!json_string_is_valid(command_item, 64U)) {
        ESP_LOGW(TAG, "Ignoring MQTT command without a valid cmd");
        cJSON_Delete(root);
        return;
    }

    const char *command = command_item->valuestring;
    if (strcmp(command, "PROTOCOL_FRAME") == 0) {
        handle_remote_frame(root);
        cJSON_Delete(root);
        return;
    }
    tmy06_state_t state;
    if (!ble_tmy06_get_state(&state)) {
        ESP_LOGE(TAG, "Cannot read state for MQTT command %s", command);
        cJSON_Delete(root);
        return;
    }

    bool get_state = strcmp(command, "GET_STATE") == 0;
    bool setting_command = !get_state;
    const char *error_code = NULL;
    const char *error_message = NULL;
    cJSON *params = NULL;

    if (get_state) {
        setting_command = false;
    } else if (strcmp(command, "SET_POWER") == 0) {
        params = required_params_object(root);
        cJSON *value = params == NULL ? NULL
                                      : cJSON_GetObjectItemCaseSensitive(params, "powerOn");
        if (!cJSON_IsBool(value)) {
            error_code = "INVALID_PARAMETER";
            error_message = "powerOn must be boolean";
        } else {
            state.power = cJSON_IsTrue(value) ? TMY06_POWER_ON : TMY06_POWER_OFF;
        }
    } else if (strcmp(command, "SET_MODE") == 0) {
        params = required_params_object(root);
        cJSON *value = params == NULL ? NULL
                                      : cJSON_GetObjectItemCaseSensitive(params, "mode");
        if (!json_string_is_valid(value, 16U)) {
            error_code = "INVALID_PARAMETER";
            error_message = "mode must be string";
        } else {
            tmy06_mode_t requested_mode;
            bool known_mode = true;
            if (strcmp(value->valuestring, "COOL") == 0) {
                requested_mode = TMY06_MODE_COOLING;
            } else if (strcmp(value->valuestring, "MAX") == 0) {
                requested_mode = TMY06_MODE_MAX;
            } else if (strcmp(value->valuestring, "ECO") == 0) {
                requested_mode = TMY06_MODE_ECO;
            } else if (strcmp(value->valuestring, "HEAT") == 0) {
                requested_mode = TMY06_MODE_HEATING;
            } else {
                known_mode = false;
            }
            if (!known_mode) {
                error_code = "INVALID_PARAMETER";
                error_message = "invalid mode";
            } else if ((g_profile_info->supported_mode_mask &
                        (1U << (unsigned int)requested_mode)) == 0U) {
                error_code = "MODEL_NOT_SUPPORTED";
                error_message = "mode is not supported by this model";
            } else {
                state.mode = requested_mode;
            }
        }
    } else if (strcmp(command, "SET_TEMPERATURE_UNIT") == 0) {
        params = required_params_object(root);
        cJSON *value = params == NULL ? NULL
                                      : cJSON_GetObjectItemCaseSensitive(params, "unit");
        tmy06_temperature_unit_t requested_unit;
        if (!json_string_is_valid(value, 16U)) {
            error_code = "INVALID_PARAMETER";
            error_message = "unit must be string";
        } else if (strcmp(value->valuestring, "CELSIUS") == 0) {
            requested_unit = TMY06_UNIT_CELSIUS;
        } else if (strcmp(value->valuestring, "FAHRENHEIT") == 0) {
            requested_unit = TMY06_UNIT_FAHRENHEIT;
        } else {
            error_code = "INVALID_PARAMETER";
            error_message = "invalid temperature unit";
        }
        if (error_code == NULL && requested_unit != state.unit) {
            if (!fridge_protocol_change_unit(&state, requested_unit)) {
                error_code = "PARAMETER_OUT_OF_RANGE";
                error_message = "converted temperature is out of range";
            }
        }
    } else if (strcmp(command, "SET_TARGET_TEMPERATURE") == 0) {
        params = required_params_object(root);
        cJSON *value = params == NULL ? NULL
                                      : cJSON_GetObjectItemCaseSensitive(params, "temperature");
        if (!json_integer_to_int16(value, &state.target_temperature)) {
            error_code = "INVALID_PARAMETER";
            error_message = "temperature must be an integer";
        }
    } else if (strcmp(command, "SET_CHILD_LOCK") == 0) {
        if (!g_profile_info->has_child_lock) {
            error_code = "MODEL_NOT_SUPPORTED";
            error_message = "child lock is not supported by this model";
        } else {
            params = required_params_object(root);
            cJSON *value = params == NULL ? NULL
                                          : cJSON_GetObjectItemCaseSensitive(params, "locked");
            if (!cJSON_IsBool(value)) {
                error_code = "INVALID_PARAMETER";
                error_message = "locked must be boolean";
            } else {
                state.child_lock = cJSON_IsTrue(value) ? TMY06_CHILD_LOCK_ON
                                                       : TMY06_CHILD_LOCK_OFF;
            }
        }
    } else {
        setting_command = false;
        error_code = "INVALID_COMMAND";
        error_message = "unknown command";
    }

    if (error_code != NULL) {
        ESP_LOGW(TAG, "Rejected MQTT command %s: %s (%s)",
                 command, error_code, error_message);
    } else if (setting_command) {
        g_applying_mqtt_command = true;
        bool applied = ble_tmy06_set_state(&state);
        g_applying_mqtt_command = false;
        if (!applied) {
            ESP_LOGE(TAG, "MQTT command %s failed to update state", command);
        } else {
            publish_state_if_viewer_online();
        }
    } else {
        publish_state_if_viewer_online();
    }

    cJSON_Delete(root);
}

static bool parse_viewer_status(const char *json, size_t json_length,
                                bool *online)
{
    if (online == NULL ||
        !utf8_is_valid((const uint8_t *)json, json_length)) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_length + 1U,
                                            NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *online_item = cJSON_GetObjectItemCaseSensitive(root, "online");
    bool valid = cJSON_IsBool(online_item);
    if (valid) {
        *online = cJSON_IsTrue(online_item);
    }
    cJSON_Delete(root);
    return valid;
}

static void state_change_handler(const tmy06_state_t *before,
                                 const tmy06_state_t *after)
{
    if (g_work_queue == NULL ||
        (g_applying_mqtt_command && xTaskGetCurrentTaskHandle() == g_service_task)) {
        return;
    }
    mqtt_work_item_t item = {
        .type = MQTT_WORK_STATE_CHANGED,
        .data.state.before = *before,
        .data.state.after = *after,
    };
    if (xQueueSend(g_work_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "State publish queue is full");
    }
}

static void process_ready(void)
{
    publish_online();
}

static void mqtt_service_task(void *argument)
{
    (void)argument;
    if (!wifi_provisioning_wait_connected(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Wi-Fi wait failed");
        vTaskDelete(NULL);
        return;
    }

    const esp_mqtt_client_config_t config = {
        .broker.address.uri = CONFIG_TMY06_MQTT_BROKER_URI,
        .credentials.client_id = g_sn,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .session.last_will.topic = g_topic_online,
        .session.last_will.msg = "{\"online\":false}",
        .session.last_will.msg_len = 16,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .network.disable_auto_reconnect = true,
    };
    g_client = esp_mqtt_client_init(&config);
    if (g_client == NULL) {
        ESP_LOGE(TAG, "MQTT client initialization failed");
        vTaskDelete(NULL);
        return;
    }
    esp_mqtt_client_register_event(g_client, ESP_EVENT_ANY_ID,
                                   (esp_event_handler_t)mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(g_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT client start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    TickType_t next_state_periodic = xTaskGetTickCount() +
                                     pdMS_TO_TICKS(MQTT_STATE_PERIOD_SECONDS * 1000U);
    TickType_t viewer_last_seen = 0;
    TickType_t reconnect_due = 0;
    uint32_t reconnect_delay_ms = MQTT_RECONNECT_INITIAL_MS;
    bool reconnect_scheduled = false;
    mqtt_work_item_t item;
    while (true) {
        if (xQueueReceive(g_work_queue, &item, pdMS_TO_TICKS(1000)) == pdPASS) {
            if (item.type == MQTT_WORK_TRANSPORT_CONNECTED) {
                reconnect_scheduled = false;
                reconnect_delay_ms = MQTT_RECONNECT_INITIAL_MS;
            } else if (item.type == MQTT_WORK_DISCONNECTED) {
                g_viewer_online = false;
                if (!reconnect_scheduled) {
                    reconnect_due = xTaskGetTickCount() +
                                    pdMS_TO_TICKS(reconnect_delay_ms);
                    reconnect_scheduled = true;
                    reconnect_delay_ms = reconnect_delay_ms < MQTT_RECONNECT_MAX_MS / 2U
                                             ? reconnect_delay_ms * 2U
                                             : MQTT_RECONNECT_MAX_MS;
                }
            } else if (item.type == MQTT_WORK_READY) {
                process_ready();
                next_state_periodic = xTaskGetTickCount() +
                                      pdMS_TO_TICKS(MQTT_STATE_PERIOD_SECONDS * 1000U);
                if (g_viewer_online) {
                    publish_state();
                }
            } else if (item.type == MQTT_WORK_COMMAND) {
                handle_command(item.data.command.json,
                               item.data.command.length);
                free(item.data.command.json);
            } else if (item.type == MQTT_WORK_VIEWER_STATUS) {
                bool online;
                if (!parse_viewer_status(item.data.command.json,
                                         item.data.command.length,
                                         &online)) {
                    ESP_LOGW(TAG, "Ignoring invalid viewer status");
                } else if (online) {
                    bool became_online = !g_viewer_online;
                    g_viewer_online = true;
                    viewer_last_seen = xTaskGetTickCount();
                    if (became_online && g_mqtt_ready) {
                        ESP_LOGI(TAG, "Viewer online; state reporting enabled");
                        publish_state();
                        next_state_periodic = viewer_last_seen +
                            pdMS_TO_TICKS(MQTT_STATE_PERIOD_SECONDS * 1000U);
                    }
                } else {
                    if (g_viewer_online) {
                        ESP_LOGI(TAG, "Viewer offline; state reporting paused");
                    }
                    g_viewer_online = false;
                }
                free(item.data.command.json);
            } else if (item.type == MQTT_WORK_STATE_CHANGED) {
                publish_state_if_viewer_online();
            }
        }
        TickType_t now = xTaskGetTickCount();
        if (reconnect_scheduled && (int32_t)(now - reconnect_due) >= 0) {
            reconnect_scheduled = false;
            esp_err_t reconnect_error = esp_mqtt_client_reconnect(g_client);
            if (reconnect_error != ESP_OK) {
                ESP_LOGW(TAG, "MQTT reconnect start failed: %s",
                         esp_err_to_name(reconnect_error));
                reconnect_due = now + pdMS_TO_TICKS(reconnect_delay_ms);
                reconnect_scheduled = true;
                reconnect_delay_ms = reconnect_delay_ms < MQTT_RECONNECT_MAX_MS / 2U
                                         ? reconnect_delay_ms * 2U
                                         : MQTT_RECONNECT_MAX_MS;
            }
        }
        if (g_viewer_online &&
            now - viewer_last_seen >=
                pdMS_TO_TICKS(MQTT_VIEWER_TIMEOUT_SECONDS * 1000U)) {
            g_viewer_online = false;
            ESP_LOGI(TAG, "Viewer heartbeat timed out; state reporting paused");
        }
        if (g_mqtt_ready && g_viewer_online &&
            (int32_t)(now - next_state_periodic) >= 0) {
            publish_state();
            next_state_periodic = now +
                                  pdMS_TO_TICKS(MQTT_STATE_PERIOD_SECONDS * 1000U);
        }
    }
}

static void queue_complete_message(mqtt_rx_type_t rx_type)
{
    char *copy = malloc(g_rx_expected + 1U);
    if (copy == NULL) {
        ESP_LOGW(TAG, "Cannot allocate MQTT command buffer");
        return;
    }
    memcpy(copy, g_rx_buffer, g_rx_expected);
    copy[g_rx_expected] = '\0';
    mqtt_work_item_t item = {
        .type = rx_type == MQTT_RX_COMMAND ? MQTT_WORK_COMMAND
                                           : MQTT_WORK_VIEWER_STATUS,
        .data.command = {
            .json = copy,
            .length = g_rx_expected,
        },
    };
    if (xQueueSend(g_work_queue, &item, 0) != pdPASS) {
        ESP_LOGW(TAG, "MQTT command queue is full");
        free(copy);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        {
            mqtt_work_item_t item = {.type = MQTT_WORK_TRANSPORT_CONNECTED};
            xQueueSendToFront(g_work_queue, &item, 0);
        }
        g_cmd_subscribed = false;
        g_viewer_subscribed = false;
        g_cmd_subscribe_message_id = esp_mqtt_client_subscribe(g_client,
                                                                g_topic_cmd, 1);
        g_viewer_subscribe_message_id = esp_mqtt_client_subscribe(g_client,
                                                                   g_topic_viewer, 1);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->msg_id == g_cmd_subscribe_message_id) {
            g_cmd_subscribed = true;
        }
        if (event->msg_id == g_viewer_subscribe_message_id) {
            g_viewer_subscribed = true;
        }
        if (!g_mqtt_ready && g_cmd_subscribed && g_viewer_subscribed) {
            g_mqtt_ready = true;
            mqtt_work_item_t item = {.type = MQTT_WORK_READY};
            xQueueSend(g_work_queue, &item, 0);
            ESP_LOGI(TAG, "MQTT ready: %s", g_sn);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_mqtt_ready = false;
        g_viewer_online = false;
        g_rx_expected = 0U;
        g_rx_type = MQTT_RX_NONE;
        {
            mqtt_work_item_t item = {.type = MQTT_WORK_DISCONNECTED};
            xQueueSendToFront(g_work_queue, &item, 0);
        }
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        if (event->msg_id == g_offline_message_id && g_offline_ack != NULL) {
            xSemaphoreGive(g_offline_ack);
        }
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset == 0) {
            g_rx_expected = event->total_data_len > 0
                                ? (size_t)event->total_data_len
                                : (size_t)event->data_len;
            g_rx_type = MQTT_RX_NONE;
            if (event->topic != NULL &&
                !event->retain &&
                event->topic_len == (int)strlen(g_topic_cmd) &&
                memcmp(event->topic, g_topic_cmd,
                       (size_t)event->topic_len) == 0) {
                g_rx_type = MQTT_RX_COMMAND;
            } else if (event->topic != NULL &&
                       event->topic_len == (int)strlen(g_topic_viewer) &&
                       memcmp(event->topic, g_topic_viewer,
                              (size_t)event->topic_len) == 0) {
                g_rx_type = MQTT_RX_VIEWER_STATUS;
            }
            if (g_rx_expected > MQTT_COMMAND_MAX_LENGTH) {
                g_rx_type = MQTT_RX_NONE;
            }
        }
        if (g_rx_type != MQTT_RX_NONE && event->current_data_offset >= 0 &&
            event->data_len >= 0 &&
            (size_t)event->current_data_offset + (size_t)event->data_len <=
                g_rx_expected) {
            memcpy(g_rx_buffer + event->current_data_offset, event->data,
                   (size_t)event->data_len);
            if ((size_t)event->current_data_offset + (size_t)event->data_len ==
                g_rx_expected) {
                queue_complete_message(g_rx_type);
                g_rx_expected = 0U;
                g_rx_type = MQTT_RX_NONE;
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport error");
        break;
    default:
        break;
    }
}

esp_err_t mqtt_service_init(void)
{
    g_remote_lock = xSemaphoreCreateMutex();
    if (g_remote_lock == NULL) return ESP_ERR_NO_MEM;
    app_set_remote_notify_handler(remote_frame_notification);
    ble_sim_profile_t profile = ble_sim_get_active_profile();
    if (profile == BLE_SIM_PROFILE_LEGACY_Y06) {
        ESP_LOGI(TAG, "MQTT disabled for profile %s",
                 ble_sim_profile_info(profile)->display_name);
        return ESP_OK;
    }

    g_profile_info = ble_sim_profile_info(profile);

    esp_err_t err = load_device_sn(profile);
    if (err != ESP_OK) {
        return err;
    }
    ble_set_mqtt_sn(g_sn);
    if (!make_topics()) {
        return ESP_ERR_INVALID_SIZE;
    }
    g_work_queue = xQueueCreate(MQTT_WORK_QUEUE_LENGTH,
                                sizeof(mqtt_work_item_t));
    g_offline_ack = xSemaphoreCreateBinary();
    if (g_work_queue == NULL || g_offline_ack == NULL) {
        if (g_work_queue != NULL) {
            vQueueDelete(g_work_queue);
            g_work_queue = NULL;
        }
        if (g_offline_ack != NULL) {
            vSemaphoreDelete(g_offline_ack);
            g_offline_ack = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    ble_tmy06_set_state_change_handler(state_change_handler);
    if (xTaskCreate(mqtt_service_task, "fridge_mqtt", 7168, NULL, 5,
                    &g_service_task) != pdPASS) {
        ble_tmy06_set_state_change_handler(NULL);
        vQueueDelete(g_work_queue);
        vSemaphoreDelete(g_offline_ack);
        g_work_queue = NULL;
        g_offline_ack = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mqtt_service_prepare_restart(void)
{
    if (!g_mqtt_ready || g_client == NULL || g_offline_ack == NULL) {
        return;
    }
    xSemaphoreTake(g_offline_ack, 0);
    g_offline_message_id = esp_mqtt_client_publish(
        g_client, g_topic_online, "{\"online\":false}", 0, 1, 1);
    if (g_offline_message_id >= 0) {
        xSemaphoreTake(g_offline_ack, pdMS_TO_TICKS(1000));
    }
    g_offline_message_id = -1;
    g_mqtt_ready = false;
    esp_mqtt_client_disconnect(g_client);
}
