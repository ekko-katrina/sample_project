/**
 * @file tesla_ble.c
 * @brief Tesla BLE 连接实现
 * 
 * 实现与 Tesla 车辆的蓝牙 BLE 连接，包括扫描、连接、认证和命令发送
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "tesla_ble.h"
#include "tesla_protocol.h"

static const char *TAG = "TESLA_BLE";

// Tesla 服务 UUID（从车辆日志获取）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 11 02 00 00
static const ble_uuid128_t tesla_svc_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x11, 0x02, 0x00, 0x00
);

// 写特征 UUID（发送到车辆）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 12 02 00 00
static const ble_uuid128_t tesla_write_chr_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x12, 0x02, 0x00, 0x00
);

// 读特征 UUID（从车辆接收）
// 原始字节（小端序）：1e b9 f8 eb 0c 96 88 9b f0 43 d1 b2 13 02 00 00
static const ble_uuid128_t tesla_read_chr_uuid = BLE_UUID128_INIT(
    0x1e, 0xb9, 0xf8, 0xeb, 0x0c, 0x96, 0x88, 0x9b,
    0xf0, 0x43, 0xd1, 0xb2, 0x13, 0x02, 0x00, 0x00
);

// 全局连接状态
static tesla_connection_t g_conn = {0};
static tesla_vehicle_state_t g_vehicle_state = {0};
static SemaphoreHandle_t g_mutex = NULL;         // 互斥锁
static SemaphoreHandle_t g_rx_sem = NULL;        // 接收信号量
static SemaphoreHandle_t g_tx_sem = NULL;        // 写完成信号量

// 回调函数
static tesla_state_callback_t g_state_cb = NULL;
static void *g_state_cb_arg = NULL;
static tesla_vehicle_state_callback_t g_vehicle_cb = NULL;
static void *g_vehicle_cb_arg = NULL;

// 目标 VIN 用于扫描
static char g_target_vin[18] = {0};
static char g_target_local_name[20] = {0};
static ble_addr_t g_target_addr = {0};
static bool g_target_found = false;
static bool g_connect_task_running = false;

static int tesla_gap_event(struct ble_gap_event *event, void *arg);
static void tesla_connect_task(void *arg);
static void clear_pending_notifications(TickType_t mutex_timeout_ticks);

// sample_project 已有 FFF0 手机外设服务；集成模式只保留 Tesla Central。
#ifndef TESLA_BLE_EXTERNAL_HOST
// 手机连接用的外设 GATT 服务（从另一个项目合并）
#define PHONE_BLE_DEVICE_NAME      "ESP32S3-NimBLE"
#define PHONE_MAX_CONNECTIONS      2

static bool g_phone_adv_active = false;
static bool g_phone_adv_paused = false;
static uint16_t g_phone_rx_value_handle = 0;
static uint16_t g_phone_tx_value_handle = 0;
static uint16_t g_phone_conn_handles[PHONE_MAX_CONNECTIONS] = {0};
static uint8_t g_phone_conn_count = 0;

#define PHONE_UART_PACKET_SIZE     9
#define PHONE_UART_HEADER_0        0xA5
#define PHONE_UART_HEADER_1        0x5A
#define PHONE_UART_LENGTH_FIELD    0x06
#define PHONE_UART_TAIL            0xAA

// 前向声明
static void phone_ble_start_advertising(void);
static void phone_ble_stop_advertising(void);
static void phone_notify_uart_packet_all(void);

// ========== 手机 BLE 外设服务 ==========

static bool phone_has_free_connection_slot(void)
{
    return g_phone_conn_count < PHONE_MAX_CONNECTIONS;
}

static bool phone_connection_exists(uint16_t conn_handle)
{
    for (uint8_t i = 0; i < g_phone_conn_count; i++) {
        if (g_phone_conn_handles[i] == conn_handle) {
            return true;
        }
    }
    return false;
}

static bool phone_add_connection(uint16_t conn_handle)
{
    if (!phone_has_free_connection_slot() || phone_connection_exists(conn_handle)) {
        return false;
    }

    g_phone_conn_handles[g_phone_conn_count++] = conn_handle;
    return true;
}

static bool phone_remove_connection(uint16_t conn_handle)
{
    for (uint8_t i = 0; i < g_phone_conn_count; i++) {
        if (g_phone_conn_handles[i] == conn_handle) {
            g_phone_conn_count--;
            g_phone_conn_handles[i] = g_phone_conn_handles[g_phone_conn_count];
            g_phone_conn_handles[g_phone_conn_count] = 0;
            return true;
        }
    }
    return false;
}

static bool phone_closure_is_open(tesla_closure_state_t state)
{
    switch (state) {
    case TESLA_CLOSURE_OPEN:
    case TESLA_CLOSURE_AJAR:
    case TESLA_CLOSURE_OPENING:
    case TESLA_CLOSURE_FAILED_UNLATCH:
    case TESLA_CLOSURE_UNKNOWN:
        return true;
    case TESLA_CLOSURE_CLOSED:
    case TESLA_CLOSURE_CLOSING:
    default:
        return false;
    }
}

static void phone_fill_uart_packet(uint8_t packet[PHONE_UART_PACKET_SIZE])
{
    tesla_vehicle_state_t state = {0};
    bool has_state = false;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = g_vehicle_state;
        has_state = g_vehicle_state.valid;
        xSemaphoreGive(g_mutex);
    }

    uint16_t speed_x10 = 0;
    uint8_t battery = 0xFF;
    uint8_t door_flags = 0;

    if (has_state) {
        float speed = state.drive.speed_kmh;
        if (speed < 0.0f) {
            speed = 0.0f;
        }
        float scaled = speed * 10.0f;
        if (scaled > 65535.0f) {
            scaled = 65535.0f;
        }
        speed_x10 = (uint16_t)(scaled + 0.5f);

        // 当前主流程尚未接入 charge state 读取，先固定输出 0xFF 表示未知。

        if (phone_closure_is_open(state.body.closures.front_driver_door)) {
            door_flags |= BIT0;
        }
        if (phone_closure_is_open(state.body.closures.front_passenger_door)) {
            door_flags |= BIT1;
        }
        if (phone_closure_is_open(state.body.closures.rear_driver_door)) {
            door_flags |= BIT2;
        }
        if (phone_closure_is_open(state.body.closures.rear_passenger_door)) {
            door_flags |= BIT3;
        }
        if (phone_closure_is_open(state.body.closures.front_trunk)) {
            door_flags |= BIT4;
        }
        if (phone_closure_is_open(state.body.closures.rear_trunk)) {
            door_flags |= BIT5;
        }
        if (phone_closure_is_open(state.body.closures.charge_port)) {
            door_flags |= BIT6;
        }
    }

    packet[0] = PHONE_UART_HEADER_0;
    packet[1] = PHONE_UART_HEADER_1;
    packet[2] = PHONE_UART_LENGTH_FIELD;
    packet[3] = (uint8_t)(speed_x10 & 0xFF);
    packet[4] = (uint8_t)((speed_x10 >> 8) & 0xFF);
    packet[5] = battery;
    packet[6] = door_flags;

    uint8_t checksum = 0;
    for (size_t i = 0; i <= 6; i++) {
        checksum ^= packet[i];
    }
    packet[7] = checksum;
    packet[8] = PHONE_UART_TAIL;
}

static void phone_notify_uart_packet(uint16_t conn_handle)
{
    uint8_t packet[PHONE_UART_PACKET_SIZE];
    phone_fill_uart_packet(packet);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(packet, sizeof(packet));
    if (!om) {
        ESP_LOGW(TAG, "为手机连接 %d 创建 UART 通知数据失败", conn_handle);
        return;
    }

    int rc = ble_gatts_notify_custom(conn_handle, g_phone_tx_value_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "向手机连接 %d 发送 FF02 通知失败: %d", conn_handle, rc);
    }
}

static void phone_notify_uart_packet_all(void)
{
    for (uint8_t i = 0; i < g_phone_conn_count; i++) {
        phone_notify_uart_packet(g_phone_conn_handles[i]);
    }
}

static int phone_gatt_event(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle != g_phone_rx_value_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > 0) {
            uint8_t data[8] = {0};
            uint16_t copy_len = len > sizeof(data) ? sizeof(data) : len;
            int rc = os_mbuf_copydata(ctxt->om, 0, copy_len, data);
            if (rc != 0) {
                ESP_LOGW(TAG, "读取手机写入数据失败: %d", rc);
                return BLE_ATT_ERR_UNLIKELY;
            }
            ESP_LOGI(TAG, "手机连接 %d 写入 FF01，长度=%u，触发返回当前 UART 数据包",
                     conn_handle, len);
        } else {
            ESP_LOGI(TAG, "手机连接 %d 写入空 FF01，触发返回当前 UART 数据包", conn_handle);
        }

        phone_notify_uart_packet(conn_handle);
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle != g_phone_tx_value_handle) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        uint8_t packet[PHONE_UART_PACKET_SIZE];
        phone_fill_uart_packet(packet);
        int rc = os_mbuf_append(ctxt->om, packet, sizeof(packet));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int phone_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            uint16_t conn_handle = event->connect.conn_handle;

            if (!phone_add_connection(conn_handle)) {
                ESP_LOGW(TAG, "手机连接 %d 超出 %d 台上限，主动断开",
                         conn_handle, PHONE_MAX_CONNECTIONS);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }

            g_phone_adv_active = false;
            ESP_LOGI(TAG, "手机已连接，conn_handle=%d，当前手机连接数=%d",
                     conn_handle, g_phone_conn_count);

            if (phone_has_free_connection_slot()) {
                phone_ble_start_advertising();
            }
        } else {
            g_phone_adv_active = false;
            ESP_LOGW(TAG, "手机连接失败: %d", event->connect.status);
            if (phone_has_free_connection_slot()) {
                phone_ble_start_advertising();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        phone_remove_connection(event->disconnect.conn.conn_handle);
        g_phone_adv_active = false;
        ESP_LOGI(TAG, "手机已断开，conn_handle=%d，当前手机连接数=%d",
                 event->disconnect.conn.conn_handle, g_phone_conn_count);

        if (phone_has_free_connection_slot()) {
            phone_ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        g_phone_adv_active = false;
        if (g_phone_adv_paused) {
            return 0;
        }
        if (phone_has_free_connection_slot()) {
            phone_ble_start_advertising();
        }
        return 0;

    default:
        return 0;
    }
}

static const struct ble_gatt_svc_def phone_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x00FF),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xFF01),
                .access_cb = phone_gatt_event,
                .flags = BLE_GATT_CHR_F_WRITE,
                .val_handle = &g_phone_rx_value_handle,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0xFF02),
                .access_cb = phone_gatt_event,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_phone_tx_value_handle,
            },
            {0}
        },
    },
    {0}
};

static esp_err_t phone_ble_gatt_init(void)
{
    int rc = ble_gatts_count_cfg(phone_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "手机 GATT 服务计数失败: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(phone_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "手机 GATT 服务注册失败: %d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void phone_ble_stop_advertising(void)
{
    if (!g_phone_adv_active) {
        return;
    }

    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "停止手机广播失败: %d", rc);
    }
    g_phone_adv_active = false;
    g_phone_adv_paused = true;
}

static void phone_ble_start_advertising(void)
{
    g_phone_adv_paused = false;

    if (!phone_has_free_connection_slot()) {
        g_phone_adv_active = false;
        ESP_LOGI(TAG, "手机连接已达到 %d 台上限，停止广播", PHONE_MAX_CONNECTIONS);
        return;
    }

    if (g_phone_adv_active) {
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)PHONE_BLE_DEVICE_NAME;
    fields.name_len = strlen(PHONE_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "手机广播内容设置失败: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(200),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(500),
    };

    uint8_t own_addr_type;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "手机广播地址类型获取失败: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, phone_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "手机广播启动失败: %d", rc);
        g_phone_adv_active = false;
        return;
    }

    g_phone_adv_active = true;
    ESP_LOGI(TAG, "手机 BLE 广播已启动，设备名: %s", PHONE_BLE_DEVICE_NAME);
}
#else
static bool g_phone_adv_active = false;
static void phone_ble_start_advertising(void) {}
static void phone_ble_stop_advertising(void) {}
static void phone_notify_uart_packet_all(void) {}
#endif

static void clear_pending_notifications(TickType_t mutex_timeout_ticks)
{
    while (xSemaphoreTake(g_rx_sem, 0) == pdTRUE) {}

    if (xSemaphoreTake(g_mutex, mutex_timeout_ticks) == pdTRUE) {
        g_conn.rx_len = 0;
        g_conn.rx_complete = false;
        xSemaphoreGive(g_mutex);
    }
}

// ========== 工具函数 ==========

/**
 * @brief 通知状态变化
 * @param state 新的连接状态
 */
