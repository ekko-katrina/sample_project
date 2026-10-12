#include "wifi_provisioning.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAXIMUM_RETRY 5
#define DHCPS_OFFER_DNS 0x02
#define DNS_BUFFER_SIZE 512
#define HTTP_BODY_SIZE 256
#define WIFI_SCAN_MAX_RECORDS 40U
#define WIFI_BLE_MAX_FRAME_SIZE 128U
#define WIFI_BLE_CONNECT_TIMEOUT_MS 30000U
#define WIFI_BLE_NOTIFY_QUEUE_LENGTH 24U
#define WIFI_BLE_NOTIFY_TASK_STACK_SIZE 4096U

#define WIFI_COMMAND_QUERY_STATUS 0xB0U
#define WIFI_COMMAND_STATUS 0xB1U
#define WIFI_COMMAND_SCAN 0xB2U
#define WIFI_COMMAND_SCAN_EVENT 0xB3U
#define WIFI_COMMAND_CONFIGURE 0xB4U
#define WIFI_COMMAND_CONFIGURE_EVENT 0xB5U
#define WIFI_COMMAND_CLEAR 0xB6U
#define WIFI_COMMAND_CLEAR_RESULT 0xB7U

#define WIFI_STATE_NOT_CONFIGURED 0x00U
#define WIFI_STATE_SAVED_OFFLINE 0x01U
#define WIFI_STATE_CONNECTING 0x02U
#define WIFI_STATE_CONNECTED 0x03U

#define WIFI_SCAN_STARTED 0x00U
#define WIFI_SCAN_AP_ITEM 0x01U
#define WIFI_SCAN_FINISHED 0x02U
#define WIFI_SCAN_FAILED 0x03U

#define WIFI_STAGE_ACCEPTED 0x00U
#define WIFI_STAGE_ASSOCIATING 0x01U
#define WIFI_STAGE_OBTAINING_IP 0x02U
#define WIFI_STAGE_SUCCESS 0x03U
#define WIFI_STAGE_FAILED 0x04U

#define WIFI_RESULT_IN_PROGRESS 0x00U
#define WIFI_RESULT_SUCCESS 0x01U
#define WIFI_RESULT_FAILED 0x02U

#define WIFI_ERROR_NONE 0x00U
#define WIFI_ERROR_BUSY 0x01U
#define WIFI_ERROR_INVALID_PARAM 0x02U
#define WIFI_ERROR_AP_NOT_FOUND 0x04U
#define WIFI_ERROR_AUTH_FAILED 0x05U
#define WIFI_ERROR_ASSOC_TIMEOUT 0x06U
#define WIFI_ERROR_DHCP_FAILED 0x07U
#define WIFI_ERROR_UNSUPPORTED_AUTH 0x08U
#define WIFI_ERROR_INTERNAL 0x09U
#define WIFI_ERROR_SAVE_FAILED 0x0AU
#define WIFI_ERROR_CANCELLED 0x0BU
#define WIFI_ERROR_SESSION_INVALID 0x0CU
#define WIFI_ERROR_UNKNOWN 0xFFU

static const char *TAG = "WIFI-PROV";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASSWORD = "password";
static const char *NVS_KEY_BSSID = "bssid";
static const char *NVS_KEY_USE_BSSID = "use_bssid";
static const char *PORTAL_ADDRESS = "http://192.168.4.1/";

typedef struct {
    char ssid[33];
    char password[65];
    uint8_t bssid[6];
    bool use_bssid;
} wifi_credentials_t;

typedef struct {
    bool active;
    uint16_t connection_handle;
    uint16_t request_id;
    uint8_t stage;
    wifi_credentials_t credentials;
} wifi_ble_connect_request_t;

typedef struct {
    uint16_t connection_handle;
    size_t frame_length;
    uint8_t frame[WIFI_BLE_MAX_FRAME_SIZE];
} wifi_ble_notification_t;

static EventGroupHandle_t g_wifi_events;
static esp_netif_t *g_sta_netif;
static esp_netif_t *g_ap_netif;
static httpd_handle_t g_http_server;
static TaskHandle_t g_dns_task;
static bool g_dns_running;
static bool g_portal_started;
static bool g_portal_starting;
static bool g_portal_stopping;
static bool g_have_credentials;
static int g_retry_count;
static char g_ap_ssid[33];
static wifi_credentials_t g_saved_credentials;
static wifi_ble_connect_request_t g_ble_connect_request;
static wifi_provisioning_ble_notify_t g_ble_notify_handler;
static QueueHandle_t g_ble_notify_queue;
static TaskHandle_t g_ble_notify_task;
static TaskHandle_t g_ble_scan_task;
static uint16_t g_ble_scan_connection;
static uint16_t g_ble_scan_request_id;
static bool g_ble_scan_show_hidden;
static bool g_ignore_next_disconnect;
static bool g_ble_waiting_for_ip;
static uint8_t g_last_wifi_error;

