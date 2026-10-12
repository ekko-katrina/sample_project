#include "myble.h"
#include "ambient_light.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_provisioning.h"
#include "tesla_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "FRIDGE-BLE"
#define FRIDGE_SERVICE_UUID 0xFFF0
#define MAX_PHONE_CONNECTIONS 2U
#define RX_STREAM_CAPACITY 160U
#define MIN_DATA_LENGTH 3U

typedef struct {
    uint16_t handle;
    bool in_use;
    bool notify_enabled;
    uint8_t rx_stream[RX_STREAM_CAPACITY];
    size_t rx_length;
} phone_connection_t;

static bool g_ble_adv_active;
static uint8_t g_own_addr_type;
static uint16_t g_rx_value_handle;
static uint16_t g_tx_value_handle;
static phone_connection_t g_connections[MAX_PHONE_CONNECTIONS];
static portMUX_TYPE g_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t g_notify_mutex;
static tmy06_state_t g_fridge_state;
static ble_sim_profile_t g_active_profile = BLE_SIM_PROFILE_LEGACY_Y06;
static const ble_sim_profile_info_t *g_profile_info;
static ble_ota_start_handler_t g_ota_start_handler;
static ble_state_change_handler_t g_state_change_handler;
static char g_device_name[32];
/* AD type 0x16 contains its UUID first, in little-endian byte order. */
static uint8_t g_service_data[] = {0xF0, 0xFF, 0x54, 0x4D, 0x01, 0x04};

static const ble_uuid16_t g_service_uuid = BLE_UUID16_INIT(FRIDGE_SERVICE_UUID);
static ble_uuid16_t g_write_uuid = BLE_UUID16_INIT(0xFFF7);
static ble_uuid16_t g_notify_uuid = BLE_UUID16_INIT(0xFFF7);

static int gap_event_handler(struct ble_gap_event *event, void *arg);

static phone_connection_t *find_connection(uint16_t conn_handle)
{
    for (size_t i = 0; i < MAX_PHONE_CONNECTIONS; ++i) {
        if (g_connections[i].in_use && g_connections[i].handle == conn_handle) {
            return &g_connections[i];
        }
    }

    return NULL;
}

static phone_connection_t *add_connection(uint16_t conn_handle)
{
    phone_connection_t *existing = find_connection(conn_handle);
    if (existing != NULL) {
        return existing;
    }

    for (size_t i = 0; i < MAX_PHONE_CONNECTIONS; ++i) {
        if (!g_connections[i].in_use) {
            memset(&g_connections[i], 0, sizeof(g_connections[i]));
            g_connections[i].in_use = true;
            g_connections[i].handle = conn_handle;
            return &g_connections[i];
        }
    }

    return NULL;
}

static bool remove_connection(uint16_t conn_handle)
{
    phone_connection_t *connection = find_connection(conn_handle);
    if (connection == NULL) {
        return false;
    }

    memset(connection, 0, sizeof(*connection));
    return true;
}

static size_t connection_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < MAX_PHONE_CONNECTIONS; ++i) {
        if (g_connections[i].in_use) {
            ++count;
        }
    }
    return count;
}

static bool has_free_connection_slot(void)
{
    return connection_count() < MAX_PHONE_CONNECTIONS;
}

static tmy06_state_t get_state_snapshot(void)
{
    tmy06_state_t snapshot;
    portENTER_CRITICAL(&g_state_lock);
    snapshot = g_fridge_state;
    portEXIT_CRITICAL(&g_state_lock);
    return snapshot;
}

static void report_state_change(const tmy06_state_t *before,
                                const tmy06_state_t *after)
{
    if (g_state_change_handler != NULL &&
        memcmp(before, after, sizeof(*before)) != 0) {
        g_state_change_handler(before, after);
    }
}

