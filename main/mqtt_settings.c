#include "mqtt_settings.h"
#include <stdio.h>
#include <string.h>

static const char *const commands[] = {
    "GET_AMBIENT_LIGHT", "SET_AMBIENT_LIGHT", "GET_WIFI_STATUS", "SCAN_WIFI",
    "CONFIGURE_WIFI", "CLEAR_WIFI", "GET_FIRMWARE_VERSION", "START_FIRMWARE_UPDATE",
    "GET_TESLA_STATUS", "CONFIGURE_TESLA", "STOP_TESLA"
};
static const cJSON *field(const cJSON *object, const char *key) { return cJSON_GetObjectItemCaseSensitive(object, key); }
static bool integer(const cJSON *value, int low, int high, int *out)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < low || value->valuedouble > high) return false;
    int n = (int)value->valuedouble;
    if ((double)n != value->valuedouble) return false;
    *out = n; return true;
}
static bool boolean(const cJSON *params, const char *key, bool fallback, bool *out)
{
    const cJSON *value = field(params, key);
    if (value == NULL) { *out = fallback; return true; }
    if (!cJSON_IsBool(value)) return false;
    *out = cJSON_IsTrue(value); return true;
}
static bool string_value(const cJSON *value, char *out, size_t capacity)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL || strlen(value->valuestring) >= capacity) return false;
    strlcpy(out, value->valuestring, capacity); return true;
}
static bool color(const cJSON *value, uint8_t rgb[3])
{
    const char *keys[] = { "r", "g", "b" };
    if (!cJSON_IsObject(value)) return false;
    for (size_t i = 0; i < 3; ++i) { int n; if (!integer(field(value, keys[i]), 0, 255, &n)) return false; rgb[i] = n; }
    return true;
}
static bool version(const cJSON *value, tmy06_ota_request_t *out)
{
    if (!cJSON_IsString(value) || value->valuestring == NULL) return false;
    const char *p = value->valuestring;
    uint8_t parts[3];
    for (size_t i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') return false;
        unsigned n = 0;
        do { n = n * 10U + (unsigned)(*p++ - '0'); if (n > 255U) return false; } while (*p >= '0' && *p <= '9');
        parts[i] = n;
        if (i < 2 ? *p++ != '.' : *p != '\0') return false;
    }
    out->major = parts[0]; out->minor = parts[1]; out->patch = parts[2]; return true;
}
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool bssid(const cJSON *value, uint8_t out[6])
{
    if (!cJSON_IsString(value) || value->valuestring == NULL || strlen(value->valuestring) != 17U) return false;
    for (size_t i = 0; i < 6; ++i) {
        const char *p = value->valuestring + i * 3U;
        int a = hex_digit(p[0]), b = hex_digit(p[1]);
        if (a < 0 || b < 0 || (i < 5 && p[2] != ':')) return false;
        out[i] = (a << 4) | b;
    }
    return true;
}
static int operation_index(const char *command)
{
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) if (strcmp(command, commands[i]) == 0) return i;
    return -1;
}
bool mqtt_settings_command_known(const char *command) { return operation_index(command) >= 0; }