static const char PORTAL_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>设备配网</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#f3f6fa;color:#172033;font-family:system-ui,-apple-system,sans-serif}"
    ".card{width:min(92%,420px);margin:10vh auto;background:#fff;padding:28px;border-radius:18px;box-shadow:0 16px 50px #20305024}"
    "h1{font-size:24px;margin:0 0 8px}p{color:#647087;line-height:1.55;margin:0 0 22px}"
    "label{display:block;font-size:14px;font-weight:600;margin:15px 0 7px}"
    "input{width:100%;padding:13px 14px;border:1px solid #ccd4e0;border-radius:10px;font-size:16px;outline:none}"
    "input:focus{border-color:#2267e5;box-shadow:0 0 0 3px #2267e51c}"
    "button{width:100%;margin-top:22px;padding:14px;border:0;border-radius:10px;background:#2267e5;color:#fff;font-size:16px;font-weight:700}"
    ".hint{font-size:12px;margin-top:14px;color:#8893a6}</style></head><body><main class=\"card\">"
    "<h1>连接设备到 Wi-Fi</h1><p>请输入家庭或现场的 2.4 GHz Wi-Fi 信息。提交后设备会保存配置并自动连接。</p>"
    "<form method=\"post\" action=\"/configure\"><label for=\"ssid\">Wi-Fi 名称</label>"
    "<input id=\"ssid\" name=\"ssid\" maxlength=\"32\" required autocomplete=\"off\">"
    "<label for=\"password\">Wi-Fi 密码</label><input id=\"password\" name=\"password\" type=\"password\" maxlength=\"64\">"
    "<button type=\"submit\">保存并连接</button></form><div class=\"hint\">仅支持 2.4 GHz Wi-Fi</div>"
    "</main></body></html>";

static const char SUCCESS_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>配置已保存</title>"
    "<style>body{font-family:system-ui;text-align:center;padding:18vh 24px;color:#172033}"
    "b{display:block;color:#178447;font-size:26px;margin-bottom:12px}</style></head>"
    "<body><b>配置已保存</b>设备正在连接 Wi-Fi。连接成功后此热点会自动关闭。</body></html>";