static const char *fault_text(uint8_t fault_code)
{
    if (g_profile_info != NULL && !g_profile_info->has_child_lock &&
        fault_code <= 0x07U) {
        static const char *const errors[] = {
            "无故障", "Er0", "Er1", "Er2", "Er3", "Er4", "Er5", "Er6"
        };
        return errors[fault_code];
    }

    switch (fault_code) {
    case TMY06_FAULT_NONE:
        return "无故障";
    case TMY06_FAULT_NTC_SHORT:
        return "NTC短路";
    case TMY06_FAULT_NTC_OPEN:
        return "NTC开路";
    case TMY06_FAULT_HIGH_TEMPERATURE:
        return "75度高温保护";
    default:
        return "未知故障";
    }
}

static const char *mode_text(tmy06_mode_t mode)
{
    switch (mode) {
    case TMY06_MODE_COOLING:
        return "冷藏/制冷";
    case TMY06_MODE_MAX:
        return "MAX";
    case TMY06_MODE_ECO:
        return "ECO";
    case TMY06_MODE_HEATING:
        return "制热";
    default:
        return "未知";
    }
}

static void log_state_summary(const tmy06_state_t *state)
{
    if (g_profile_info->has_voltage) {
        ESP_LOGI(TAG,
                 "当前状态：%s，%s，%s，目标温度=%d，实际温度=%d，电压=%u.%02uV，故障=%s(%u)",
                 state->power == TMY06_POWER_ON ? "开机" : "关机",
                 state->unit == TMY06_UNIT_FAHRENHEIT ? "华氏度" : "摄氏度",
                 mode_text(state->mode), state->target_temperature,
                 state->actual_temperature,
                 state->actual_voltage_centi_volts / 100U,
                 state->actual_voltage_centi_volts % 100U,
                 fault_text(state->fault_code), state->fault_code);
    } else {
        ESP_LOGI(TAG,
                 "当前状态：%s，%s，%s，童锁%s，目标温度=%d，实际温度=%d，故障=%s(%u)",
                 state->power == TMY06_POWER_ON ? "开机" : "关机",
                 state->unit == TMY06_UNIT_FAHRENHEIT ? "华氏度" : "摄氏度",
                 mode_text(state->mode),
                 state->child_lock == TMY06_CHILD_LOCK_ON ? "上锁" : "解锁",
                 state->target_temperature, state->actual_temperature,
                 fault_text(state->fault_code), state->fault_code);
    }
}

static void log_app_command(tmy06_command_t command, const tmy06_state_t *state)
{
    switch (command) {
    case TMY06_COMMAND_GET_STATE:
        ESP_LOGI(TAG, "收到App命令：获取冰箱全状态");
        break;
    case TMY06_COMMAND_SET_POWER:
        ESP_LOGI(TAG,
                 "收到App命令：%s",
                 state->power == TMY06_POWER_ON ? "开机" : "关机");
        break;
    case TMY06_COMMAND_SET_MODE:
        ESP_LOGI(TAG,
                 "收到App命令：切换为%s模式",
                 mode_text(state->mode));
        break;
    case TMY06_COMMAND_SET_UNIT:
        ESP_LOGI(TAG,
                 "收到App命令：温度单位改为%s",
                 state->unit == TMY06_UNIT_FAHRENHEIT ? "华氏度" : "摄氏度");
        break;
    case TMY06_COMMAND_SET_TARGET_TEMPERATURE:
        ESP_LOGI(TAG, "收到App命令：目标温度改为%d", state->target_temperature);
        break;
    case TMY06_COMMAND_SET_CHILD_LOCK:
        ESP_LOGI(TAG,
                 "收到App命令：童锁%s",
                 state->child_lock == TMY06_CHILD_LOCK_ON ? "上锁" : "解锁");
        break;
    case TMY06_COMMAND_SET_AMBIENT_LIGHT:
        ESP_LOGI(TAG, "氛围灯已保存：灯效=%s，制冷=#%02X%02X%02X，制热=#%02X%02X%02X",
                 state->ambient.effect ? "呼吸" : "常亮",
                 state->ambient.colors[0][0], state->ambient.colors[0][1], state->ambient.colors[0][2],
                 state->ambient.colors[1][0], state->ambient.colors[1][1], state->ambient.colors[1][2]);
        break;
    default:
        ESP_LOGI(TAG, "收到App命令：0x%02X", (unsigned int)command);
        break;
    }

    log_state_summary(state);
}

