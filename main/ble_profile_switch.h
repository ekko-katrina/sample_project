#ifndef BLE_PROFILE_SWITCH_H
#define BLE_PROFILE_SWITCH_H

#include "tmy06_protocol.h"

ble_sim_profile_t ble_profile_load(void);
void ble_profile_switch_button_start(ble_sim_profile_t active_profile);

#endif