static void notify_state_change(tesla_conn_state_t state)
{
    g_conn.state = state;
    if (g_state_cb) {
        g_state_cb(state, g_state_cb_arg);
    }
}

/**
 * @brief 生成随机路由地址
 * @param addr 16 字节的地址缓冲区
 */
static void generate_routing_address(uint8_t *addr)
{
    esp_fill_random(addr, 16);
}

// ========== BLE 扫描 ==========

/**
 * @brief 检查 BLE 广播数据是否为目标 Tesla 车辆
 * @param data 广播数据
 * @param len 数据长度
 * @param target_name 目标设备名称
 * @return true 如果找到目标设备
 */
static bool check_tesla_adv(const uint8_t *data, uint8_t len, const char *target_name)
{
    uint8_t offset = 0;
    while (offset < len) {
        uint8_t field_len = data[offset];
        if (field_len == 0 || offset + field_len >= len) break;

        uint8_t field_type = data[offset + 1];

        // 检查完整本地名称（0x09）或缩短本地名称（0x08）
        if (field_type == 0x09 || field_type == 0x08) {
            char name[32] = {0};
            uint8_t name_len = field_len - 1;
            if (name_len > sizeof(name) - 1) name_len = sizeof(name) - 1;
            memcpy(name, &data[offset + 2], name_len);

            // ESP_LOGI(TAG, "Found device: %s", name);

            if (strcmp(name, target_name) == 0) {
                return true;
            }
        }

        offset += field_len + 1;
    }
    return false;
}

/**
 * @brief 启动 BLE 扫描
 */