static int append_status_to_mbuf(struct os_mbuf *om)
{
    uint8_t frame[FRIDGE_MAX_STATUS_FRAME_SIZE];
    tmy06_state_t snapshot = get_state_snapshot();
    size_t length = fridge_protocol_build_status_frame(g_active_profile,
                                                       &snapshot,
                                                       frame,
                                                       sizeof(frame));

    if (length == 0U || os_mbuf_append(om, frame, length) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return 0;
}

static void notify_connection(phone_connection_t *connection,
                               const uint8_t *frame,
                               size_t length)
{
    if (connection == NULL || !connection->in_use || !connection->notify_enabled) {
        return;
    }
    if (g_notify_mutex != NULL &&
        xSemaphoreTake(g_notify_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Notify queue busy: conn=%u", connection->handle);
        return;
    }

    uint16_t mtu = ble_att_mtu(connection->handle);
    size_t chunk_capacity = mtu > 3U ? (size_t)mtu - 3U : 20U;
    for (size_t offset = 0U; offset < length; offset += chunk_capacity) {
        size_t remaining = length - offset;
        size_t chunk_length = remaining < chunk_capacity
                                  ? remaining
                                  : chunk_capacity;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame + offset,
                                                   chunk_length);
        if (om == NULL) {
            ESP_LOGE(TAG, "Unable to allocate notification buffer");
            if (g_notify_mutex != NULL) {
                xSemaphoreGive(g_notify_mutex);
            }
            return;
        }

        int rc = ble_gatts_notify_custom(connection->handle,
                                         g_tx_value_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Notify failed: conn=%u rc=%d",
                     connection->handle, rc);
            if (g_notify_mutex != NULL) {
                xSemaphoreGive(g_notify_mutex);
            }
            return;
        }
    }
    if (g_notify_mutex != NULL) {
        xSemaphoreGive(g_notify_mutex);
    }
}

static void notify_wifi_provisioning(uint16_t connection_handle,
                                     const uint8_t *frame,
                                     size_t length)
{
    notify_connection(find_connection(connection_handle), frame, length);
}

static void notify_tesla_link(uint16_t connection_handle,
                              const uint8_t *frame,
                              size_t length)
{
    notify_connection(find_connection(connection_handle), frame, length);
}

static size_t notify_subscriber_count(void)
{
    size_t count = 0U;
    for (size_t i = 0U; i < MAX_PHONE_CONNECTIONS; ++i) {
        if (g_connections[i].in_use && g_connections[i].notify_enabled) {
            ++count;
        }
    }
    return count;
}

static void notify_all_subscribers(const char *reason, bool proactive)
{
    uint8_t frame[FRIDGE_MAX_STATUS_FRAME_SIZE];
    tmy06_state_t snapshot = get_state_snapshot();
    size_t length = fridge_protocol_build_status_frame(g_active_profile,
                                                       &snapshot,
                                                       frame,
                                                       sizeof(frame));
    if (length == 0U) {
        return;
    }

    ESP_LOGI(TAG,
             "时间=%lld ms，ESP32%s全状态：原因=%s，Notify订阅数=%u",
             (long long)esp_log_timestamp(),
             proactive ? "主动上报" : "响应查询上报",
             reason != NULL ? reason : "未指定",
             (unsigned int)notify_subscriber_count());
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, length, ESP_LOG_INFO);
    for (size_t i = 0; i < MAX_PHONE_CONNECTIONS; ++i) {
        notify_connection(&g_connections[i], frame, length);
    }
}