static esp_err_t load_credentials(wifi_credentials_t *credentials)
{
    if (credentials == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(credentials, 0, sizeof(*credentials));
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_length = sizeof(credentials->ssid);
    size_t password_length = sizeof(credentials->password);
    err = nvs_get_str(handle, NVS_KEY_SSID, credentials->ssid, &ssid_length);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, NVS_KEY_PASSWORD,
                          credentials->password, &password_length);
    }
    if (err == ESP_OK) {
        uint8_t use_bssid = 0U;
        esp_err_t optional = nvs_get_u8(handle, NVS_KEY_USE_BSSID,
                                        &use_bssid);
        if (optional == ESP_OK && use_bssid != 0U) {
            size_t bssid_length = sizeof(credentials->bssid);
            optional = nvs_get_blob(handle, NVS_KEY_BSSID,
                                    credentials->bssid, &bssid_length);
            credentials->use_bssid = optional == ESP_OK &&
                                     bssid_length == sizeof(credentials->bssid);
        }
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_credentials(const wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, credentials->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_PASSWORD, credentials->password);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, NVS_KEY_USE_BSSID,
                         credentials->use_bssid ? 1U : 0U);
    }
    if (err == ESP_OK && credentials->use_bssid) {
        err = nvs_set_blob(handle, NVS_KEY_BSSID,
                           credentials->bssid, sizeof(credentials->bssid));
    } else if (err == ESP_OK) {
        esp_err_t erase_result = nvs_erase_key(handle, NVS_KEY_BSSID);
        if (erase_result != ESP_OK && erase_result != ESP_ERR_NVS_NOT_FOUND) {
            err = erase_result;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t clear_credentials(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void apply_station_config(const wifi_credentials_t *credentials,
                                 wifi_config_t *station_config)
{
    memset(station_config, 0, sizeof(*station_config));
    strlcpy((char *)station_config->sta.ssid, credentials->ssid,
            sizeof(station_config->sta.ssid));
    strlcpy((char *)station_config->sta.password, credentials->password,
            sizeof(station_config->sta.password));
    station_config->sta.threshold.authmode = credentials->password[0] == '\0'
                                                 ? WIFI_AUTH_OPEN
                                                 : WIFI_AUTH_WPA_PSK;
    station_config->sta.bssid_set = credentials->use_bssid;
    if (credentials->use_bssid) {
        memcpy(station_config->sta.bssid, credentials->bssid,
               sizeof(credentials->bssid));
    }
}

static uint8_t wifi_ble_checksum(const uint8_t *data, size_t length)
{
    uint8_t checksum = 0U;
    for (size_t i = 0; i < length; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

static bool wifi_ble_validate_frame(const uint8_t *frame, size_t length)
{
    return frame != NULL && length >= 6U && frame[0] == 0xA5U &&
           frame[1] == 0x5AU && frame[2] >= 3U &&
           length == (size_t)frame[2] + 3U && frame[length - 1U] == 0xAAU &&
           frame[length - 2U] == wifi_ble_checksum(frame, length - 2U);
}

static size_t wifi_ble_build_frame(uint8_t command,
                                   const uint8_t *payload,
                                   size_t payload_length,
                                   uint8_t *frame,
                                   size_t capacity)
{
    size_t frame_length = payload_length + 6U;
    if (frame == NULL || frame_length > capacity ||
        payload_length + 3U > UINT8_MAX) {
        return 0U;
    }
    frame[0] = 0xA5U;
    frame[1] = 0x5AU;
    frame[2] = (uint8_t)(payload_length + 3U);
    frame[3] = command;
    if (payload_length > 0U && payload != NULL) {
        memcpy(frame + 4U, payload, payload_length);
    }
    frame[frame_length - 2U] = wifi_ble_checksum(frame, frame_length - 2U);
    frame[frame_length - 1U] = 0xAAU;
    return frame_length;
}

static void wifi_ble_notify_worker(void *argument)
{
    (void)argument;
    wifi_ble_notification_t notification;
    while (true) {
        if (xQueueReceive(g_ble_notify_queue, &notification,
                          portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (g_ble_notify_handler != NULL) {
            g_ble_notify_handler(notification.connection_handle,
                                 notification.frame,
                                 notification.frame_length);
        }
    }
}

static uint16_t wifi_ble_read_u16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static size_t wifi_bounded_ssid_length(const uint8_t *ssid)
{
    size_t length = 0U;
    while (length < 32U && ssid[length] != 0U) {
        ++length;
    }
    return length;
}

static void wifi_ble_notify(uint16_t connection_handle,
                            uint8_t command,
                            const uint8_t *payload,
                            size_t payload_length)
{
    if (g_ble_notify_handler == NULL || g_ble_notify_queue == NULL) {
        return;
    }
    wifi_ble_notification_t notification = {
        .connection_handle = connection_handle,
    };
    size_t frame_length = wifi_ble_build_frame(command, payload,
                                                payload_length,
                                                notification.frame,
                                                sizeof(notification.frame));
    if (frame_length == 0U) {
        return;
    }
    notification.frame_length = frame_length;
    if (xQueueSend(g_ble_notify_queue, &notification, 0U) != pdTRUE) {
        ESP_LOGW(TAG, "BLE notify queue full: command=0x%02X", command);
    }
}

static void wifi_ble_get_ip(uint8_t output[4])
{
    memset(output, 0, 4U);
    if (g_sta_netif == NULL || !wifi_provisioning_is_connected()) {
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(g_sta_netif, &ip_info) != ESP_OK) {
        return;
    }
    uint32_t address = ntohl(ip_info.ip.addr);
    output[0] = (uint8_t)(address >> 24U);
    output[1] = (uint8_t)(address >> 16U);
    output[2] = (uint8_t)(address >> 8U);
    output[3] = (uint8_t)address;
}

static uint8_t wifi_ble_state(void)
{
    if (g_ble_connect_request.active) {
        return WIFI_STATE_CONNECTING;
    }
    if (!g_have_credentials) {
        return WIFI_STATE_NOT_CONFIGURED;
    }
    if (wifi_provisioning_is_connected()) {
        return WIFI_STATE_CONNECTED;
    }
    return WIFI_STATE_SAVED_OFFLINE;
}

static void wifi_ble_send_status(uint16_t connection_handle,
                                 uint16_t request_id)
{
    uint8_t payload[42U] = {0};
    payload[0] = (uint8_t)(request_id >> 8U);
    payload[1] = (uint8_t)request_id;
    payload[2] = wifi_ble_state();
    payload[3] = g_last_wifi_error;
    payload[4] = 0x80U;

    const char *ssid = g_ble_connect_request.active
                           ? g_ble_connect_request.credentials.ssid
                           : g_saved_credentials.ssid;
    wifi_ap_record_t ap_info = {0};
    if (wifi_provisioning_is_connected() &&
        esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ssid = (const char *)ap_info.ssid;
        payload[4] = (uint8_t)ap_info.rssi;
    }
    size_t ssid_length = strnlen(ssid, 32U);
    payload[5] = (uint8_t)ssid_length;
    memcpy(payload + 6U, ssid, ssid_length);
    wifi_ble_get_ip(payload + 6U + ssid_length);
    wifi_ble_notify(connection_handle, WIFI_COMMAND_STATUS,
                    payload, 10U + ssid_length);
}

static void wifi_ble_send_scan_event(uint16_t connection_handle,
                                     uint16_t request_id,
                                     uint8_t event,
                                     const wifi_ap_record_t *record)
{
    uint8_t payload[44U] = {0};
    payload[0] = (uint8_t)(request_id >> 8U);
    payload[1] = (uint8_t)request_id;
    payload[2] = event;
    size_t payload_length = 3U;

    if (event == WIFI_SCAN_AP_ITEM && record != NULL) {
        payload[3] = (uint8_t)record->rssi;
        switch (record->authmode) {
        case WIFI_AUTH_OPEN: payload[4] = 0x00U; break;
        case WIFI_AUTH_WEP: payload[4] = 0x01U; break;
        case WIFI_AUTH_WPA_PSK: payload[4] = 0x02U; break;
        case WIFI_AUTH_WPA2_PSK: payload[4] = 0x03U; break;
        case WIFI_AUTH_WPA_WPA2_PSK: payload[4] = 0x04U; break;
        case WIFI_AUTH_WPA2_ENTERPRISE: payload[4] = 0x05U; break;
        case WIFI_AUTH_WPA3_PSK: payload[4] = 0x06U; break;
        case WIFI_AUTH_WPA2_WPA3_PSK: payload[4] = 0x07U; break;
        default: payload[4] = 0xFFU; break;
        }
        memcpy(payload + 5U, record->bssid, 6U);
        size_t ssid_length = wifi_bounded_ssid_length(record->ssid);
        payload[11] = (uint8_t)ssid_length;
        memcpy(payload + 12U, record->ssid, ssid_length);
        payload_length = 12U + ssid_length;
    }
    wifi_ble_notify(connection_handle, WIFI_COMMAND_SCAN_EVENT,
                    payload, payload_length);
}

static void wifi_ble_send_configure_event(uint16_t connection_handle,
                                          uint16_t request_id,
                                          uint8_t stage,
                                          uint8_t result,
                                          uint8_t error_code,
                                          const char *ssid)
{
    uint8_t payload[43U] = {0};
    payload[0] = (uint8_t)(request_id >> 8U);
    payload[1] = (uint8_t)request_id;
    payload[2] = stage;
    payload[3] = result;
    payload[4] = error_code;
    payload[5] = 0x80U;
    wifi_ap_record_t ap_info = {0};
    if (wifi_provisioning_is_connected() &&
        esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        payload[5] = (uint8_t)ap_info.rssi;
    }
    const char *safe_ssid = ssid != NULL ? ssid : "";
    size_t ssid_length = strnlen(safe_ssid, 32U);
    payload[6] = (uint8_t)ssid_length;
    memcpy(payload + 7U, safe_ssid, ssid_length);
    if (stage == WIFI_STAGE_SUCCESS && result == WIFI_RESULT_SUCCESS) {
        wifi_ble_get_ip(payload + 7U + ssid_length);
    }
    wifi_ble_notify(connection_handle, WIFI_COMMAND_CONFIGURE_EVENT,
                    payload, 11U + ssid_length);
}

static void wifi_ble_send_clear_result(uint16_t connection_handle,
                                       uint16_t request_id,
                                       uint8_t result,
                                       uint8_t error_code)
{
    const uint8_t payload[] = {
        (uint8_t)(request_id >> 8U), (uint8_t)request_id,
        result, error_code, wifi_ble_state(),
    };
    wifi_ble_notify(connection_handle, WIFI_COMMAND_CLEAR_RESULT,
                    payload, sizeof(payload));
}

static bool hex_value(char input, uint8_t *value)
{
    if (input >= '0' && input <= '9') {
        *value = (uint8_t)(input - '0');
        return true;
    }
    input = (char)tolower((unsigned char)input);
    if (input >= 'a' && input <= 'f') {
        *value = (uint8_t)(input - 'a' + 10);
        return true;
    }
    return false;
}

static bool url_decode(const char *source, char *destination, size_t capacity)
{
    size_t output = 0;
    while (*source != '\0') {
        if (output + 1U >= capacity) {
            return false;
        }
        if (*source == '+') {
            destination[output++] = ' ';
            ++source;
        } else if (*source == '%' && source[1] != '\0' && source[2] != '\0') {
            uint8_t high;
            uint8_t low;
            if (!hex_value(source[1], &high) || !hex_value(source[2], &low)) {
                return false;
            }
            destination[output++] = (char)((high << 4U) | low);
            source += 3;
        } else {
            destination[output++] = *source++;
        }
    }
    destination[output] = '\0';
    return true;
}

static esp_err_t portal_get_handler(httpd_req_t *request)
{
    if (strcmp(request->uri, "/generate_204") == 0 ||
        strcmp(request->uri, "/gen_204") == 0 ||
        strcmp(request->uri, "/connecttest.txt") == 0 ||
        strcmp(request->uri, "/redirect") == 0) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", PORTAL_ADDRESS);
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        return httpd_resp_send(request, NULL, 0);
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(request, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static void portal_shutdown_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (g_http_server != NULL) {
        httpd_stop(g_http_server);
        g_http_server = NULL;
    }
    g_dns_running = false;
    vTaskDelay(pdMS_TO_TICKS(750));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    g_portal_started = false;
    g_portal_stopping = false;
    ESP_LOGI(TAG, "配网成功，配置热点已关闭");
    vTaskDelete(NULL);
}

static esp_err_t portal_configure_handler(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len >= HTTP_BODY_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid request");
    }

    char body[HTTP_BODY_SIZE];
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    request->content_len - received);
        if (result <= 0) {
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Receive failed");
        }
        received += (size_t)result;
    }
    body[received] = '\0';

    wifi_credentials_t credentials = {0};
    bool have_ssid = false;
    char *save_pointer = NULL;
    for (char *field = strtok_r(body, "&", &save_pointer);
         field != NULL;
         field = strtok_r(NULL, "&", &save_pointer)) {
        if (strncmp(field, "ssid=", 5) == 0) {
            have_ssid = url_decode(field + 5, credentials.ssid,
                                   sizeof(credentials.ssid));
        } else if (strncmp(field, "password=", 9) == 0) {
            if (!url_decode(field + 9, credentials.password,
                            sizeof(credentials.password))) {
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                           "Invalid password");
            }
        }
    }

    if (!have_ssid || credentials.ssid[0] == '\0') {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "SSID required");
    }

    esp_err_t err = save_credentials(&credentials);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存 Wi-Fi 配置失败: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Save failed");
    }
    g_saved_credentials = credentials;

    wifi_config_t station_config;
    apply_station_config(&credentials, &station_config);

    g_have_credentials = true;
    g_retry_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA,
                                                       &station_config));
    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "开始连接 Wi-Fi 失败: %s", esp_err_to_name(err));
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 4;

    esp_err_t err = httpd_start(&g_http_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t configure = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = portal_configure_handler,
    };
    const httpd_uri_t all_get_requests = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = portal_get_handler,
    };
    err = httpd_register_uri_handler(g_http_server, &configure);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(g_http_server, &all_get_requests);
    }
    if (err != ESP_OK) {
        httpd_stop(g_http_server);
        g_http_server = NULL;
    }
    return err;
}

