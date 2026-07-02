#include "wifiap.h"
#include "esp_wifi.h"
#include <string.h>

void wifista_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_AP_STACONNECTED)
        {
            printf("connected\n");
        }
        else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
        {
            printf("disconnected\n");
        }
        
    }
}

void wifiap_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifista_event_handler,
        NULL);
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t wifiap_config = {
        .ap = {
            .ssid = DEFAULT_SSID,
            .ssid_len = strlen(DEFAULT_SSID),
            .password = DEFAULT_PWD,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        }};
    esp_wifi_set_config(ESP_IF_WIFI_AP, &wifiap_config);

    esp_wifi_start();
}