static esp_err_t tesla_scan(void)
{
    uint8_t own_addr_type;
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return ESP_ERR_INVALID_STATE;
    }

    disc_params.filter_duplicates = 0;  // 不过滤重复设备
    disc_params.passive = 0;             // 主动扫描
    // Units are 0.625 ms: scan for 40 ms every 160 ms, leaving airtime for
    // phone advertising/connections and Wi-Fi. Active scanning reads scan responses.
    disc_params.itvl = 256;
    disc_params.window = 64;
    disc_params.filter_policy = 0;       // 不使用白名单
    disc_params.limited = 0;             // 一般发现模式

    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, tesla_gap_event, NULL);
    if (rc != 0 && g_phone_adv_active) {
        ESP_LOGW(TAG, "扫描车辆失败: %d，临时停止手机广播后重试", rc);
        phone_ble_stop_advertising();
        rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc_params, tesla_gap_event, NULL);
    }

    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        return ESP_FAIL;
    } else {
        ESP_LOGI(TAG, "Scanning for Tesla vehicle: %s", g_target_local_name);
        notify_state_change(TESLA_STATE_SCANNING);
    }
    return ESP_OK;
}

static void tesla_start_deferred_connect(void)
{
    if (g_connect_task_running) {
        return;
    }

    g_connect_task_running = true;
    BaseType_t ok = xTaskCreate(tesla_connect_task, "tesla_connect", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        g_connect_task_running = false;
        ESP_LOGE(TAG, "Failed to create connect task");
        notify_state_change(TESLA_STATE_DISCONNECTED);
    }
}

static void tesla_connect_task(void *arg)
{
    (void)arg;

    int rc = 0;
    uint8_t own_addr_type = 0;

    vTaskDelay(pdMS_TO_TICKS(100));

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type before connect: %d", rc);
        notify_state_change(TESLA_STATE_DISCONNECTED);
        g_connect_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    for (int attempt = 1; attempt <= 5; attempt++) {
        if (ble_gap_disc_active()) {
            ble_gap_disc_cancel();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        rc = ble_gap_connect(own_addr_type, &g_target_addr, 30000, NULL,
                             tesla_gap_event, NULL);
        if (rc != 0 && g_phone_adv_active) {
            ESP_LOGW(TAG, "Connect start failed while advertising: %d, stop phone adv and retry", rc);
            phone_ble_stop_advertising();
            vTaskDelay(pdMS_TO_TICKS(100));
            rc = ble_gap_connect(own_addr_type, &g_target_addr, 30000, NULL,
                                 tesla_gap_event, NULL);
        }

        if (rc == 0) {
            ESP_LOGI(TAG, "Connect started");
            notify_state_change(TESLA_STATE_CONNECTING);
            g_connect_task_running = false;
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGW(TAG, "Connect start failed (attempt %d/5): %d", attempt, rc);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGE(TAG, "Failed to start connect after retries: %d", rc);
    notify_state_change(TESLA_STATE_DISCONNECTED);
    g_connect_task_running = false;
    vTaskDelete(NULL);
}

// ========== GATT 客户端 ==========

/**
 * @brief GATT 写操作回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param attr 属性信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_write(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Write complete, handle=%d", attr->handle);
    } else {
        ESP_LOGE(TAG, "Write failed, status=%d", error->status);
    }
    if (g_tx_sem) {
        xSemaphoreGive(g_tx_sem);
    }
    return 0;
}

/**
 * @brief MTU 交换完成回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param mtu 协商后的 MTU 值
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_mtu_exchange(uint16_t conn_handle, const struct ble_gatt_error *error,
                                  uint16_t mtu, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU exchange complete, MTU=%d", mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed: %d, using default MTU", error->status);
    }
    // 无论 MTU 交换是否成功，都通知服务发现完成
    notify_state_change(TESLA_STATE_SERVICE_DISCOVERED);
    return 0;
}

/**
 * @brief 订阅通知回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param attr 属性信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_subscribe(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    if (error->status == 0) {
        ESP_LOGI(TAG, "Subscribed to notifications");
        // 订阅成功后，发起 MTU 交换（与 Go SDK 一致）
        int rc = ble_gattc_exchange_mtu(conn_handle, tesla_on_mtu_exchange, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "Failed to initiate MTU exchange: %d", rc);
            // 即使 MTU 交换失败，也继续
            notify_state_change(TESLA_STATE_SERVICE_DISCOVERED);
        }
    } else {
        ESP_LOGE(TAG, "Subscribe failed: %d", error->status);
    }
    return 0;
}

/**
 * @brief 订阅通知
 * @param conn_handle 连接句柄
 * @param cccd_handle CCCD 句柄
 */
static void tesla_subscribe_notifications(uint16_t conn_handle, uint16_t cccd_handle)
{
    uint8_t value[2] = {0x01, 0x00};  // 启用通知
    int rc = ble_gattc_write_flat(conn_handle, cccd_handle, value, sizeof(value),
                                   tesla_on_subscribe, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to subscribe: %d", rc);
    }
}

// 简单的对等设备/服务跟踪
static uint16_t g_svc_start_handle = 0;
static uint16_t g_svc_end_handle = 0;

/**
 * @brief 特征发现回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param chr 特征信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL) {
        // 打印 UUID 用于调试
        if (chr->uuid.u.type == BLE_UUID_TYPE_128) {
            const uint8_t *u = chr->uuid.u128.value;
            ESP_LOGI(TAG, "Found chr: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, def=%d, val=%d",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                     chr->def_handle, chr->val_handle);
        } else if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "Found 16-bit chr: 0x%04x, def=%d, val=%d",
                     chr->uuid.u16.value, chr->def_handle, chr->val_handle);
        }

        // 检查是否为写特征
        if (ble_uuid_cmp(&chr->uuid.u, &tesla_write_chr_uuid.u) == 0) {
            g_conn.write_handle = chr->val_handle;
            ESP_LOGI(TAG, "Write characteristic MATCHED: %d", g_conn.write_handle);
        }
        // 检查是否为读特征
        else if (ble_uuid_cmp(&chr->uuid.u, &tesla_read_chr_uuid.u) == 0) {
            g_conn.read_handle = chr->val_handle;
            ESP_LOGI(TAG, "Read characteristic MATCHED: %d", g_conn.read_handle);
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Characteristic discovery complete");

        // 现在发现描述符以查找 CCCD
        if (g_conn.read_handle != 0) {
            // 订阅通知 - 首先查找 CCCD
            // 为简单起见，假设 CCCD 在 read_handle + 1
            tesla_subscribe_notifications(conn_handle, g_conn.read_handle + 1);
        }
    }
    return 0;
}

/**
 * @brief 服务发现回调
 * @param conn_handle 连接句柄
 * @param error 错误信息
 * @param svc 服务信息
 * @param arg 用户参数
 * @return 0
 */
static int tesla_on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc != NULL) {
        // 打印 UUID 用于调试
        if (svc->uuid.u.type == BLE_UUID_TYPE_128) {
            const uint8_t *u = svc->uuid.u128.value;
            ESP_LOGI(TAG, "Found 128-bit service: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x, start=%d, end=%d",
                     u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                     u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                     svc->start_handle, svc->end_handle);

            const uint8_t *e = tesla_svc_uuid.value;
            ESP_LOGI(TAG, "Expected Tesla svc: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7],
                     e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15]);
        } else if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
            ESP_LOGI(TAG, "Found 16-bit service: 0x%04x, start=%d, end=%d",
                     svc->uuid.u16.value, svc->start_handle, svc->end_handle);
        }

        if (ble_uuid_cmp(&svc->uuid.u, &tesla_svc_uuid.u) == 0) {
            ESP_LOGI(TAG, "Tesla service MATCHED!");
            g_svc_start_handle = svc->start_handle;
            g_svc_end_handle = svc->end_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "Service discovery complete");

        if (g_svc_start_handle != 0) {
            // 发现特征
            int rc = ble_gattc_disc_all_chrs(conn_handle, g_svc_start_handle,
                                              g_svc_end_handle, tesla_on_chr_disc, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover characteristics: %d", rc);
            }
        } else {
            ESP_LOGE(TAG, "Tesla service not found!");
        }
    }
    return 0;
}