void ble_ota_report_status(tmy06_ota_status_t status, uint8_t information)
{
    uint8_t frame[TMY06_OTA_STATUS_FRAME_SIZE];
    size_t length = tmy06_protocol_build_ota_status_frame(status,
                                                          information,
                                                          frame,
                                                          sizeof(frame));
    if (length == 0U) {
        ESP_LOGE(TAG, "Invalid OTA status: status=%u information=%u",
                 (unsigned int)status, information);
        return;
    }

    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, length, ESP_LOG_INFO);
    for (size_t i = 0; i < MAX_PHONE_CONNECTIONS; ++i) {
        notify_connection(&g_connections[i], frame, length);
    }
}

void ble_ota_set_start_handler(ble_ota_start_handler_t handler)
{
    g_ota_start_handler = handler;
}

static void consume_rx_bytes(phone_connection_t *connection, size_t count)
{
    if (count >= connection->rx_length) {
        connection->rx_length = 0;
        return;
    }

    memmove(connection->rx_stream,
            connection->rx_stream + count,
            connection->rx_length - count);
    connection->rx_length -= count;
}

static void get_firmware_version(uint8_t *major,
                                 uint8_t *minor,
                                 uint8_t *patch)
{
    unsigned int parsed_major;
    unsigned int parsed_minor;
    unsigned int parsed_patch;
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if (sscanf(app_desc->version, "%u.%u.%u",
               &parsed_major, &parsed_minor, &parsed_patch) == 3 &&
        parsed_major <= UINT8_MAX && parsed_minor <= UINT8_MAX &&
        parsed_patch <= UINT8_MAX) {
        *major = (uint8_t)parsed_major;
        *minor = (uint8_t)parsed_minor;
        *patch = (uint8_t)parsed_patch;
        return;
    }

    ESP_LOGW(TAG, "无法解析应用版本 '%s'，使用 Kconfig 版本", app_desc->version);
    *major = CONFIG_TMY06_FW_VERSION_MAJOR;
    *minor = CONFIG_TMY06_FW_VERSION_MINOR;
    *patch = CONFIG_TMY06_FW_VERSION_PATCH;
}

static bool process_ota_frame(phone_connection_t *connection,
                              const uint8_t *frame,
                              size_t length)
{
    if (g_profile_info->is_legacy) {
        return false;
    }

    tmy06_ota_request_t request;
    if (!tmy06_protocol_parse_ota_request(frame, length, &request)) {
        return false;
    }

    if (request.command == TMY06_OTA_COMMAND_QUERY_VERSION) {
        uint8_t major;
        uint8_t minor;
        uint8_t patch;
        get_firmware_version(&major, &minor, &patch);

        uint8_t response[TMY06_OTA_VERSION_FRAME_SIZE];
        size_t response_length = tmy06_protocol_build_ota_version_frame(
            g_profile_info->model_id,
            major,
            minor,
            patch,
            response,
            sizeof(response));
        ESP_LOGI(TAG, "收到App命令：查询固件版本 %u.%u.%u",
                 major, minor, patch);
        notify_connection(connection, response, response_length);
        return true;
    }

    ESP_LOGI(TAG, "收到App命令：开始 OTA，目标 %u/%u.%u.%u",
             request.model_id, request.major, request.minor, request.patch);
    if (request.model_id != g_profile_info->model_id) {
        ESP_LOGE(TAG, "OTA 目标型号与当前设备不一致");
        ble_ota_report_status(TMY06_OTA_STATUS_ERROR,
                              TMY06_OTA_ERROR_VERSION);
        return true;
    }
    if (g_ota_start_handler == NULL || !g_ota_start_handler(&request)) {
        ble_ota_report_status(TMY06_OTA_STATUS_ERROR,
                              TMY06_OTA_ERROR_UNKNOWN);
    }
    return true;
}

