#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_provisioning_start(void);
bool wifi_provisioning_is_connected(void);
bool wifi_provisioning_wait_connected(TickType_t timeout);
const char *wifi_provisioning_ap_ssid(void);

#ifdef __cplusplus
}
#endif

#endif