bool mqtt_settings_parse(const char *command, const cJSON *params, mqtt_settings_request_t *request)
{
    int index = operation_index(command), n;
    if (index < 0 || request == NULL || (params != NULL && !cJSON_IsObject(params))) return false;
    // Unknown additional fields are ignored, as in the basic MQTT commands.
    memset(request, 0, sizeof(*request)); request->operation = index;
    if (index == MQTT_GET_AMBIENT || index == MQTT_GET_FIRMWARE || index == MQTT_GET_TESLA || index == MQTT_STOP_TESLA) return true;
    if (index == MQTT_SET_AMBIENT) {
        const cJSON *enabled = field(params, "enabled"), *effect = field(params, "effect");
        const cJSON *cooling = field(params, "coolingColor"), *heating = field(params, "heatingColor");
        request->has_enabled = enabled != NULL; request->has_effect = effect != NULL;
        request->has_cooling = cooling != NULL; request->has_heating = heating != NULL;
        if (enabled != NULL) { if (!cJSON_IsBool(enabled)) return false; request->ambient.disabled = !cJSON_IsTrue(enabled); }
        if (effect != NULL) {
            if (!cJSON_IsString(effect)) return false;
            if (strcmp(effect->valuestring, "STEADY") == 0) request->ambient.effect = 0;
            else if (strcmp(effect->valuestring, "BREATHING") == 0) request->ambient.effect = 1;
            else return false;
        }
        return (enabled || effect || cooling || heating) && (!cooling || color(cooling, request->ambient.colors[0])) && (!heating || color(heating, request->ambient.colors[1]));
    }
    if (index >= MQTT_GET_WIFI && index <= MQTT_CLEAR_WIFI) {
        request->wifi.operation = index - MQTT_GET_WIFI;
        if (index == MQTT_SCAN_WIFI) return boolean(params, "includeHidden", false, &request->wifi.include_hidden);
        if (index == MQTT_CLEAR_WIFI) return boolean(params, "disconnect", true, &request->wifi.disconnect);
        if (index == MQTT_GET_WIFI) return true;
        if (!string_value(field(params, "ssid"), request->wifi.ssid, sizeof(request->wifi.ssid)) ||
            !string_value(field(params, "password"), request->wifi.password, sizeof(request->wifi.password)) ||
            !boolean(params, "hidden", false, &request->wifi.hidden) || !boolean(params, "lockBssid", false, &request->wifi.lock_bssid)) return false;
        if (!request->wifi.hidden && !request->wifi.ssid[0]) return false;
        size_t password_length = strlen(request->wifi.password);
        if (password_length != 0U && password_length < 8U) return false;
        if (password_length == 64U) for (size_t i = 0; i < 64U; ++i) if (hex_digit(request->wifi.password[i]) < 0) return false;
        const cJSON *address = field(params, "bssid");
        if (address && !bssid(address, request->wifi.bssid)) return false;
        if (request->wifi.lock_bssid) {
            bool nonzero = false; for (size_t i = 0; i < 6; ++i) nonzero |= request->wifi.bssid[i] != 0;
            if (!nonzero) return false;
        }
        return true;
    }
    if (index == MQTT_START_FIRMWARE) {
        if (!integer(field(params, "modelId"), 1, 255, &n) || !version(field(params, "version"), &request->firmware)) return false;
        request->firmware.model_id = n; request->firmware.command = TMY06_OTA_COMMAND_START; return true;
    }
    if (!string_value(field(params, "vin"), request->vin, sizeof(request->vin)) || strlen(request->vin) != 17U) return false;
    for (size_t i = 0; i < 17U; ++i) {
        char c = request->vin[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O' && c != 'Q'))) return false;
    }
    return true;
}

bool mqtt_settings_execute(const mqtt_settings_request_t *request)
{
    switch (request->operation) {
    case MQTT_GET_AMBIENT:
    case MQTT_SET_AMBIENT: {
        if (!ble_sim_profile_info(ble_sim_get_active_profile())->has_ambient_light) return false;
        if (request->operation == MQTT_GET_AMBIENT) { app_query_ambient_light(); return true; }
        tmy06_state_t state; if (!ble_tmy06_get_state(&state)) return false;
        if (request->has_enabled) state.ambient.disabled = request->ambient.disabled;
        if (request->has_effect) state.ambient.effect = request->ambient.effect;
        if (request->has_cooling) memcpy(state.ambient.colors[0], request->ambient.colors[0], 3U);
        if (request->has_heating) memcpy(state.ambient.colors[1], request->ambient.colors[1], 3U);
        return app_set_ambient_light(&state.ambient);
    }
    case MQTT_GET_WIFI: case MQTT_SCAN_WIFI: case MQTT_CONFIGURE_WIFI: case MQTT_CLEAR_WIFI: {
        static uint16_t local_operation = 0;
        wifi_control_request_t wifi = request->wifi;
        if (++local_operation == 0U) ++local_operation;
        wifi.operation_id = local_operation;
        return wifi_provisioning_execute(APP_REMOTE_CONNECTION_HANDLE, &wifi);
    }
    case MQTT_GET_FIRMWARE: app_query_firmware_version(); return true;
    case MQTT_START_FIRMWARE: return app_start_firmware_update(&request->firmware);
    case MQTT_GET_TESLA: return tesla_link_execute(APP_REMOTE_CONNECTION_HANDLE, TESLA_CONTROL_STATUS, NULL);
    case MQTT_CONFIGURE_TESLA: return tesla_link_execute(APP_REMOTE_CONNECTION_HANDLE, TESLA_CONTROL_CONFIGURE, request->vin);
    case MQTT_STOP_TESLA: return tesla_link_execute(APP_REMOTE_CONNECTION_HANDLE, TESLA_CONTROL_STOP, NULL);
    }
    return false;
}

static cJSON *rgb(const uint8_t color[3])
{
    cJSON *object = cJSON_CreateObject();
    cJSON_AddNumberToObject(object, "r", color[0]); cJSON_AddNumberToObject(object, "g", color[1]); cJSON_AddNumberToObject(object, "b", color[2]);
    return object;
}
const char *mqtt_settings_event_name(app_event_kind_t kind)
{
    static const char *const types[] = { "AMBIENT_LIGHT_STATUS", "FIRMWARE_VERSION", "FIRMWARE_UPDATE_STATUS", "WIFI_STATUS", "WIFI_SCAN_EVENT", "WIFI_CONFIGURE_EVENT", "WIFI_CLEAR_RESULT", "TESLA_STATUS", "TESLA_EVENT" };
    return (unsigned)kind < sizeof(types) / sizeof(types[0]) ? types[kind] : NULL;
}
cJSON *mqtt_settings_event_json(const app_control_event_t *event)
{
    const char *type = mqtt_settings_event_name(event->kind);
    if (type == NULL) return NULL;
    cJSON *root = cJSON_CreateObject(), *data = cJSON_CreateObject();
    if (!root || !data) { cJSON_Delete(root); cJSON_Delete(data); return NULL; }
    cJSON_AddStringToObject(root, "type", type); cJSON_AddItemToObject(root, "data", data);
    if (event->kind == APP_EVENT_AMBIENT) {
        cJSON_AddBoolToObject(data, "enabled", !event->data.ambient.disabled);
        cJSON_AddStringToObject(data, "effect", event->data.ambient.effect ? "BREATHING" : "STEADY");
        cJSON_AddItemToObject(data, "coolingColor", rgb(event->data.ambient.colors[0]));
        cJSON_AddItemToObject(data, "heatingColor", rgb(event->data.ambient.colors[1]));
    } else if (event->kind == APP_EVENT_FIRMWARE_VERSION) {
        char text[12]; snprintf(text, sizeof(text), "%u.%u.%u", event->data.firmware.major, event->data.firmware.minor, event->data.firmware.patch);
        cJSON_AddNumberToObject(data, "modelId", event->data.firmware.model_id); cJSON_AddStringToObject(data, "version", text);
    } else if (event->kind == APP_EVENT_FIRMWARE_STATUS) {
        const char *stages[] = { "PREPARING", "DOWNLOADING", "VERIFYING", "RESTARTING", "ERROR" };
        cJSON_AddStringToObject(data, "stage", event->data.firmware.stage <= 4 ? stages[event->data.firmware.stage] : "ERROR");
        cJSON_AddNumberToObject(data, "errorCode", event->data.firmware.error);
    } else if (event->kind >= APP_EVENT_WIFI_STATUS && event->kind <= APP_EVENT_WIFI_CLEAR) {
        cJSON_AddNumberToObject(data, "errorCode", event->data.wifi.error);
        if (event->kind == APP_EVENT_WIFI_STATUS || event->kind == APP_EVENT_WIFI_CLEAR) cJSON_AddNumberToObject(data, "state", event->data.wifi.state);
        if (event->kind == APP_EVENT_WIFI_CONFIGURE || event->kind == APP_EVENT_WIFI_CLEAR) cJSON_AddNumberToObject(data, "result", event->data.wifi.result);
        if (event->kind == APP_EVENT_WIFI_CONFIGURE) cJSON_AddNumberToObject(data, "stage", event->data.wifi.stage);
        if (event->kind == APP_EVENT_WIFI_SCAN) {
            const char *events[] = { "STARTED", "AP_FOUND", "FINISHED", "FAILED" };
            cJSON_AddStringToObject(data, "event", event->data.wifi.event <= 3 ? events[event->data.wifi.event] : "FAILED");
            if (event->data.wifi.event == 1) {
                char address[18]; snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", event->data.wifi.bssid[0], event->data.wifi.bssid[1], event->data.wifi.bssid[2], event->data.wifi.bssid[3], event->data.wifi.bssid[4], event->data.wifi.bssid[5]);
                cJSON_AddStringToObject(data, "bssid", address); cJSON_AddNumberToObject(data, "authMode", event->data.wifi.auth_mode);
            }
        }
        if (event->kind != APP_EVENT_WIFI_CLEAR) {
            cJSON_AddStringToObject(data, "ssid", event->data.wifi.ssid); cJSON_AddNumberToObject(data, "rssi", event->data.wifi.rssi);
        }
        if (event->kind == APP_EVENT_WIFI_STATUS || event->kind == APP_EVENT_WIFI_CONFIGURE) {
            char ip[16]; snprintf(ip, sizeof(ip), "%u.%u.%u.%u", event->data.wifi.ip[0], event->data.wifi.ip[1], event->data.wifi.ip[2], event->data.wifi.ip[3]);
            cJSON_AddStringToObject(data, "ip", ip);
        }
    } else {
        cJSON_AddBoolToObject(data, "configured", event->data.tesla.configured); cJSON_AddBoolToObject(data, "paired", event->data.tesla.paired);
        cJSON_AddNumberToObject(data, "connectionState", event->data.tesla.connection_state); cJSON_AddNumberToObject(data, "stage", event->data.tesla.stage);
        cJSON_AddNumberToObject(data, "errorCode", event->data.tesla.error); cJSON_AddStringToObject(data, "vin", event->data.tesla.vin);
    }
    return root;
}