static void process_complete_app_frame(phone_connection_t *connection,
                                       const uint8_t *frame,
                                       size_t length)
{
    tmy06_command_t command;
    tmy06_state_t previous;
    tmy06_state_t snapshot;
    bool accepted;

    if (tesla_link_handle_ble_frame(connection->handle, frame, length)) {
        return;
    }

    if (!g_profile_info->is_legacy &&
        wifi_provisioning_handle_ble_frame(connection->handle,
                                            frame, length)) {
        return;
    }

    if (process_ota_frame(connection, frame, length)) {
        return;
    }

    previous = get_state_snapshot();
    snapshot = previous;
    accepted = fridge_protocol_apply_app_frame(g_active_profile,
                                               frame,
                                               length,
                                               &snapshot,
                                               &command);
    if (accepted && command == TMY06_COMMAND_SET_AMBIENT_LIGHT &&
        memcmp(&previous.ambient, &snapshot.ambient, sizeof(snapshot.ambient)) != 0) {
        esp_err_t err = ambient_light_save(&snapshot.ambient);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "氛围灯保存失败：%s", esp_err_to_name(err));
            notify_all_subscribers("氛围灯保存失败，回传原设置", false);
            return;
        }
    }
    // Reapply against the latest refrigerator state after the NVS write.
    // Flash writes must never run inside the state critical section.
    portENTER_CRITICAL(&g_state_lock);
    previous = g_fridge_state;
    snapshot = previous;
    accepted = accepted && fridge_protocol_apply_app_frame(g_active_profile, frame, length, &snapshot, &command);
    if (accepted) g_fridge_state = snapshot;
    portEXIT_CRITICAL(&g_state_lock);

    if (!accepted) {
        ESP_LOGW(TAG, "当前档位不支持该命令，或命令参数无效");
        return;
    }

    report_state_change(&previous, &snapshot);

    log_app_command(command, &snapshot);
    if (command == TMY06_COMMAND_GET_STATE) {
        notify_all_subscribers("App获取全状态命令", false);
    } else {
        notify_all_subscribers("收到App控制命令", true);
    }
}

static void parse_rx_stream(phone_connection_t *connection)
{
    while (connection->rx_length > 0U) {
        while (connection->rx_length > 0U && connection->rx_stream[0] != 0xA5U) {
            consume_rx_bytes(connection, 1U);
        }

        if (connection->rx_length < 2U) {
            return;
        }

        if (connection->rx_stream[1] != 0x5AU) {
            consume_rx_bytes(connection, 1U);
            continue;
        }

        if (connection->rx_length < 3U) {
            return;
        }

        uint8_t data_length = connection->rx_stream[2];
        size_t frame_length = (size_t)data_length + 3U;
        if (data_length < MIN_DATA_LENGTH || frame_length > RX_STREAM_CAPACITY) {
            ESP_LOGW(TAG, "Invalid data length: %u", data_length);
            consume_rx_bytes(connection, 1U);
            continue;
        }

        if (connection->rx_length < frame_length) {
            return;
        }

        if (!tmy06_protocol_validate_frame(connection->rx_stream, frame_length)) {
            ESP_LOGW(TAG, "Checksum/tail error, resynchronizing RX stream");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,
                                     connection->rx_stream,
                                     frame_length,
                                     ESP_LOG_WARN);
            consume_rx_bytes(connection, 1U);
            continue;
        }

        if (connection->rx_stream[3] == 0xB4U) {
            uint16_t request_id = frame_length >= 8U
                                      ? (uint16_t)(((uint16_t)connection->rx_stream[4] << 8U) |
                                                   connection->rx_stream[5])
                                      : 0U;
            ESP_LOGI(TAG,
                     "收到 BLE 配网凭据: request=%u length=%u password=<redacted>",
                     request_id, (unsigned int)frame_length);
        } else {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,
                                     connection->rx_stream,
                                     frame_length,
                                     ESP_LOG_INFO);
        }
        process_complete_app_frame(connection,
                                   connection->rx_stream,
                                   frame_length);
        consume_rx_bytes(connection, frame_length);
    }
}

