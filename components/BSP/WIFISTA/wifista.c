#include "wifista.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

void wifista_event_handler(void *event_handler_arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_CONNECTED)
        {
            printf("connected\n");
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            printf("disconnected\n");
            esp_wifi_stop();
        }
    }

    if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            esp_netif_ip_info_t *event = (esp_netif_ip_info_t *)event_data;
            printf(IPSTR "\n", IP2STR(&event->ip));
        }
    }
}

void wifista_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &wifista_event_handler,
        NULL);
    esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifista_event_handler,
        NULL);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_wifi_set_mode(WIFI_MODE_STA);

    wifi_config_t wifista_config = {
        .sta = {
            .ssid = DEFAULT_SSID,
            .password = DEFAULT_PWD,
        }};
    esp_wifi_set_config(WIFI_IF_STA, &wifista_config);

    esp_wifi_start();
}
