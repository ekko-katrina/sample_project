#ifndef MQTT_SETTINGS_H
#define MQTT_SETTINGS_H
#include "cJSON.h"
#include "myble.h"
#include "wifi_provisioning.h"
#include "tesla_link.h"

typedef enum {
    MQTT_GET_AMBIENT, MQTT_SET_AMBIENT, MQTT_GET_WIFI, MQTT_SCAN_WIFI,
    MQTT_CONFIGURE_WIFI, MQTT_CLEAR_WIFI, MQTT_GET_FIRMWARE, MQTT_START_FIRMWARE,
    MQTT_GET_TESLA, MQTT_CONFIGURE_TESLA, MQTT_STOP_TESLA
} mqtt_settings_operation_t;
typedef struct {
    mqtt_settings_operation_t operation;
    wifi_control_request_t wifi;
    tmy06_ota_request_t firmware;
    tmy06_ambient_settings_t ambient;
    bool has_enabled, has_effect, has_cooling, has_heating;
    char vin[18];
} mqtt_settings_request_t;
bool mqtt_settings_command_known(const char *command);
bool mqtt_settings_parse(const char *command, const cJSON *params, mqtt_settings_request_t *request);
bool mqtt_settings_execute(const mqtt_settings_request_t *request);
cJSON *mqtt_settings_event_json(const app_control_event_t *event);
const char *mqtt_settings_event_name(app_event_kind_t kind);
#endif