static int receive_app_data(uint16_t conn_handle, struct os_mbuf *om)
{
    phone_connection_t *connection = find_connection(conn_handle);
    if (connection == NULL) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t incoming_length = OS_MBUF_PKTLEN(om);
    if (incoming_length == 0U) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t incoming[RX_STREAM_CAPACITY];
    if (incoming_length > sizeof(incoming)) {
        ESP_LOGW(TAG, "Write too large: %u bytes", incoming_length);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint16_t copied = 0;
    int rc = ble_hs_mbuf_to_flat(om, incoming, sizeof(incoming), &copied);
    if (rc != 0 || copied != incoming_length) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (connection->rx_length + copied > sizeof(connection->rx_stream)) {
        ESP_LOGW(TAG, "RX stream overflow; clearing partial frame");
        connection->rx_length = 0;
    }

    memcpy(connection->rx_stream + connection->rx_length, incoming, copied);
    connection->rx_length += copied;
    parse_rx_stream(connection);
    return 0;
}

static int gatt_event_handler(uint16_t conn_handle,
                              uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt,
                              void *arg)
{
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        (attr_handle == g_rx_value_handle || attr_handle == g_tx_value_handle)) {
        return receive_app_data(conn_handle, ctxt->om);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == g_tx_value_handle) {
        return append_status_to_mbuf(ctxt->om);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == g_rx_value_handle) {
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_chr_def g_gatt_characteristics[] = {
    {
        .uuid = &g_write_uuid.u,
        .access_cb = gatt_event_handler,
        .flags = BLE_GATT_CHR_F_READ |
                 BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &g_rx_value_handle,
    },
    {
        .uuid = &g_notify_uuid.u,
        .access_cb = gatt_event_handler,
        .flags = BLE_GATT_CHR_F_READ |
                 BLE_GATT_CHR_F_WRITE |
                 BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &g_tx_value_handle,
    },
    {0},
};

static void configure_gatt_characteristics(void)
{
    if (g_profile_info->is_legacy) {
        /*
         * Legacy Y06 uses one bidirectional FFF7 characteristic.  Registering
         * separate write and notify characteristics with the same UUID makes
         * them ambiguous to phone APIs, which normally resolve a
         * characteristic by service UUID + characteristic UUID only.
         */
        g_gatt_characteristics[0].uuid = &g_notify_uuid.u;
        g_gatt_characteristics[0].flags = BLE_GATT_CHR_F_READ |
                                           BLE_GATT_CHR_F_WRITE |
                                           BLE_GATT_CHR_F_WRITE_NO_RSP |
                                           BLE_GATT_CHR_F_NOTIFY;
        g_gatt_characteristics[0].val_handle = &g_tx_value_handle;

        /* A NULL UUID terminates the characteristic definition array. */
        g_gatt_characteristics[1].uuid = NULL;
        g_gatt_characteristics[1].flags = 0;
        g_gatt_characteristics[1].val_handle = NULL;
        return;
    }

    /* Unified profiles keep independent FFF1 write and FFF2 notify values. */
    g_gatt_characteristics[0].uuid = &g_write_uuid.u;
    g_gatt_characteristics[0].flags = BLE_GATT_CHR_F_WRITE;
    g_gatt_characteristics[0].val_handle = &g_rx_value_handle;
    g_gatt_characteristics[1].uuid = &g_notify_uuid.u;
    g_gatt_characteristics[1].flags = BLE_GATT_CHR_F_NOTIFY;
    g_gatt_characteristics[1].val_handle = &g_tx_value_handle;
}

static const struct ble_gatt_svc_def g_gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_service_uuid.u,
        .characteristics = g_gatt_characteristics,
    },
    {0},
};