static void configure_captive_portal_dhcp(void)
{
    esp_netif_ip_info_t ip_info;
    if (g_ap_netif == NULL ||
        esp_netif_get_ip_info(g_ap_netif, &ip_info) != ESP_OK) {
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = ip_info.ip;
    uint8_t offer_dns = DHCPS_OFFER_DNS;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(g_ap_netif));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_set_dns_info(
        g_ap_netif, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
        &offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
        g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        (void *)PORTAL_ADDRESS, strlen(PORTAL_ADDRESS)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(g_ap_netif));
}

static size_t dns_question_length(const uint8_t *packet, size_t length)
{
    size_t offset = 12U;
    while (offset < length && packet[offset] != 0U) {
        uint8_t label_length = packet[offset];
        if ((label_length & 0xC0U) != 0U ||
            offset + 1U + label_length >= length) {
            return 0U;
        }
        offset += 1U + label_length;
    }
    if (offset + 5U > length) {
        return 0U;
    }
    return offset + 5U;
}

static void dns_server_task(void *argument)
{
    (void)argument;
    uint8_t request[DNS_BUFFER_SIZE];
    uint8_t response[DNS_BUFFER_SIZE];
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "无法创建 DNS socket: errno=%d", errno);
        g_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "无法监听 DNS 端口: errno=%d", errno);
        close(socket_fd);
        g_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (g_dns_running) {
        struct sockaddr_storage source_address;
        socklen_t source_length = sizeof(source_address);
        int received = recvfrom(socket_fd, request, sizeof(request), 0,
                                (struct sockaddr *)&source_address,
                                &source_length);
        if (received < 12) {
            continue;
        }

        size_t question_length = dns_question_length(request, (size_t)received);
        if (question_length == 0U || question_length + 16U > sizeof(response)) {
            continue;
        }

        memcpy(response, request, question_length);
        response[2] = 0x81;
        response[3] = 0x80;
        response[6] = 0x00;
        response[7] = 0x01;
        response[8] = response[9] = response[10] = response[11] = 0x00;

        size_t offset = question_length;
        const uint8_t answer_header[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
            192, 168, 4, 1,
        };
        memcpy(response + offset, answer_header, sizeof(answer_header));
        offset += sizeof(answer_header);
        sendto(socket_fd, response, offset, 0,
               (struct sockaddr *)&source_address, source_length);
    }

    close(socket_fd);
    g_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_portal_services(void)
{
    if (g_portal_started) {
        return ESP_OK;
    }

    configure_captive_portal_dhcp();
    esp_err_t err = start_http_server();
    if (err != ESP_OK) {
        g_portal_starting = false;
        ESP_LOGE(TAG, "启动配网页面失败: %s", esp_err_to_name(err));
        return err;
    }

    g_dns_running = true;
    if (xTaskCreate(dns_server_task, "captive_dns", 4096, NULL, 4,
                    &g_dns_task) != pdPASS) {
        g_dns_running = false;
        httpd_stop(g_http_server);
        g_http_server = NULL;
        g_portal_starting = false;
        return ESP_ERR_NO_MEM;
    }

    g_portal_started = true;
    g_portal_starting = false;
    ESP_LOGI(TAG, "配网热点已开启: %s，密码: %s",
             g_ap_ssid, CONFIG_TMY06_PROVISION_AP_PASSWORD);
    ESP_LOGI(TAG, "若手机未自动弹出页面，请访问 %s", PORTAL_ADDRESS);
    return ESP_OK;
}