// ========== GAP 事件处理器 ==========

/**
 * @brief GAP 事件处理器
 * @param event GAP 事件
 * @param arg 用户参数
 * @return 0
 */
static int tesla_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        // 检查是否为目标 Tesla
        if (check_tesla_adv(event->disc.data, event->disc.length_data, g_target_local_name)) {
            ESP_LOGI(TAG, "Found target Tesla! RSSI=%d", event->disc.rssi);
            g_target_addr = event->disc.addr;
            g_target_found = true;

            // 停止扫描
            ble_gap_disc_cancel();
            tesla_start_deferred_connect();
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Connected!");
            g_conn.conn_handle = event->connect.conn_handle;
            g_conn.mtu = 23;  // 交换前的默认 BLE MTU

            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "Peer addr: %02x:%02x:%02x:%02x:%02x:%02x",
                         desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                         desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                         desc.peer_id_addr.val[1], desc.peer_id_addr.val[0]);
            }

            // 请求更快的连接参数以获得更好的吞吐量
            // min_interval=6 (7.5ms), max_interval=12 (15ms), latency=0, timeout=200 (2s)
            struct ble_gap_upd_params conn_params = {
                .itvl_min = 6,
                .itvl_max = 12,
                .latency = 0,
                .supervision_timeout = 200,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            rc = ble_gap_update_params(event->connect.conn_handle, &conn_params);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to request conn param update: %d", rc);
            }

            notify_state_change(TESLA_STATE_CONNECTED);

            // 启动服务发现
            rc = ble_gattc_disc_all_svcs(event->connect.conn_handle,
                                          tesla_on_svc_disc, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to discover services: %d", rc);
            }
            phone_ble_start_advertising();
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            notify_state_change(TESLA_STATE_DISCONNECTED);
            // The integrated Tesla Link task owns retries and their backoff.
#ifndef TESLA_BLE_EXTERNAL_HOST
            tesla_scan();
#endif
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected, reason=%d", event->disconnect.reason);
        g_conn.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_conn.write_handle = 0;
        g_conn.read_handle = 0;
        notify_state_change(TESLA_STATE_DISCONNECTED);
        phone_ble_start_advertising();
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!g_connect_task_running) {
            phone_ble_start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            ESP_LOGD(TAG, "Notification received, handle=%d, len=%d", event->notify_rx.attr_handle, len);

            // 累积数据到接收缓冲区（多个通知可能形成一条消息）
            BaseType_t got_mutex = xSemaphoreTake(g_mutex, pdMS_TO_TICKS(1000));
            if (got_mutex == pdTRUE) {
                if (g_conn.rx_len + len <= sizeof(g_conn.rx_buffer)) {
                    os_mbuf_copydata(event->notify_rx.om, 0, len, g_conn.rx_buffer + g_conn.rx_len);
                    g_conn.rx_len += len;
                } else {
                    ESP_LOGW(TAG, "RX buffer full!");
                }
                xSemaphoreGive(g_mutex);

                if (g_rx_sem) {
                    xSemaphoreGive(g_rx_sem);
                }
            } else {
                ESP_LOGE(TAG, "Failed to get mutex in notification handler!");
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        g_conn.mtu = event->mtu.value;
        return 0;

    default:
        return 0;
    }
}

// ========== NimBLE 主机任务 ==========

/**
 * @brief BLE 主机重置回调
 * @param reason 重置原因
 */
static void tesla_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

/**
 * @brief BLE 主机同步回调
 */
static void tesla_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
    }
    ESP_LOGI(TAG, "BLE host synced");
    phone_ble_start_advertising();
}

/**
 * @brief BLE 主机任务
 * @param param 任务参数
 */
static void tesla_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ========== 公共 API ==========

esp_err_t tesla_ble_init(void)
{
    esp_err_t ret;

    // 创建信号量和互斥锁
    g_mutex = xSemaphoreCreateMutex();
    g_rx_sem = xSemaphoreCreateBinary();
    g_tx_sem = xSemaphoreCreateBinary();
    if (!g_mutex || !g_rx_sem || !g_tx_sem) {
        return ESP_ERR_NO_MEM;
    }

#ifndef TESLA_BLE_EXTERNAL_HOST
    // 独立工程负责初始化 NimBLE；集成工程复用 MYBLE 的 Host。
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %d", ret);
        return ret;
    }

    // 设置回调函数
    g_conn.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_hs_cfg.reset_cb = tesla_on_reset;
    ble_hs_cfg.sync_cb = tesla_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    // 初始化 GAP 服务（仅用于外设角色）
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(PHONE_BLE_DEVICE_NAME);
#endif

    ret = phone_ble_gatt_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // 启动 NimBLE 主机任务
    nimble_port_freertos_init(tesla_host_task);
#else
    g_conn.conn_handle = BLE_HS_CONN_HANDLE_NONE;
#endif

    // 生成路由地址
    generate_routing_address(g_conn.routing_address);

    ESP_LOGI(TAG, "Tesla BLE initialized");
    return ESP_OK;
}

/**
 * @brief 反初始化 Tesla BLE 模块
 */
void tesla_ble_deinit(void)
{
#ifndef TESLA_BLE_EXTERNAL_HOST
    nimble_port_stop();
    nimble_port_deinit();
#else
    tesla_ble_scan_stop();
    tesla_ble_disconnect();
#endif

    if (g_mutex) {
        vSemaphoreDelete(g_mutex);
        g_mutex = NULL;
    }
    if (g_rx_sem) {
        vSemaphoreDelete(g_rx_sem);
        g_rx_sem = NULL;
    }
    if (g_tx_sem) {
        vSemaphoreDelete(g_tx_sem);
        g_tx_sem = NULL;
    }
}

/**
 * @brief 设置状态变化回调函数
 * @param callback 回调函数指针
 * @param arg 用户参数
 */
void tesla_ble_set_state_callback(tesla_state_callback_t callback, void *arg)
{
    g_state_cb = callback;
    g_state_cb_arg = arg;
}

/**
 * @brief 设置车辆状态回调函数
 * @param callback 回调函数指针
 * @param arg 用户参数
 */
void tesla_ble_set_vehicle_state_callback(tesla_vehicle_state_callback_t callback, void *arg)
{
    g_vehicle_cb = callback;
    g_vehicle_cb_arg = arg;
}

