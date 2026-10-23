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

typedef enum { WIFI_CONTROL_STATUS, WIFI_CONTROL_SCAN, WIFI_CONTROL_CONFIGURE, WIFI_CONTROL_CLEAR } wifi_control_operation_t;
typedef struct {
    wifi_control_operation_t operation;
    uint16_t operation_id;
    bool include_hidden, hidden, lock_bssid, disconnect;
    char ssid[33], password[65];
    uint8_t bssid[6];
} wifi_control_request_t;
bool wifi_provisioning_execute(uint16_t client, const wifi_control_request_t *request);

#ifdef __cplusplus
}
#endif

#endif