static void enable_portal_task(void *argument)
{
    (void)argument;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    start_portal_services();
    vTaskDelete(NULL);
}

static void restore_saved_connection(void)
{
    if (!g_have_credentials || g_saved_credentials.ssid[0] == '\0') {
        if (!g_portal_started && !g_portal_starting) {
            g_portal_starting = true;
            if (xTaskCreate(enable_portal_task, "enable_portal", 3072,
                            NULL, 4, NULL) != pdPASS) {
                g_portal_starting = false;
            }
        }
        return;
    }

    wifi_config_t station_config;
    apply_station_config(&g_saved_credentials, &station_config);
    if (wifi_provisioning_is_connected()) {
        g_ignore_next_disconnect = true;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA,
                                                       &station_config));
    g_retry_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

static uint8_t map_disconnect_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return WIFI_ERROR_AP_NOT_FOUND;
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
        return WIFI_ERROR_AUTH_FAILED;
    case WIFI_REASON_ASSOC_EXPIRE:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_FAIL:
        return WIFI_ERROR_ASSOC_TIMEOUT;
    default:
        return WIFI_ERROR_ASSOC_TIMEOUT;
    }
}

static void finish_ble_connection_failure(uint8_t error_code)
{
    if (!g_ble_connect_request.active) {
        return;
    }
    wifi_ble_connect_request_t request = g_ble_connect_request;
    g_ble_connect_request.active = false;
    g_ble_waiting_for_ip = false;
    g_last_wifi_error = error_code;
    wifi_ble_send_configure_event(request.connection_handle,
                                  request.request_id,
                                  WIFI_STAGE_FAILED,
                                  WIFI_RESULT_FAILED,
                                  error_code,
                                  request.credentials.ssid);
    restore_saved_connection();
}