/**
 * @brief 加载或生成密钥对
 * @param key_data 私钥数据（32 字节），传入 NULL 则生成新密钥
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_load_key(const uint8_t *key_data)
{
    if (key_data) {
        // 加载现有私钥
        memcpy(g_conn.private_key, key_data, 32);
        // 从私钥派生公钥
        if (!tesla_crypto_derive_public_key(g_conn.private_key, g_conn.public_key)) {
            ESP_LOGE(TAG, "Failed to derive public key from private key");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Loaded private key and derived public key");
    } else {
        // 生成新的密钥对
        if (!tesla_crypto_generate_keypair(g_conn.private_key, g_conn.public_key)) {
            ESP_LOGE(TAG, "Failed to generate key pair");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Generated new key pair");
    }

    return ESP_OK;
}

/**
 * @brief 获取公钥
 * @param key_data 公钥缓冲区（65 字节）
 * @return ESP_OK 成功
 */
esp_err_t tesla_ble_get_public_key(uint8_t *key_data)
{
    memcpy(key_data, g_conn.public_key, 65);
    return ESP_OK;
}

/**
 * @brief 获取私钥（用于保存到 NVS）
 * @param key_data 私钥缓冲区（32 字节）
 * @return ESP_OK 成功
 */
esp_err_t tesla_ble_get_private_key(uint8_t *key_data)
{
    memcpy(key_data, g_conn.private_key, 32);
    return ESP_OK;
}

/**
 * @brief 从 VIN 计算车辆的 BLE 本地名称
 * @param vin 车辆识别码（17 个字符）
 * @param local_name 输出本地名称的缓冲区（至少 20 字节）
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t tesla_ble_get_local_name(const char *vin, char *local_name)
{
    if (!vin || strlen(vin) != 17 || !local_name) {
        return ESP_ERR_INVALID_ARG;
    }

    // 计算 VIN 的 SHA1 并取前 8 字节作为十六进制
    uint8_t hash[20];
    tesla_crypto_sha1((const uint8_t *)vin, 17, hash);

    // 格式：S + SHA1 前 8 字节的十六进制 + C
    sprintf(local_name, "S%02x%02x%02x%02x%02x%02x%02x%02xC",
            hash[0], hash[1], hash[2], hash[3],
            hash[4], hash[5], hash[6], hash[7]);

    return ESP_OK;
}

/**
 * @brief 开始扫描 Tesla 车辆
 * @param vin 目标车辆的 VIN 码
 * @return ESP_OK 成功，ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t tesla_ble_scan(const char *vin)
{
    if (!vin || strlen(vin) != 17) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(g_target_vin, vin, sizeof(g_target_vin) - 1);
    strncpy(g_conn.vin, vin, sizeof(g_conn.vin) - 1);

    tesla_ble_get_local_name(vin, g_target_local_name);
    ESP_LOGI(TAG, "Target local name: %s", g_target_local_name);

    g_target_found = false;
    return tesla_scan();
}

/**
 * @brief 停止扫描
 */
void tesla_ble_scan_stop(void)
{
    ble_gap_disc_cancel();
    phone_ble_start_advertising();
}

/**
 * @brief 连接到已发现的车辆
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 状态错误
 */
esp_err_t tesla_ble_connect(void)
{
    if (!g_target_found) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);

    int rc = ble_gap_connect(own_addr_type, &g_target_addr, 30000, NULL,
                             tesla_gap_event, NULL);
    if (rc != 0 && g_phone_adv_active) {
        ESP_LOGW(TAG, "Connect failed while advertising: %d, stop phone adv and retry", rc);
        phone_ble_stop_advertising();
        vTaskDelay(pdMS_TO_TICKS(100));
        rc = ble_gap_connect(own_addr_type, &g_target_addr, 30000, NULL,
                             tesla_gap_event, NULL);
    }

    if (rc != 0) {
        ESP_LOGE(TAG, "Connect failed: %d", rc);
        return ESP_FAIL;
    }

    notify_state_change(TESLA_STATE_CONNECTING);
    return ESP_OK;
}

/**
 * @brief 断开与车辆的连接
 */
void tesla_ble_disconnect(void)
{
    if (g_conn.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(g_conn.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

/**
 * @brief 获取当前连接状态
 * @return 当前连接状态
 */
tesla_conn_state_t tesla_ble_get_state(void)
{
    return g_conn.state;
}

/**
 * @brief 获取当前车辆状态
 * @param state 输出车辆状态结构体
 * @return ESP_OK 状态有效，ESP_ERR_INVALID_STATE 状态无效，ESP_ERR_TIMEOUT 超时
 */
esp_err_t tesla_ble_get_vehicle_state(tesla_vehicle_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(state, &g_vehicle_state, sizeof(tesla_vehicle_state_t));
        xSemaphoreGive(g_mutex);
        return g_vehicle_state.valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_TIMEOUT;
}

// ========== 会话管理 ==========

/**
 * @brief 等待响应
 * @param timeout_ms 超时时间（毫秒）
 * @return true 如果接收到完整响应
 * 
 * 注意：调用者应在发送请求前清除接收缓冲区
 * 不要在此处清除接收缓冲区，因为响应可能已经到达
 */
static bool wait_for_response(uint32_t timeout_ms)
{
    ESP_LOGD(TAG, "Waiting for response (timeout=%lu ms)...", (unsigned long)timeout_ms);

    // 等待完整消息（可能分多个通知到达）
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout) {
        // 首先检查是否已有完整消息
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_conn.rx_len >= 2) {
                uint16_t msg_len = (g_conn.rx_buffer[0] << 8) | g_conn.rx_buffer[1];
                if (msg_len > 0 && g_conn.rx_len >= 2 + msg_len) {
                    // 移除长度头，移动数据
                    g_conn.rx_len = msg_len;
                    memmove(g_conn.rx_buffer, g_conn.rx_buffer + 2, msg_len);
                    g_conn.rx_complete = true;
                    xSemaphoreGive(g_mutex);
                    ESP_LOGD(TAG, "Complete message received: %d bytes", msg_len);
                    return true;
                }
            }
            xSemaphoreGive(g_mutex);
        }

        // 等待新数据，使用较短超时以快速反应
        xSemaphoreTake(g_rx_sem, pdMS_TO_TICKS(20));
    }

    // 超时时打印最终缓冲区状态
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ESP_LOGW(TAG, "Timeout! Final rx_len=%d", g_conn.rx_len);
        xSemaphoreGive(g_mutex);
    }
    return false;
}

/**
 * @brief 发送消息
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return ESP_OK 成功
 */
