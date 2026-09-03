#ifndef TMY05_PROTOCOL_H
#define TMY05_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TMY05_APP_FRAME_SIZE 9U
#define TMY05_STATUS_FRAME_SIZE 12U
#define FRIDGE_STATUS_FRAME_SIZE 14U
#define FRIDGE_MAX_STATUS_FRAME_SIZE FRIDGE_STATUS_FRAME_SIZE

typedef enum {
    BLE_SIM_PROFILE_LEGACY_Y05 = 0,
    BLE_SIM_PROFILE_NEW_Y17,
    BLE_SIM_PROFILE_NEW_Y16,
    BLE_SIM_PROFILE_NEW_TM310,
    BLE_SIM_PROFILE_NEW_Y05,
    BLE_SIM_PROFILE_COUNT,
} ble_sim_profile_t;

typedef struct {
    ble_sim_profile_t id;
    const char *display_name;
    const char *device_name_prefix;
    uint8_t model_id;
    uint16_t write_uuid;
    uint16_t notify_uuid;
    bool is_legacy;
    bool has_child_lock;
    bool has_voltage;
    uint8_t supported_mode_mask;
    uint8_t maximum_fault_code;
} ble_sim_profile_info_t;

typedef enum {
    TMY05_POWER_OFF = 0,
    TMY05_POWER_ON = 1,
} tmy05_power_t;

typedef enum {
    TMY05_UNIT_CELSIUS = 0,
    TMY05_UNIT_FAHRENHEIT = 1,
} tmy05_temperature_unit_t;

/* Logical modes. New Y16/Y17/TM-310 use these values directly on air.
 * Y05 maps HEATING to its wire value 1 in bits 3..2. */
typedef enum {
    TMY05_MODE_COOLING = 0,
    TMY05_MODE_MAX = 1,
    TMY05_MODE_ECO = 2,
    TMY05_MODE_HEATING = 3,
} tmy05_mode_t;

typedef enum {
    TMY05_CHILD_LOCK_OFF = 0,
    TMY05_CHILD_LOCK_ON = 1,
} tmy05_child_lock_t;

typedef enum {
    TMY05_FAULT_NONE = 0x00,
    TMY05_FAULT_NTC_SHORT = 0x01,
    TMY05_FAULT_NTC_OPEN = 0x02,
    TMY05_FAULT_HIGH_TEMPERATURE = 0x03,
} tmy05_fault_t;

typedef struct {
    tmy05_power_t power;
    tmy05_temperature_unit_t unit;
    tmy05_mode_t mode;
    tmy05_child_lock_t child_lock;
    uint8_t fault_code;
    int16_t target_temperature;
    int16_t actual_temperature;
    uint16_t actual_voltage_centi_volts;
} tmy05_state_t;

typedef enum {
    TMY05_COMMAND_GET_STATE = 0x01,
    TMY05_COMMAND_SET_POWER = 0x02,
    TMY05_COMMAND_SET_MODE = 0x03,
    TMY05_COMMAND_SET_UNIT = 0x04,
    TMY05_COMMAND_SET_TARGET_TEMPERATURE = 0x05,
    TMY05_COMMAND_SET_CHILD_LOCK = 0x06,
} tmy05_command_t;

const ble_sim_profile_info_t *ble_sim_profile_info(ble_sim_profile_t profile);
ble_sim_profile_t ble_sim_profile_next(ble_sim_profile_t profile);

void fridge_protocol_make_initial_state(ble_sim_profile_t profile,
                                        tmy05_state_t *state);
bool fridge_protocol_validate_state(ble_sim_profile_t profile,
                                    const tmy05_state_t *state);

uint8_t tmy05_protocol_checksum(const uint8_t *data, size_t length);
bool tmy05_protocol_validate_frame(const uint8_t *frame, size_t length);

bool fridge_protocol_apply_app_frame(ble_sim_profile_t profile,
                                     const uint8_t *frame,
                                     size_t length,
                                     tmy05_state_t *state,
                                     tmy05_command_t *command);
size_t fridge_protocol_build_status_frame(ble_sim_profile_t profile,
                                          const tmy05_state_t *state,
                                          uint8_t *frame,
                                          size_t capacity);

/* Compatibility wrappers retain the original legacy-Y05 protocol API. */
bool tmy05_protocol_apply_app_frame(const uint8_t *frame,
                                    size_t length,
                                    tmy05_state_t *state,
                                    tmy05_command_t *command);
size_t tmy05_protocol_build_status_frame(const tmy05_state_t *state,
                                         uint8_t *frame,
                                         size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