static void wifi_ble_scan_worker(void *argument)
{
    (void)argument;
    uint16_t request_id = g_ble_scan_request_id;
    uint16_t connection_handle = g_ble_scan_connection;
    wifi_scan_config_t scan_config = {
        .show_hidden = g_ble_scan_show_hidden,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK || request_id != g_ble_scan_request_id) {
        wifi_ble_send_scan_event(connection_handle, request_id,
                                 WIFI_SCAN_FAILED, NULL);
        g_ble_scan_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint16_t available = 0U;
    err = esp_wifi_scan_get_ap_num(&available);
    uint16_t record_count = available > WIFI_SCAN_MAX_RECORDS
                                ? WIFI_SCAN_MAX_RECORDS
                                : available;
    wifi_ap_record_t *records = NULL;
    if (err == ESP_OK && record_count > 0U) {
        records = calloc(record_count, sizeof(*records));
        if (records == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = esp_wifi_scan_get_ap_records(&record_count, records);
        }
    }

    if (err != ESP_OK) {
        free(records);
        wifi_ble_send_scan_event(connection_handle, request_id,
                                 WIFI_SCAN_FAILED, NULL);
        g_ble_scan_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    for (uint16_t i = 0U; i < record_count; ++i) {
        if (request_id != g_ble_scan_request_id) {
            break;
        }
        wifi_ble_send_scan_event(connection_handle, request_id,
                                 WIFI_SCAN_AP_ITEM, &records[i]);
        vTaskDelay(pdMS_TO_TICKS(35));
    }
    free(records);
    if (request_id == g_ble_scan_request_id) {
        wifi_ble_send_scan_event(connection_handle, request_id,
                                 WIFI_SCAN_FINISHED, NULL);
    }
    g_ble_scan_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_ble_connect_worker(void *argument)
{
    (void)argument;
    wifi_ble_connect_request_t request = g_ble_connect_request;
    g_ble_connect_request.stage = WIFI_STAGE_ASSOCIATING;
    wifi_ble_send_configure_event(request.connection_handle,
                                  request.request_id,
                                  WIFI_STAGE_ASSOCIATING,
                                  WIFI_RESULT_IN_PROGRESS,
                                  WIFI_ERROR_NONE,
                                  request.credentials.ssid);

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        mode = g_portal_started ? WIFI_MODE_APSTA : WIFI_MODE_STA;
        if (esp_wifi_set_mode(mode) != ESP_OK) {
            finish_ble_connection_failure(WIFI_ERROR_INTERNAL);
            vTaskDelete(NULL);
            return;
        }
    }

    wifi_config_t station_config;
    apply_station_config(&request.credentials, &station_config);
    g_ignore_next_disconnect = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));
    g_ignore_next_disconnect = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &station_config) != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        finish_ble_connection_failure(WIFI_ERROR_INTERNAL);
        vTaskDelete(NULL);
        return;
    }

    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(WIFI_BLE_CONNECT_TIMEOUT_MS);
    while (g_ble_connect_request.active &&
           g_ble_connect_request.request_id == request.request_id &&
           (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (g_ble_connect_request.active &&
        g_ble_connect_request.request_id == request.request_id) {
        finish_ble_connection_failure(g_ble_waiting_for_ip
                                          ? WIFI_ERROR_DHCP_FAILED
                                          : WIFI_ERROR_ASSOC_TIMEOUT);
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_have_credentials) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        if (g_ble_connect_request.active) {
            g_ble_waiting_for_ip = true;
            g_ble_connect_request.stage = WIFI_STAGE_OBTAINING_IP;
            wifi_ble_send_configure_event(
                g_ble_connect_request.connection_handle,
                g_ble_connect_request.request_id,
                WIFI_STAGE_OBTAINING_IP,
                WIFI_RESULT_IN_PROGRESS,
                WIFI_ERROR_NONE,
                g_ble_connect_request.credentials.ssid);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        if (g_ignore_next_disconnect) {
            g_ignore_next_disconnect = false;
            return;
        }
        if (g_ble_connect_request.active && g_retry_count < WIFI_MAXIMUM_RETRY) {
            ++g_retry_count;
            esp_wifi_connect();
            ESP_LOGW(TAG, "BLE Wi-Fi 配网连接失败，重试 %d/%d",
                     g_retry_count, WIFI_MAXIMUM_RETRY);
        } else if (g_ble_connect_request.active) {
            finish_ble_connection_failure(map_disconnect_reason(event->reason));
        } else if (g_have_credentials && g_retry_count < WIFI_MAXIMUM_RETRY) {
            ++g_retry_count;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi 连接失败，重试 %d/%d",
                     g_retry_count, WIFI_MAXIMUM_RETRY);
        } else if (!g_portal_started && !g_portal_starting) {
            ESP_LOGW(TAG, "无法连接已保存的 Wi-Fi，启动配网热点");
            g_portal_starting = true;
            if (xTaskCreate(enable_portal_task, "enable_portal", 3072,
                            NULL, 4, NULL) != pdPASS) {
                g_portal_starting = false;
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        g_retry_count = 0;
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "已连接 Wi-Fi，IP=" IPSTR, IP2STR(&event->ip_info.ip));
        if (g_ble_connect_request.active) {
            wifi_ble_connect_request_t request = g_ble_connect_request;
            esp_err_t save_result = save_credentials(&request.credentials);
            if (save_result == ESP_OK) {
                g_saved_credentials = request.credentials;
                g_have_credentials = true;
                g_last_wifi_error = WIFI_ERROR_NONE;
                g_ble_connect_request.active = false;
                g_ble_waiting_for_ip = false;
                wifi_ble_send_configure_event(request.connection_handle,
                                               request.request_id,
                                               WIFI_STAGE_SUCCESS,
                                               WIFI_RESULT_SUCCESS,
                                               WIFI_ERROR_NONE,
                                               request.credentials.ssid);
            } else {
                ESP_LOGE(TAG, "保存 BLE Wi-Fi 配置失败: %s",
                         esp_err_to_name(save_result));
                g_ble_connect_request.active = false;
                g_ble_waiting_for_ip = false;
                g_last_wifi_error = WIFI_ERROR_SAVE_FAILED;
                wifi_ble_send_configure_event(request.connection_handle,
                                               request.request_id,
                                               WIFI_STAGE_FAILED,
                                               WIFI_RESULT_FAILED,
                                               WIFI_ERROR_SAVE_FAILED,
                                               request.credentials.ssid);
                restore_saved_connection();
                return;
            }
        }
        if (g_portal_started && !g_portal_stopping) {
            g_portal_stopping = true;
            if (xTaskCreate(portal_shutdown_task, "portal_stop", 3072,
                            NULL, 4, NULL) != pdPASS) {
                g_portal_stopping = false;
            }
        }
    }
}

esp_err_t wifi_provisioning_start(void)
{
    memset(&g_saved_credentials, 0, sizeof(g_saved_credentials));
    g_have_credentials = load_credentials(&g_saved_credentials) == ESP_OK &&
                         g_saved_credentials.ssid[0] != '\0';

    g_wifi_events = xEventGroupCreate();
    if (g_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    g_ble_notify_queue = xQueueCreate(WIFI_BLE_NOTIFY_QUEUE_LENGTH,
                                      sizeof(wifi_ble_notification_t));
    if (g_ble_notify_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(wifi_ble_notify_worker, "wifi_ble_notify",
                    WIFI_BLE_NOTIFY_TASK_STACK_SIZE, NULL, 5,
                    &g_ble_notify_task) != pdPASS) {
        vQueueDelete(g_ble_notify_queue);
        g_ble_notify_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    if (g_sta_netif == NULL || g_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "Wi-Fi RAM storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID,
                            wifi_event_handler, NULL),
                        TAG, "Wi-Fi handler registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP,
                            wifi_event_handler, NULL),
                        TAG, "IP handler registration failed");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP),
                        TAG, "Cannot read Wi-Fi MAC");
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "TMY06-Setup-%02X%02X",
             mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                        TAG, "Initial Wi-Fi mode failed");

    wifi_config_t access_point_config = {0};
    strlcpy((char *)access_point_config.ap.ssid, g_ap_ssid,
            sizeof(access_point_config.ap.ssid));
    strlcpy((char *)access_point_config.ap.password,
            CONFIG_TMY06_PROVISION_AP_PASSWORD,
            sizeof(access_point_config.ap.password));
    access_point_config.ap.ssid_len = strlen(g_ap_ssid);
    access_point_config.ap.max_connection = 4;
    access_point_config.ap.authmode = strlen(CONFIG_TMY06_PROVISION_AP_PASSWORD) >= 8U
                                          ? WIFI_AUTH_WPA2_PSK
                                          : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point_config),
                        TAG, "AP config failed");

    if (g_have_credentials) {
        wifi_config_t station_config;
        apply_station_config(&g_saved_credentials, &station_config);
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station_config),
                            TAG, "STA config failed");
    }

    if (g_have_credentials) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA),
                            TAG, "STA mode failed");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    if (!g_have_credentials) {
        ESP_RETURN_ON_ERROR(start_portal_services(), TAG,
                            "Provisioning portal failed");
    } else {
        ESP_LOGI(TAG, "正在连接已保存的 Wi-Fi: %s",
                 g_saved_credentials.ssid);
    }
    return ESP_OK;
}