static esp_err_t send_message(const uint8_t *data, uint16_t len)
{
    if (g_conn.write_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // 构建带 2 字节长度头的消息（大端序）
    uint8_t msg[1024];
    if (len + 2 > sizeof(msg)) {
        return ESP_ERR_NO_MEM;
    }
    msg[0] = (len >> 8) & 0xFF;
    msg[1] = len & 0xFF;
    memcpy(msg + 2, data, len);

    uint16_t total_len = len + 2;
    uint16_t offset = 0;
    // 使用协商的 MTU，如果未设置则使用默认值 20（最小 BLE MTU）
    // 减去 3 以考虑 ATT 头开销
    uint16_t mtu = (g_conn.mtu > 3) ? (g_conn.mtu - 3) : 20;

    ESP_LOGD(TAG, "Sending message: payload=%d, total=%d, mtu=%d", len, total_len, mtu);
    ESP_LOGD(TAG, "原始消息 (%d 字节):", len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, len, ESP_LOG_DEBUG);
    ESP_LOGD(TAG, "带长度前缀 (%d 字节):", total_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg, total_len, ESP_LOG_DEBUG);

    // 如需分块发送
    // 使用无响应写入（写命令）以获得更快的吞吐量
    // 这样可以避免等待车辆对每个块的确认
    while (offset < total_len) {
        uint16_t chunk_len = (total_len - offset > mtu) ? mtu : (total_len - offset);

        int rc = ble_gattc_write_no_rsp_flat(g_conn.conn_handle, g_conn.write_handle,
                                              msg + offset, chunk_len);
        if (rc != 0) {
            ESP_LOGE(TAG, "Write failed: %d", rc);
            return ESP_FAIL;
        }

        offset += chunk_len;
    }

    ESP_LOGI(TAG, "Message sent successfully");
    return ESP_OK;
}

/**
 * @brief 与车辆建立加密会话（密钥交换）
 * @param domain 目标域（VCSEC 或 Infotainment）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t tesla_ble_start_session(tesla_domain_t domain)
{
    uint8_t buf[256];
    uint32_t len;

    int idx = (domain == TESLA_DOMAIN_INFOTAINMENT) ? 1 : 0;
    tesla_session_t *session = &g_conn.session[idx];

    // 在发送会话请求前清除所有待处理的通知
    ESP_LOGI(TAG, "Starting session for domain %d...", domain);
    clear_pending_notifications(pdMS_TO_TICKS(100));

    // 构建会话信息请求
    len = tesla_build_session_info_request(buf, sizeof(buf), domain,
                                            g_conn.public_key,
                                            g_conn.routing_address);
    if (len == 0) {
        return ESP_FAIL;
    }

    // 发送请求
    esp_err_t ret = send_message(buf, len);
    if (ret != ESP_OK) {
        return ret;
    }

    // 等待响应，使用重试逻辑跳过 VCSEC 广播
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(10000);

    while ((xTaskGetTickCount() - start) < timeout) {
        // 等待消息
        if (!wait_for_response(2000)) {
            ESP_LOGW(TAG, "No response yet, continuing to wait...");
            continue;
        }

        // 检查这是否是 VCSEC 广播（字段 7 from_destination = 域 2）
        // 而不是会话信息响应
        if (g_conn.rx_len >= 8) {
            const uint8_t *p = g_conn.rx_buffer;
            // 检查字段 6 (to_destination): 32 02 08 XX
            // 检查字段 7 (from_destination): 3a 02 08 XX
            if (p[0] == 0x32 && p[4] == 0x3a && p[6] == 0x08) {
                uint8_t from_domain = p[7];
                ESP_LOGI(TAG, "Received message from domain %d", from_domain);

                // 检查这是否是会话信息响应（有字段 15，或字段 7a = 0x7a）
                // 会话信息响应将有 session_info 字段（15 = protobuf 中的 0x7a）
                bool has_session_info = false;
                for (int i = 0; i < (int)g_conn.rx_len - 1; i++) {
                    if (g_conn.rx_buffer[i] == 0x7a) {  // 字段 15, wire type 2 = (15 << 3) | 2 = 122 = 0x7a
                        has_session_info = true;
                        break;
                    }
                }

                if (!has_session_info) {
                    ESP_LOGW(TAG, "Received VCSEC broadcast (no session_info), skipping...");
                    // 清除缓冲区并等待下一条消息
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        g_conn.rx_len = 0;
                        g_conn.rx_complete = false;
                        xSemaphoreGive(g_mutex);
                    }
                    continue;
                }
            }
        }

        // 尝试解析会话信息
        if (tesla_parse_session_info(g_conn.rx_buffer, g_conn.rx_len, session)) {
            // 成功！计算 ECDH 共享密钥
            if (!tesla_crypto_ecdh(g_conn.private_key, session->vehicle_public_key,
                                   session->shared_secret)) {
                ESP_LOGE(TAG, "ECDH failed");
                return ESP_FAIL;
            }

            // 派生会话密钥
            tesla_crypto_derive_key(session->shared_secret, session->session_key);

            // 调试：会话信息仅 DEBUG 级别输出，避免泄露密钥
            ESP_LOGD(TAG, "=== Session Debug Info ===");
            ESP_LOGD(TAG, "  Shared secret:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->shared_secret, 32, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Session key (16 bytes):");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->session_key, 16, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Vehicle public key:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->vehicle_public_key, 65, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Epoch:");
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, session->epoch, 16, ESP_LOG_DEBUG);
            ESP_LOGD(TAG, "  Counter=%lu, Clock=%lu",
                     (unsigned long)session->counter, (unsigned long)session->clock_time);

            // 记录会话建立时的本地时间（用于 expires_at 计算）
            session->local_time_sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);

            session->established = true;
            ESP_LOGI(TAG, "Session established for domain %d", domain);

            if (domain == TESLA_DOMAIN_VEHICLE_SECURITY) {
                notify_state_change(TESLA_STATE_AUTHENTICATED);
            }

            return ESP_OK;
        }

        ESP_LOGW(TAG, "Failed to parse session info, waiting for correct response...");
        // 清除缓冲区并等待下一条消息
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_conn.rx_len = 0;
            g_conn.rx_complete = false;
            xSemaphoreGive(g_mutex);
        }
    }

    ESP_LOGE(TAG, "Session info request timeout after retries");
    return ESP_ERR_TIMEOUT;
}

// ========== 车辆命令 ==========

/**
 * @brief 发送添加密钥请求（配对）
 * @return ESP_OK 成功，其他值失败
 * 
 * 向车辆发送配对请求，需要用户在车辆屏幕上确认并使用 NFC 卡验证
 */
esp_err_t tesla_ble_add_key_request(void)
{
    uint8_t buf[512];  // 增加缓冲区大小以容纳包装的消息
    uint32_t len;

    // 清除车辆的待处理通知（重要！）
    // 订阅后车辆可能发送非请求消息
    ESP_LOGI(TAG, "Clearing pending notifications before sending add key request...");
    clear_pending_notifications(pdMS_TO_TICKS(100));
    // 等待一段时间让正在传输的通知到达并被丢弃
    vTaskDelay(pdMS_TO_TICKS(200));
    clear_pending_notifications(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Building add key request...");
    ESP_LOGD(TAG, "Public key (65 bytes):");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, g_conn.public_key, 65, ESP_LOG_DEBUG);

    // 验证公钥格式（未压缩格式应以 0x04 开头）
    if (g_conn.public_key[0] != 0x04) {
        ESP_LOGE(TAG, "Invalid public key format! First byte should be 0x04, got 0x%02x",
                 g_conn.public_key[0]);
        return ESP_FAIL;
    }

    len = tesla_build_add_key_request(buf, sizeof(buf), g_conn.public_key,
                                       TESLA_KEY_CLOUD_KEY);
    if (len == 0) {
        ESP_LOGE(TAG, "Failed to build add key request");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Add key request built, length=%lu", (unsigned long)len);
    ESP_LOGD(TAG, "Full message hex:");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, len, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Sending add key request to vehicle...");

    esp_err_t ret = send_message(buf, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send add key request: %d", ret);
        return ret;
    }

    // 与 Go SDK 一样，我们不在此处等待响应。
    // 添加密钥请求会触发车辆屏幕上的用户确认。
    // 成功通过稍后尝试启动会话来验证。
    ESP_LOGI(TAG, "Add key request sent successfully");
    ESP_LOGI(TAG, "Note: Pairing requires user confirmation on vehicle screen with NFC card");

    return ESP_OK;
}

/**
 * @brief 获取车身控制器状态（VCSEC）
 * @return ESP_OK 成功，ESP_ERR_TIMEOUT 超时，ESP_FAIL 失败
 * 
 * 获取车辆锁定状态、车门状态等信息
 */
esp_err_t tesla_ble_get_body_state(void)
{
    uint8_t payload[64];
    uint8_t buf[256];
    uint32_t payload_len, len;

    // 注意：对于 VCSEC InformationRequest，我们不需要已建立的会话
    // Go SDK 对此请求使用未认证方法（AuthMethodNone）

    // 车可能持续推送异步 VCSEC 通知；发送前先清掉旧包，避免 wait_for_response
    // 从错误的帧边界开始累计，最终出现 rx_len 很大但始终拼不出完整消息。
    clear_pending_notifications(pdMS_TO_TICKS(100));

    // 构建信息请求（UnsignedMessage 与 InformationRequest）
    payload_len = tesla_build_info_request(payload, sizeof(payload),
                                            TESLA_INFO_GET_STATUS);

    ESP_LOGD(TAG, "Info request payload (%lu bytes):", (unsigned long)payload_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, payload_len, ESP_LOG_DEBUG);

    // 构建未认证的可路由消息（类似 Go SDK 使用 AuthMethodNone）
    len = tesla_build_vcsec_message_unauthenticated(buf, sizeof(buf),
                                                     TESLA_DOMAIN_VEHICLE_SECURITY,
                                                     payload, payload_len,
                                                     g_conn.routing_address);
    if (len == 0) return ESP_FAIL;

    ESP_LOGI(TAG, "Sending unauthenticated VCSEC request (%lu bytes)", (unsigned long)len);

    esp_err_t ret = send_message(buf, len);
    if (ret != ESP_OK) return ret;

    if (!wait_for_response(5000)) {
        return ESP_ERR_TIMEOUT;
    }

    // 解析响应
    ESP_LOGD(TAG, "Received response (%lu bytes):", (unsigned long)g_conn.rx_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, g_conn.rx_buffer, g_conn.rx_len > 128 ? 128 : g_conn.rx_len, ESP_LOG_DEBUG);

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 调试：解析并打印响应
        tesla_parse_vcsec_response(g_conn.rx_buffer, g_conn.rx_len);

        tesla_parse_vehicle_status(g_conn.rx_buffer, g_conn.rx_len,
                                   &g_vehicle_state.body);
        g_vehicle_state.valid = true;
        xSemaphoreGive(g_mutex);
    }

    phone_notify_uart_packet_all();

    return ESP_OK;
}

/**
 * @brief 唤醒车辆
 * @return ESP_OK 成功，其他值失败
 * 
 * 从睡眠状态唤醒车辆
 */
esp_err_t tesla_ble_wake_vehicle(void)
{
    uint8_t payload[64];
    uint8_t buf[256];
    uint32_t payload_len, len;

    tesla_session_t *session = &g_conn.session[0];
    if (!session->established) {
        esp_err_t ret = tesla_ble_start_session(TESLA_DOMAIN_VEHICLE_SECURITY);
        if (ret != ESP_OK) return ret;
    }

    payload_len = tesla_build_rke_action(payload, sizeof(payload), TESLA_RKE_WAKE);

    len = tesla_build_routable_message(buf, sizeof(buf),
                                        TESLA_DOMAIN_VEHICLE_SECURITY,
                                        payload, payload_len,
                                        session,
                                        g_conn.routing_address,
                                        g_conn.public_key,
                                        g_conn.vin,
                                        0,
                                        NULL);  // 无需响应解密
    if (len == 0) return ESP_FAIL;

    return send_message(buf, len);
}

/**
 * @brief 锁定车辆
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_FAIL 失败
 */
esp_err_t tesla_ble_lock(void)
{
    uint8_t payload[64];
    uint8_t buf[256];
    uint32_t payload_len, len;

    tesla_session_t *session = &g_conn.session[0];
    if (!session->established) {
        return ESP_ERR_INVALID_STATE;
    }

    payload_len = tesla_build_rke_action(payload, sizeof(payload), TESLA_RKE_LOCK);

    len = tesla_build_routable_message(buf, sizeof(buf),
                                        TESLA_DOMAIN_VEHICLE_SECURITY,
                                        payload, payload_len,
                                        session,
                                        g_conn.routing_address,
                                        g_conn.public_key,
                                        g_conn.vin,
                                        0,
                                        NULL);  // 无需响应解密
    if (len == 0) return ESP_FAIL;

    return send_message(buf, len);
}

/**
 * @brief 解锁车辆
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_FAIL 失败
 */
esp_err_t tesla_ble_unlock(void)
{
    uint8_t payload[64];
    uint8_t buf[256];
    uint32_t payload_len, len;

    tesla_session_t *session = &g_conn.session[0];
    if (!session->established) {
        return ESP_ERR_INVALID_STATE;
    }

    payload_len = tesla_build_rke_action(payload, sizeof(payload), TESLA_RKE_UNLOCK);

    len = tesla_build_routable_message(buf, sizeof(buf),
                                        TESLA_DOMAIN_VEHICLE_SECURITY,
                                        payload, payload_len,
                                        session,
                                        g_conn.routing_address,
                                        g_conn.public_key,
                                        g_conn.vin,
                                        0,
                                        NULL);  // 无需响应解密
    if (len == 0) return ESP_FAIL;

    return send_message(buf, len);
}

/**
 * @brief 获取驾驶状态（速度、功率、档位等）
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 会话未建立，ESP_ERR_TIMEOUT 超时
 * 
 * 从 Infotainment 域获取车辆驾驶状态，包括速度、功率、档位、里程等
 */
esp_err_t tesla_ble_get_drive_state(void)
{
    uint8_t payload[64];
    uint8_t buf[512];
    uint32_t payload_len, len;

    // DriveState 需要 INFOTAINMENT 域的加密会话
    tesla_session_t *session = &g_conn.session[1];  // 信息娱乐会话

    // 如果尚未建立信息娱乐会话，先建立
    if (!session->established) {
        ESP_LOGI(TAG, "Establishing infotainment session for drive state...");
        esp_err_t ret = tesla_ble_start_session(TESLA_DOMAIN_INFOTAINMENT);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to establish infotainment session: %d", ret);
            return ret;
        }
    }

    // 发送请求前清除所有待处理的通知
    clear_pending_notifications(pdMS_TO_TICKS(50));

    // 构建 CarServer Action 以获取 DriveState
    payload_len = tesla_build_get_drive_state(payload, sizeof(payload));
    if (payload_len == 0) {
        ESP_LOGE(TAG, "Failed to build GetDriveState request");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "GetDriveState payload (%lu bytes):", (unsigned long)payload_len);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, payload, payload_len, ESP_LOG_DEBUG);

    // 构建到 INFOTAINMENT 域的加密可路由消息
    // 递增此会话的计数器
    session->counter++;

    // 设置 FLAG_ENCRYPT_RESPONSE 标志以请求车辆返回加密响应
    uint32_t flags = (1 << TESLA_FLAG_ENCRYPT_RESPONSE);

    len = tesla_build_routable_message(buf, sizeof(buf),
                                        TESLA_DOMAIN_INFOTAINMENT,
                                        payload, payload_len,
                                        session,
                                        g_conn.routing_address,
                                        g_conn.public_key,
                                        g_conn.vin,
                                        flags,
                                        g_conn.last_request_id);  // 保存 request_id 用于响应解密
    if (len == 0) {
        ESP_LOGE(TAG, "Failed to build routable message");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sending GetDriveState request (%lu bytes)", (unsigned long)len);

    // 多次尝试从信息娱乐系统获取响应
    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) {
            ESP_LOGD(TAG, "Retry %d: clearing buffer and resending...", retry);
            // 再次清除缓冲区
            clear_pending_notifications(pdMS_TO_TICKS(50));

            // 使用新计数器重新构建消息
            session->counter++;
            len = tesla_build_routable_message(buf, sizeof(buf),
                                                TESLA_DOMAIN_INFOTAINMENT,
                                                payload, payload_len,
                                                session,
                                                g_conn.routing_address,
                                                g_conn.public_key,
                                                g_conn.vin,
                                                flags,
                                                g_conn.last_request_id);  // 为新消息更新 request_id
        }

        esp_err_t ret = send_message(buf, len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send GetDriveState request");
            return ret;
        }

        // 使用重试逻辑等待正确的响应，跳过错误域的响应
        int wait_attempts = 0;
        const int max_wait_attempts = 10;  // 等待正确响应的最大次数
        bool got_correct_response = false;

        while (wait_attempts < max_wait_attempts && !got_correct_response) {
            wait_attempts++;

            // 等待响应
            if (!wait_for_response(3000)) {
                ESP_LOGE(TAG, "GetDriveState timeout (attempt %d)", wait_attempts);
                break;  // 超时，将重试发送
            }

            // 关键：在 wait_for_response 返回后立即保存消息长度
            // 因为新的通知可能会继续向缓冲区追加数据
            uint32_t response_len = g_conn.rx_len;

            // 记录原始响应
            ESP_LOGD(TAG, "Received response (%lu bytes)", (unsigned long)response_len);

            // 检查响应是来自 INFOTAINMENT（域 3）还是 VCSEC（域 2）
            if (response_len >= 8) {
                const uint8_t *p = g_conn.rx_buffer;
                // 检查字段 6 (to_destination)
                if (p[0] == 0x32 && p[2] == 0x08) {
                    uint8_t to_domain = p[3];
                    // 检查字段 7 (from_destination)
                    if (p[4] == 0x3a && p[6] == 0x08) {
                        uint8_t from_domain = p[7];
                        ESP_LOGD(TAG, "Response: to_domain=%d, from_domain=%d", to_domain, from_domain);

                        if (from_domain == TESLA_DOMAIN_VEHICLE_SECURITY) {
                            ESP_LOGW(TAG, "Received VCSEC broadcast, waiting for Infotainment response...");
                            // 清除缓冲区并继续等待正确的响应
                            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                g_conn.rx_len = 0;
                                g_conn.rx_complete = false;
                                xSemaphoreGive(g_mutex);
                            }
                            continue;  // 继续等待，不重试发送
                        }
                    }
                }
            }

            // 我们收到了来自正确域的响应
            // 使用保存的 response_len 进行解析，而不是可能已增长的 g_conn.rx_len
            got_correct_response = true;

            // 在我们拥有正确长度时立即解析和解密响应
            uint8_t decrypted_payload[512];
            uint32_t decrypted_len = 0;
            tesla_fault_t fault = TESLA_FAULT_NONE;

            // 注意：使用 response_len（保存的消息长度）而不是 g_conn.rx_len
            // 因为新的通知可能已经向缓冲区追加了数据
            if (!tesla_parse_and_decrypt_response(g_conn.rx_buffer, response_len,
                                                   session, g_conn.vin,
                                                   g_conn.last_request_id,
                                                   decrypted_payload, &decrypted_len, &fault)) {
                if (fault != TESLA_FAULT_NONE) {
                    ESP_LOGE(TAG, "Response contains fault: %d", fault);
                }
                ESP_LOGW(TAG, "Failed to parse/decrypt response, retrying...");
                got_correct_response = false;  // Force retry
                break;  // Break inner loop to retry sending
            }

            ESP_LOGD(TAG, "Decrypted payload (%lu bytes)", (unsigned long)decrypted_len);

            // Parse DriveState from decrypted response
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (tesla_parse_drive_state(decrypted_payload, decrypted_len, &g_vehicle_state.drive)) {
                    g_vehicle_state.valid = true;
                    ESP_LOGD(TAG, "DriveState: gear=%d speed=%.1f power=%ld odo=%.1f",
                             g_vehicle_state.drive.gear, g_vehicle_state.drive.speed_kmh,
                             (long)g_vehicle_state.drive.power_kw, g_vehicle_state.drive.odometer_km);
                    xSemaphoreGive(g_mutex);
                    phone_notify_uart_packet_all();
                    return ESP_OK;
                } else {
                    ESP_LOGW(TAG, "Failed to parse DriveState from decrypted data");
                }
                xSemaphoreGive(g_mutex);
            }
        }

        if (!got_correct_response) {
            ESP_LOGW(TAG, "Did not receive Infotainment response after %d attempts", wait_attempts);
            continue;  // 重试发送
        }

        // 如果执行到这里，说明解析成功并在上面返回了 ESP_OK
        // 只有在 DriveState 解析出现问题时才会到达此路径
    }

    ESP_LOGE(TAG, "Failed to get drive state after retries");
    return ESP_FAIL;
}