void start_advertising(void)
{
    if (!has_free_connection_slot()) {
        g_ble_adv_active = false;
        ESP_LOGI(TAG, "Two phones connected; advertising stopped");
        return;
    }

    if (ble_gap_adv_active()) {
        g_ble_adv_active = true;
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids16 = (ble_uuid16_t *)&g_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    if (g_profile_info->is_legacy) {
        /* Keep the historical advertising payload byte-for-byte compatible. */
        fields.name = (uint8_t *)g_device_name;
        fields.name_len = strlen(g_device_name);
        fields.name_is_complete = 1;
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    } else {
        g_service_data[5] = g_profile_info->model_id;
        fields.svc_data_uuid16 = g_service_data;
        fields.svc_data_uuid16_len = sizeof(g_service_data);
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising fields failed: rc=%d", rc);
        return;
    }

    if (!g_profile_info->is_legacy) {
        struct ble_hs_adv_fields response = {0};
        response.name = (uint8_t *)g_device_name;
        response.name_len = strlen(g_device_name);
        response.name_is_complete = 1;
        response.tx_pwr_lvl_is_present = 1;
        response.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
        rc = ble_gap_adv_rsp_set_fields(&response);
        if (rc != 0) {
            ESP_LOGE(TAG, "Scan response fields failed: rc=%d", rc);
            return;
        }
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(100),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(150),
    };

    rc = ble_gap_adv_start(g_own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &params,
                           gap_event_handler,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Advertising start failed: rc=%d", rc);
        return;
    }

    g_ble_adv_active = true;
    ESP_LOGI(TAG, "广播已启动：%s（%s）", g_device_name,
             g_profile_info->display_name);
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        g_ble_adv_active = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: status=%d", event->connect.status);
            start_advertising();
            return 0;
        }

        if (add_connection(event->connect.conn_handle) == NULL) {
            ESP_LOGW(TAG, "Connection limit reached; disconnecting conn=%u",
                     event->connect.conn_handle);
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "Connected: conn=%u count=%u",
                 event->connect.conn_handle,
                 (unsigned int)connection_count());
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        tesla_link_handle_phone_disconnect(event->disconnect.conn.conn_handle);
        wifi_provisioning_handle_ble_disconnect(
            event->disconnect.conn.conn_handle);
        remove_connection(event->disconnect.conn.conn_handle);
        ESP_LOGI(TAG, "Disconnected: conn=%u reason=%d",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        g_ble_adv_active = false;
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        phone_connection_t *connection = find_connection(event->subscribe.conn_handle);
        if (connection != NULL && event->subscribe.attr_handle == g_tx_value_handle) {
            connection->notify_enabled = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "%04X notifications %s: conn=%u",
                     g_profile_info->notify_uuid,
                     connection->notify_enabled ? "enabled" : "disabled",
                     connection->handle);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: conn=%u mtu=%u",
                 event->mtu.conn_handle,
                 event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    g_ble_adv_active = false;
    memset(g_connections, 0, sizeof(g_connections));
    ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot infer BLE address type: rc=%d", rc);
        return;
    }

    uint8_t address[6];
    rc = ble_hs_id_copy_addr(g_own_addr_type, address, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot read BLE address: rc=%d", rc);
        return;
    }

    snprintf(g_device_name,
             sizeof(g_device_name),
             "%s%02X%02X%s",
             g_profile_info->device_name_prefix,
             address[1],
             address[0],
             g_active_profile == BLE_SIM_PROFILE_NEW_Y17 ? " JUNIPER" : "");
    rc = ble_svc_gap_device_name_set(g_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Cannot set device name: rc=%d", rc);
        return;
    }

    start_advertising();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_init(ble_sim_profile_t profile)
{
    if ((unsigned int)profile >= BLE_SIM_PROFILE_COUNT) {
        profile = BLE_SIM_PROFILE_LEGACY_Y06;
    }
    g_active_profile = profile;
    g_profile_info = ble_sim_profile_info(profile);
    fridge_protocol_make_initial_state(profile, &g_fridge_state);
    if (g_profile_info->has_ambient_light) {
        esp_err_t err = ambient_light_load(&g_fridge_state.ambient);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "氛围灯设置读取失败，使用默认值：%s", esp_err_to_name(err));
    }
    wifi_provisioning_set_ble_notify_handler(notify_wifi_provisioning);
    tesla_link_set_ble_notify_handler(notify_tesla_link);
    if (g_notify_mutex == NULL) {
        g_notify_mutex = xSemaphoreCreateMutex();
        ESP_ERROR_CHECK(g_notify_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    }

    g_write_uuid.value = g_profile_info->write_uuid;
    g_notify_uuid.value = g_profile_info->notify_uuid;
    configure_gatt_characteristics();

    ESP_LOGI(TAG, "当前模拟：%s", g_profile_info->display_name);
    if (g_profile_info->is_legacy) {
        ESP_LOGI(TAG, "旧协议，无新协议 Service Data");
    } else {
        ESP_LOGI(TAG, "Service Data：54 4D 01 %02X", g_profile_info->model_id);
    }
    ESP_LOGI(TAG, "GATT：FFF0 / %04X / %04X",
             g_profile_info->write_uuid, g_profile_info->notify_uuid);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(g_gatt_services);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    rc = ble_gatts_add_svcs(g_gatt_services);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}

ble_sim_profile_t ble_sim_get_active_profile(void)
{
    return g_active_profile;
}

bool ble_tmy06_get_state(tmy06_state_t *state)
{
    if (state == NULL) {
        return false;
    }

    *state = get_state_snapshot();
    return true;
}

bool ble_tmy06_set_state(const tmy06_state_t *state)
{
    if (!fridge_protocol_validate_state(g_active_profile, state)) {
        return false;
    }

    tmy06_state_t previous;
    portENTER_CRITICAL(&g_state_lock);
    previous = g_fridge_state;
    g_fridge_state = *state;
    // Console/MQTT refrigerator edits do not overwrite light settings.
    g_fridge_state.ambient = previous.ambient;
    tmy06_state_t current = g_fridge_state;
    portEXIT_CRITICAL(&g_state_lock);
    notify_all_subscribers("冰箱完整状态改变", true);
    report_state_change(&previous, &current);
    return true;
}

bool ble_tmy06_set_actual_temperature(int16_t temperature)
{
    tmy06_state_t previous;
    tmy06_state_t current;
    portENTER_CRITICAL(&g_state_lock);
    previous = g_fridge_state;
    g_fridge_state.actual_temperature = temperature;
    current = g_fridge_state;
    portEXIT_CRITICAL(&g_state_lock);
    notify_all_subscribers("实际温度改变", true);
    report_state_change(&previous, &current);
    return true;
}

bool ble_tmy06_set_fault(uint8_t fault_code)
{
    if (fault_code > g_profile_info->maximum_fault_code) {
        return false;
    }

    tmy06_state_t previous;
    tmy06_state_t current;
    portENTER_CRITICAL(&g_state_lock);
    previous = g_fridge_state;
    g_fridge_state.fault_code = fault_code;
    current = g_fridge_state;
    portEXIT_CRITICAL(&g_state_lock);
    notify_all_subscribers("故障状态改变", true);
    report_state_change(&previous, &current);
    return true;
}

bool ble_fridge_set_actual_voltage(uint16_t centi_volts)
{
    if (!g_profile_info->has_voltage) {
        return false;
    }

    tmy06_state_t previous;
    tmy06_state_t current;
    portENTER_CRITICAL(&g_state_lock);
    previous = g_fridge_state;
    g_fridge_state.actual_voltage_centi_volts = centi_volts;
    current = g_fridge_state;
    portEXIT_CRITICAL(&g_state_lock);
    notify_all_subscribers("实际电压改变", true);
    report_state_change(&previous, &current);
    return true;
}

void ble_tmy06_set_state_change_handler(ble_state_change_handler_t handler)
{
    g_state_change_handler = handler;
}

void ble_phone_advertising_pause(void)
{
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "Unable to pause phone advertising: rc=%d", rc);
        }
    }
    g_ble_adv_active = false;
}

void ble_phone_advertising_resume(void)
{
    start_advertising();
}
