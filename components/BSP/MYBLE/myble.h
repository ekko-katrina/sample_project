#ifndef __MYBLE_H_
#define __MYBLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "tmy05_protocol.h"

void start_advertising(void);

void ble_init(void);

bool ble_tmy05_get_state(tmy05_state_t *state);

bool ble_tmy05_set_state(const tmy05_state_t *state);

bool ble_tmy05_set_actual_temperature(int16_t temperature);

bool ble_tmy05_set_fault(uint8_t fault_code);

#endif