bool wifi_provisioning_is_connected(void)
{
    return g_wifi_events != NULL &&
           (xEventGroupGetBits(g_wifi_events) & WIFI_CONNECTED_BIT) != 0U;
}

bool wifi_provisioning_wait_connected(TickType_t timeout)
{
    if (g_wifi_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           timeout);
    return (bits & WIFI_CONNECTED_BIT) != 0U;
}

const char *wifi_provisioning_ap_ssid(void)
{
    return g_ap_ssid;
}

void wifi_provisioning_set_ble_notify_handler(
    wifi_provisioning_ble_notify_t handler)
{
    g_ble_notify_handler = handler;
}

static bool contains_zero_byte(const uint8_t *bytes, size_t length)
{
    for (size_t i = 0U; i < length; ++i) {
        if (bytes[i] == 0U) {
            return true;
        }
    }
    return false;
}

static bool is_hex_password(const uint8_t *bytes, size_t length)
{
    if (length != 64U) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (!isxdigit(bytes[i])) {
            return false;
        }
    }
    return true;
}

bool wifi_provisioning_handle_ble_frame(uint16_t connection_handle,
                                        const uint8_t *frame,
                                        size_t length)
{
    if (!wifi_ble_validate_frame(frame, length)) {
        return false;
    }
    uint8_t command = frame[3];
    if (command != WIFI_COMMAND_QUERY_STATUS &&
        command != WIFI_COMMAND_SCAN &&
        command != WIFI_COMMAND_CONFIGURE &&
        command != WIFI_COMMAND_CLEAR) {
        return false;
    }

    const uint8_t *payload = frame + 4U;
    size_t payload_length = length - 6U;
    uint16_t request_id = payload_length >= 2U
                              ? wifi_ble_read_u16(payload)
                              : 0U;

    if (command == WIFI_COMMAND_QUERY_STATUS) {
        if (payload_length == 2U && request_id != 0U) {
            wifi_ble_send_status(connection_handle, request_id);
        }
        return true;
    }

    if (command == WIFI_COMMAND_SCAN) {
        if (payload_length != 3U || request_id == 0U ||
            (payload[2] & 0xFEU) != 0U) {
            wifi_ble_send_scan_event(connection_handle, request_id,
                                     WIFI_SCAN_FAILED, NULL);
            return true;
        }
        if (g_ble_connect_request.active) {
            wifi_ble_send_scan_event(connection_handle, request_id,
                                     WIFI_SCAN_FAILED, NULL);
            return true;
        }
        if (g_ble_scan_task != NULL) {
            wifi_ble_send_scan_event(connection_handle, request_id,
                                     request_id == g_ble_scan_request_id
                                         ? WIFI_SCAN_STARTED
                                         : WIFI_SCAN_FAILED,
                                     NULL);
            return true;
        }
        g_ble_scan_connection = connection_handle;
        g_ble_scan_request_id = request_id;
        g_ble_scan_show_hidden = (payload[2] & 0x01U) != 0U;
        wifi_ble_send_scan_event(connection_handle, request_id,
                                 WIFI_SCAN_STARTED, NULL);
        if (xTaskCreate(wifi_ble_scan_worker, "ble_wifi_scan", 4096,
                        NULL, 4, &g_ble_scan_task) != pdPASS) {
            g_ble_scan_task = NULL;
            wifi_ble_send_scan_event(connection_handle, request_id,
                                     WIFI_SCAN_FAILED, NULL);
        }
        return true;
    }

    if (command == WIFI_COMMAND_CONFIGURE) {
        if (payload_length < 11U || request_id == 0U) {
            wifi_ble_send_configure_event(connection_handle, request_id,
                                          WIFI_STAGE_FAILED,
                                          WIFI_RESULT_FAILED,
                                          WIFI_ERROR_INVALID_PARAM, "");
            return true;
        }
        uint8_t flags = payload[2];
        uint8_t ssid_length = payload[3];
        uint8_t password_length = payload[4];
        bool hidden = (flags & 0x01U) != 0U;
        bool use_bssid = (flags & 0x02U) != 0U;
        bool lengths_valid = (flags & 0xFCU) == 0U &&
                             ssid_length <= 32U &&
                             password_length <= 64U &&
                             payload_length ==
                                 11U + ssid_length + password_length &&
                             (hidden || ssid_length > 0U);
        bool bssid_valid = !use_bssid;
        for (size_t i = 0U; use_bssid && i < 6U; ++i) {
            bssid_valid = bssid_valid || payload[5U + i] != 0U;
        }
        if (!lengths_valid || !bssid_valid) {
            wifi_ble_send_configure_event(connection_handle, request_id,
                                          WIFI_STAGE_FAILED,
                                          WIFI_RESULT_FAILED,
                                          WIFI_ERROR_INVALID_PARAM, "");
            return true;
        }
        const uint8_t *ssid = payload + 11U;
        const uint8_t *password = ssid + ssid_length;
        bool password_valid = password_length == 0U ||
                              (password_length >= 8U &&
                               password_length <= 63U) ||
                              is_hex_password(password, password_length);
        if (!password_valid ||
            contains_zero_byte(ssid, ssid_length) ||
            contains_zero_byte(password, password_length)) {
            wifi_ble_send_configure_event(connection_handle, request_id,
                                          WIFI_STAGE_FAILED,
                                          WIFI_RESULT_FAILED,
                                          WIFI_ERROR_INVALID_PARAM, "");
            return true;
        }
        if (g_ble_connect_request.active) {
            if (g_ble_connect_request.connection_handle == connection_handle &&
                g_ble_connect_request.request_id == request_id) {
                wifi_ble_send_configure_event(
                    connection_handle,
                    request_id,
                    g_ble_connect_request.stage,
                    WIFI_RESULT_IN_PROGRESS,
                    WIFI_ERROR_NONE,
                    g_ble_connect_request.credentials.ssid);
                return true;
            }
            wifi_ble_send_configure_event(connection_handle, request_id,
                                          WIFI_STAGE_FAILED,
                                          WIFI_RESULT_FAILED,
                                          WIFI_ERROR_BUSY, "");
            return true;
        }

        memset(&g_ble_connect_request, 0, sizeof(g_ble_connect_request));
        g_ble_connect_request.active = true;
        g_ble_connect_request.connection_handle = connection_handle;
        g_ble_connect_request.request_id = request_id;
        g_ble_connect_request.stage = WIFI_STAGE_ACCEPTED;
        memcpy(g_ble_connect_request.credentials.ssid, ssid, ssid_length);
        memcpy(g_ble_connect_request.credentials.password,
               password, password_length);
        g_ble_connect_request.credentials.use_bssid = use_bssid;
        if (use_bssid) {
            memcpy(g_ble_connect_request.credentials.bssid, payload + 5U, 6U);
        }
        g_retry_count = 0;
        g_ble_waiting_for_ip = false;
        g_last_wifi_error = WIFI_ERROR_NONE;
        wifi_ble_send_configure_event(connection_handle, request_id,
                                      WIFI_STAGE_ACCEPTED,
                                      WIFI_RESULT_IN_PROGRESS,
                                      WIFI_ERROR_NONE,
                                      g_ble_connect_request.credentials.ssid);
        if (xTaskCreate(wifi_ble_connect_worker, "ble_wifi_connect", 4096,
                        NULL, 5, NULL) != pdPASS) {
            finish_ble_connection_failure(WIFI_ERROR_INTERNAL);
        }
        return true;
    }

    if (payload_length != 3U || request_id == 0U ||
        (payload[2] & 0xFEU) != 0U) {
        wifi_ble_send_clear_result(connection_handle, request_id,
                                   WIFI_RESULT_FAILED,
                                   WIFI_ERROR_INVALID_PARAM);
        return true;
    }
    if (g_ble_connect_request.active) {
        wifi_ble_send_clear_result(connection_handle, request_id,
                                   WIFI_RESULT_FAILED, WIFI_ERROR_BUSY);
        return true;
    }
    esp_err_t clear_result = clear_credentials();
    if (clear_result == ESP_OK) {
        memset(&g_saved_credentials, 0, sizeof(g_saved_credentials));
        g_have_credentials = false;
        g_last_wifi_error = WIFI_ERROR_NONE;
        wifi_ble_send_clear_result(connection_handle, request_id,
                                   WIFI_RESULT_SUCCESS, WIFI_ERROR_NONE);
        if ((payload[2] & 0x01U) != 0U) {
            esp_wifi_disconnect();
        }
    } else {
        wifi_ble_send_clear_result(connection_handle, request_id,
                                   WIFI_RESULT_FAILED,
                                   WIFI_ERROR_SAVE_FAILED);
    }
    return true;
}

void wifi_provisioning_handle_ble_disconnect(uint16_t connection_handle)
{
    if (g_ble_scan_task != NULL &&
        g_ble_scan_connection == connection_handle) {
        g_ble_scan_request_id = 0U;
    }
}
