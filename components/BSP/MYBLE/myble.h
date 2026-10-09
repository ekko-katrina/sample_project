#ifndef __MYBLE_H_
#define __MYBLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "tmy05_protocol.h"

typedef bool (*ble_ota_start_handler_t)(const tmy05_ota_request_t *request);
typedef void (*ble_state_change_handler_t)(const tmy05_state_t *before,
                                           const tmy05_state_t *after);

void start_advertising(void);

void ble_init(ble_sim_profile_t profile);

void ble_ota_set_start_handler(ble_ota_start_handler_t handler);

void ble_ota_report_status(tmy05_ota_status_t status, uint8_t information);

ble_sim_profile_t ble_sim_get_active_profile(void);

bool ble_tmy05_get_state(tmy05_state_t *state);

bool ble_tmy05_set_state(const tmy05_state_t *state);

bool ble_tmy05_set_actual_temperature(int16_t temperature);

bool ble_tmy05_set_fault(uint8_t fault_code);

bool ble_fridge_set_actual_voltage(uint16_t centi_volts);

void ble_tmy05_set_state_change_handler(ble_state_change_handler_t handler);

void ble_phone_advertising_pause(void);
void ble_phone_advertising_resume(void);

#endif
