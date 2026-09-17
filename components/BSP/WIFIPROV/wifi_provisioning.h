#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_provisioning_ble_notify_t)(uint16_t connection_handle,
                                                const uint8_t *frame,
                                                size_t length);

esp_err_t wifi_provisioning_start(void);
bool wifi_provisioning_is_connected(void);
bool wifi_provisioning_wait_connected(TickType_t timeout);
const char *wifi_provisioning_ap_ssid(void);
void wifi_provisioning_set_ble_notify_handler(
    wifi_provisioning_ble_notify_t handler);
bool wifi_provisioning_handle_ble_frame(uint16_t connection_handle,
                                        const uint8_t *frame,
                                        size_t length);
void wifi_provisioning_handle_ble_disconnect(uint16_t connection_handle);

#ifdef __cplusplus
}
#endif

#endif
