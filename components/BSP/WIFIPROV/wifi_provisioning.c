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

static const char *TAG = "WIFI-PROV";
static const char *NVS_NAMESPACE = "wifi_cfg";
static const char *NVS_KEY_SSID = "ssid";
static const char *NVS_KEY_PASSWORD = "password";
static const char *PORTAL_ADDRESS = "http://192.168.4.1/";

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

static EventGroupHandle_t g_wifi_events;
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
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
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

    wifi_config_t station_config = {0};
    strlcpy((char *)station_config.sta.ssid, credentials.ssid,
            sizeof(station_config.sta.ssid));
    strlcpy((char *)station_config.sta.password, credentials.password,
            sizeof(station_config.sta.password));
    station_config.sta.threshold.authmode = credentials.password[0] == '\0'
                                                ? WIFI_AUTH_OPEN
                                                : WIFI_AUTH_WPA2_PSK;

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
             g_ap_ssid, CONFIG_TMY05_PROVISION_AP_PASSWORD);
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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        if (g_have_credentials && g_retry_count < WIFI_MAXIMUM_RETRY) {
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
    wifi_credentials_t credentials = {0};
    g_have_credentials = load_credentials(&credentials) == ESP_OK &&
                         credentials.ssid[0] != '\0';

    g_wifi_events = xEventGroupCreate();
    if (g_wifi_events == NULL) {
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

    esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();
    if (g_ap_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&initialization), TAG, "Wi-Fi init failed");
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
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "TMY05-Setup-%02X%02X",
             mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA),
                        TAG, "Initial Wi-Fi mode failed");

    wifi_config_t access_point_config = {0};
    strlcpy((char *)access_point_config.ap.ssid, g_ap_ssid,
            sizeof(access_point_config.ap.ssid));
    strlcpy((char *)access_point_config.ap.password,
            CONFIG_TMY05_PROVISION_AP_PASSWORD,
            sizeof(access_point_config.ap.password));
    access_point_config.ap.ssid_len = strlen(g_ap_ssid);
    access_point_config.ap.max_connection = 4;
    access_point_config.ap.authmode = strlen(CONFIG_TMY05_PROVISION_AP_PASSWORD) >= 8U
                                          ? WIFI_AUTH_WPA2_PSK
                                          : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point_config),
                        TAG, "AP config failed");

    if (g_have_credentials) {
        wifi_config_t station_config = {0};
        strlcpy((char *)station_config.sta.ssid, credentials.ssid,
                sizeof(station_config.sta.ssid));
        strlcpy((char *)station_config.sta.password, credentials.password,
                sizeof(station_config.sta.password));
        station_config.sta.threshold.authmode = credentials.password[0] == '\0'
                                                    ? WIFI_AUTH_OPEN
                                                    : WIFI_AUTH_WPA2_PSK;
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
        ESP_LOGI(TAG, "正在连接已保存的 Wi-Fi: %s", credentials.ssid);
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
