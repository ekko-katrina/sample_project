#ifndef __MYBLE_H_
#define __MYBLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "tmy06_protocol.h"
#include "app_control_event.h"

typedef bool (*ble_ota_start_handler_t)(const tmy06_ota_request_t *request);
typedef void (*ble_state_change_handler_t)(const tmy06_state_t *before,
                                           const tmy06_state_t *after);

void start_advertising(void);

void ble_init(ble_sim_profile_t profile);

void ble_ota_set_start_handler(ble_ota_start_handler_t handler);

/* MQTT supplies its resolved SN (including the development fallback), so BLE
 * identity and the broker always use the same device identity. */
void ble_set_mqtt_sn(const char *sn);

void ble_ota_report_status(tmy06_ota_status_t status, uint8_t information);

ble_sim_profile_t ble_sim_get_active_profile(void);

bool ble_tmy06_get_state(tmy06_state_t *state);

bool ble_tmy06_set_state(const tmy06_state_t *state);

bool ble_tmy06_set_actual_temperature(int16_t temperature);

bool ble_tmy06_set_fault(uint8_t fault_code);

bool ble_fridge_set_actual_voltage(uint16_t centi_volts);

void ble_tmy06_set_state_change_handler(ble_state_change_handler_t handler);

void ble_phone_advertising_pause(void);
void ble_phone_advertising_resume(void);

/* MQTT routing token, never an actual BLE link. */
#define APP_REMOTE_CONNECTION_HANDLE 0xFFFEU
static inline const char *app_control_transport(uint16_t handle)
{
    return handle == APP_REMOTE_CONNECTION_HANDLE ? "MQTT" : "BLE";
}
static inline const char *app_control_log_tag(uint16_t handle)
{
    return handle == APP_REMOTE_CONNECTION_HANDLE ? "FRIDGE-MQTT" : "FRIDGE-BLE";
}
typedef void (*app_remote_notify_t)(const app_control_event_t *event);
void app_set_remote_notify_handler(app_remote_notify_t handler);
void app_report_remote_event(const app_control_event_t *event);
void app_query_firmware_version(void);
bool app_start_firmware_update(const tmy06_ota_request_t *request);
bool app_set_ambient_light(const tmy06_ambient_settings_t *settings);
void app_query_ambient_light(void);

#endif
