#ifndef TESLA_LINK_H
#define TESLA_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*tesla_link_ble_notify_t)(uint16_t connection_handle,
                                        const uint8_t *frame,
                                        size_t length);

esp_err_t tesla_link_start(void);
void tesla_link_set_ble_notify_handler(tesla_link_ble_notify_t handler);
bool tesla_link_handle_ble_frame(uint16_t connection_handle,
                                 const uint8_t *frame,
                                 size_t length);
void tesla_link_handle_phone_disconnect(uint16_t connection_handle);
typedef enum { TESLA_CONTROL_STATUS, TESLA_CONTROL_CONFIGURE, TESLA_CONTROL_STOP } tesla_control_operation_t;
bool tesla_link_execute(uint16_t client, tesla_control_operation_t operation, const char *vin);

#endif