// ========== 状态打印 ==========

/**
 * @brief 打印车辆状态到控制台
 * @param state 车辆状态
 */
void tesla_ble_print_state(const tesla_vehicle_state_t *state)
{
    if (!state || !state->valid) {
        printf("Vehicle state not available\n");
        return;
    }

    printf("\n=== 车辆车身状态 ===\n");
    printf("锁定: %s\n", state->body.lock_state == TESLA_LOCK_LOCKED ? "已锁定" : "未锁定");
    printf("睡眠: %s\n", state->body.sleep_status == TESLA_SLEEP_AWAKE ? "唤醒" : "睡眠");

    printf("\n=== 关闭状态 ===\n");
    printf("前排驾驶员门: %s\n", state->body.closures.front_driver_door == TESLA_CLOSURE_CLOSED ? "关闭" : "打开");
    printf("前舱盖: %s\n", state->body.closures.front_trunk == TESLA_CLOSURE_CLOSED ? "关闭" : "打开");
    printf("后备箱: %s\n", state->body.closures.rear_trunk == TESLA_CLOSURE_CLOSED ? "关闭" : "打开");

    printf("\n=== 充电状态 ===\n");
    printf("电池: %" PRId32 "%%\n", state->charge.battery_level);
    printf("续航: %.1f km\n", state->charge.battery_range_km);

    printf("\n=== 气候 ===\n");
    printf("气候开启: %s\n", state->climate.climate_on ? "是" : "否");
    printf("车内温度: %.1f°C\n", state->climate.inside_temp_c);
    printf("车外温度: %.1f°C\n", state->climate.outside_temp_c);
}
