#ifndef APP_CONTROL_EVENT_H
#define APP_CONTROL_EVENT_H
#include "tmy06_protocol.h"

typedef enum {
    APP_EVENT_AMBIENT, APP_EVENT_FIRMWARE_VERSION, APP_EVENT_FIRMWARE_STATUS,
    APP_EVENT_WIFI_STATUS, APP_EVENT_WIFI_SCAN, APP_EVENT_WIFI_CONFIGURE,
    APP_EVENT_WIFI_CLEAR, APP_EVENT_TESLA_STATUS, APP_EVENT_TESLA_EVENT
} app_event_kind_t;

/* Business data shared by transports; no packet headers/checksums or frames. */
typedef struct {
    app_event_kind_t kind;
    uint16_t operation_id;
    union {
        tmy06_ambient_settings_t ambient;
        struct { uint8_t model_id, major, minor, patch, stage, error; } firmware;
        struct {
            uint8_t state, stage, result, error, event, auth_mode;
            int8_t rssi;
            char ssid[33];
            uint8_t ip[4], bssid[6];
        } wifi;
        struct {
            bool configured, paired;
            uint8_t connection_state, stage, error;
            char vin[18];
        } tesla;
    } data;
} app_control_event_t;
#endif
