#include "tmy05_protocol.h"

#define TMY05_FRAME_HEADER_HIGH 0xA5U
#define TMY05_FRAME_HEADER_LOW 0x5AU
#define TMY05_FRAME_TAIL 0xAAU
#define TMY05_APP_DATA_LENGTH 0x06U
#define TMY05_STATUS_DATA_LENGTH 0x09U
#define FRIDGE_STATUS_DATA_LENGTH 0x0BU
#define TMY05_STATUS_COMMAND 0x00U
#define MODE_MASK(mode) (1U << (unsigned int)(mode))

static const ble_sim_profile_info_t g_profiles[BLE_SIM_PROFILE_COUNT] = {
    [BLE_SIM_PROFILE_LEGACY_Y05] = {
        .id = BLE_SIM_PROFILE_LEGACY_Y05,
        .display_name = "旧版 Y05",
        .device_name_prefix = "Aurora Y05_",
        .model_id = 0x04,
        .write_uuid = 0xFFF6,
        .notify_uuid = 0xFFF7,
        .is_legacy = true,
        .has_child_lock = true,
        .has_voltage = false,
        .supported_mode_mask = MODE_MASK(TMY05_MODE_COOLING) |
                               MODE_MASK(TMY05_MODE_HEATING),
        .maximum_fault_code = 0x03,
    },
    [BLE_SIM_PROFILE_NEW_Y17] = {
        .id = BLE_SIM_PROFILE_NEW_Y17,
        .display_name = "新版 Y17",
        .device_name_prefix = "GLACIER Y_",
        .model_id = 0x03,
        .write_uuid = 0xFFF1,
        .notify_uuid = 0xFFF2,
        .is_legacy = false,
        .has_child_lock = false,
        .has_voltage = true,
        .supported_mode_mask = MODE_MASK(TMY05_MODE_COOLING) |
                               MODE_MASK(TMY05_MODE_MAX) |
                               MODE_MASK(TMY05_MODE_ECO) |
                               MODE_MASK(TMY05_MODE_HEATING),
        .maximum_fault_code = 0x07,
    },
    [BLE_SIM_PROFILE_NEW_Y16] = {
        .id = BLE_SIM_PROFILE_NEW_Y16,
        .display_name = "新版 Y16",
        .device_name_prefix = "GLACIER Y_",
        .model_id = 0x02,
        .write_uuid = 0xFFF1,
        .notify_uuid = 0xFFF2,
        .is_legacy = false,
        .has_child_lock = false,
        .has_voltage = true,
        .supported_mode_mask = MODE_MASK(TMY05_MODE_COOLING) |
                               MODE_MASK(TMY05_MODE_MAX) |
                               MODE_MASK(TMY05_MODE_ECO),
        .maximum_fault_code = 0x07,
    },
    [BLE_SIM_PROFILE_NEW_TM310] = {
        .id = BLE_SIM_PROFILE_NEW_TM310,
        .display_name = "新版 TM-310",
        .device_name_prefix = "GLACIER 3_",
        .model_id = 0x01,
        .write_uuid = 0xFFF1,
        .notify_uuid = 0xFFF2,
        .is_legacy = false,
        .has_child_lock = false,
        .has_voltage = true,
        .supported_mode_mask = MODE_MASK(TMY05_MODE_COOLING) |
                               MODE_MASK(TMY05_MODE_MAX) |
                               MODE_MASK(TMY05_MODE_ECO) |
                               MODE_MASK(TMY05_MODE_HEATING),
        .maximum_fault_code = 0x07,
    },
    [BLE_SIM_PROFILE_NEW_Y05] = {
        .id = BLE_SIM_PROFILE_NEW_Y05,
        .display_name = "新版 Y05",
        .device_name_prefix = "AURORA Y05_",
        .model_id = 0x04,
        .write_uuid = 0xFFF1,
        .notify_uuid = 0xFFF2,
        .is_legacy = false,
        .has_child_lock = true,
        .has_voltage = false,
        .supported_mode_mask = MODE_MASK(TMY05_MODE_COOLING) |
                               MODE_MASK(TMY05_MODE_HEATING),
        .maximum_fault_code = 0x03,
    },
};

const ble_sim_profile_info_t *ble_sim_profile_info(ble_sim_profile_t profile)
{
    if ((unsigned int)profile >= BLE_SIM_PROFILE_COUNT) {
        return &g_profiles[BLE_SIM_PROFILE_LEGACY_Y05];
    }
    return &g_profiles[profile];
}

ble_sim_profile_t ble_sim_profile_next(ble_sim_profile_t profile)
{
    if ((unsigned int)profile >= BLE_SIM_PROFILE_COUNT - 1U) {
        return BLE_SIM_PROFILE_LEGACY_Y05;
    }
    return (ble_sim_profile_t)(profile + 1);
}

void fridge_protocol_make_initial_state(ble_sim_profile_t profile,
                                        tmy05_state_t *state)
{
    (void)profile;
    if (state == NULL) {
        return;
    }

    *state = (tmy05_state_t) {
        .power = TMY05_POWER_ON,
        .unit = TMY05_UNIT_CELSIUS,
        .mode = TMY05_MODE_COOLING,
        .child_lock = TMY05_CHILD_LOCK_OFF,
        .fault_code = TMY05_FAULT_NONE,
        .target_temperature = -5,
        .actual_temperature = 3,
        .actual_voltage_centi_volts = 1265,
    };
}

bool fridge_protocol_validate_state(ble_sim_profile_t profile,
                                    const tmy05_state_t *state)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    if (state == NULL || state->power > TMY05_POWER_ON ||
        state->unit > TMY05_UNIT_FAHRENHEIT ||
        (unsigned int)state->mode > TMY05_MODE_HEATING ||
        (info->supported_mode_mask & MODE_MASK(state->mode)) == 0U ||
        state->child_lock > TMY05_CHILD_LOCK_ON ||
        state->fault_code > info->maximum_fault_code) {
        return false;
    }

    return info->has_child_lock || state->child_lock == TMY05_CHILD_LOCK_OFF;
}

uint8_t tmy05_protocol_checksum(const uint8_t *data, size_t length)
{
    uint8_t checksum = 0;

    if (data == NULL) {
        return 0;
    }

    for (size_t i = 0; i < length; ++i) {
        checksum ^= data[i];
    }

    return checksum;
}

bool tmy05_protocol_validate_frame(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < 6U) {
        return false;
    }

    if (frame[0] != TMY05_FRAME_HEADER_HIGH ||
        frame[1] != TMY05_FRAME_HEADER_LOW ||
        frame[length - 1U] != TMY05_FRAME_TAIL) {
        return false;
    }

    if ((size_t)frame[2] + 3U != length) {
        return false;
    }

    return frame[length - 2U] == tmy05_protocol_checksum(frame, length - 2U);
}

static bool apply_control_value(const ble_sim_profile_info_t *info,
                                tmy05_command_t command,
                                uint8_t control,
                                tmy05_state_t *state)
{
    uint8_t value;

    switch (command) {
    case TMY05_COMMAND_SET_POWER:
        value = (control >> 6U) & 0x03U;
        if (value > TMY05_POWER_ON) {
            return false;
        }
        state->power = (tmy05_power_t)value;
        return true;

    case TMY05_COMMAND_SET_UNIT:
        value = (control >> 4U) & 0x03U;
        if (value > TMY05_UNIT_FAHRENHEIT) {
            return false;
        }
        state->unit = (tmy05_temperature_unit_t)value;
        return true;

    case TMY05_COMMAND_SET_MODE:
        if (info->has_child_lock) {
            value = (control >> 2U) & 0x03U;
            if (value > 1U) {
                return false;
            }
            state->mode = value == 0U ? TMY05_MODE_COOLING : TMY05_MODE_HEATING;
        } else {
            value = control & 0x0FU;
            if (value > TMY05_MODE_HEATING ||
                (info->supported_mode_mask & MODE_MASK(value)) == 0U) {
                return false;
            }
            state->mode = (tmy05_mode_t)value;
        }
        return true;

    case TMY05_COMMAND_SET_CHILD_LOCK:
        if (!info->has_child_lock) {
            return false;
        }
        value = control & 0x03U;
        if (value > TMY05_CHILD_LOCK_ON) {
            return false;
        }
        state->child_lock = (tmy05_child_lock_t)value;
        return true;

    default:
        return false;
    }
}

bool fridge_protocol_apply_app_frame(ble_sim_profile_t profile,
                                     const uint8_t *frame,
                                     size_t length,
                                     tmy05_state_t *state,
                                     tmy05_command_t *command)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    if (state == NULL || frame == NULL || length != TMY05_APP_FRAME_SIZE ||
        frame[2] != TMY05_APP_DATA_LENGTH ||
        !tmy05_protocol_validate_frame(frame, length)) {
        return false;
    }

    tmy05_command_t received_command = (tmy05_command_t)frame[3];
    bool accepted = false;

    switch (received_command) {
    case TMY05_COMMAND_GET_STATE:
        accepted = true;
        break;

    case TMY05_COMMAND_SET_POWER:
    case TMY05_COMMAND_SET_MODE:
    case TMY05_COMMAND_SET_UNIT:
    case TMY05_COMMAND_SET_CHILD_LOCK:
        accepted = apply_control_value(info, received_command, frame[4], state);
        break;

    case TMY05_COMMAND_SET_TARGET_TEMPERATURE:
        state->target_temperature = (int16_t)(((uint16_t)frame[5] << 8U) |
                                               (uint16_t)frame[6]);
        accepted = true;
        break;

    default:
        break;
    }

    if (accepted && command != NULL) {
        *command = received_command;
    }

    return accepted;
}

static uint8_t build_control(const ble_sim_profile_info_t *info,
                             const tmy05_state_t *state)
{
    uint8_t control = ((uint8_t)state->power << 6U) |
                      ((uint8_t)state->unit << 4U);
    if (info->has_child_lock) {
        uint8_t y05_mode = state->mode == TMY05_MODE_HEATING ? 1U : 0U;
        control |= (uint8_t)(y05_mode << 2U) | (uint8_t)state->child_lock;
    } else {
        control |= (uint8_t)state->mode;
    }
    return control;
}

size_t fridge_protocol_build_status_frame(ble_sim_profile_t profile,
                                          const tmy05_state_t *state,
                                          uint8_t *frame,
                                          size_t capacity)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    size_t frame_size = info->has_voltage ? FRIDGE_STATUS_FRAME_SIZE
                                          : TMY05_STATUS_FRAME_SIZE;
    if (!fridge_protocol_validate_state(profile, state) || frame == NULL ||
        capacity < frame_size) {
        return 0;
    }

    uint16_t target = (uint16_t)state->target_temperature;
    uint16_t actual = (uint16_t)state->actual_temperature;

    frame[0] = TMY05_FRAME_HEADER_HIGH;
    frame[1] = TMY05_FRAME_HEADER_LOW;
    frame[2] = info->has_voltage ? FRIDGE_STATUS_DATA_LENGTH
                                 : TMY05_STATUS_DATA_LENGTH;
    frame[3] = TMY05_STATUS_COMMAND;
    frame[4] = build_control(info, state);
    frame[5] = state->fault_code;
    frame[6] = (uint8_t)(target >> 8U);
    frame[7] = (uint8_t)target;
    frame[8] = (uint8_t)(actual >> 8U);
    frame[9] = (uint8_t)actual;

    if (info->has_voltage) {
        frame[10] = (uint8_t)(state->actual_voltage_centi_volts >> 8U);
        frame[11] = (uint8_t)state->actual_voltage_centi_volts;
        frame[12] = tmy05_protocol_checksum(frame, 12U);
        frame[13] = TMY05_FRAME_TAIL;
    } else {
        frame[10] = tmy05_protocol_checksum(frame, 10U);
        frame[11] = TMY05_FRAME_TAIL;
    }

    return frame_size;
}

bool tmy05_protocol_apply_app_frame(const uint8_t *frame,
                                    size_t length,
                                    tmy05_state_t *state,
                                    tmy05_command_t *command)
{
    return fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_LEGACY_Y05,
                                           frame, length, state, command);
}

size_t tmy05_protocol_build_status_frame(const tmy05_state_t *state,
                                         uint8_t *frame,
                                         size_t capacity)
{
    return fridge_protocol_build_status_frame(BLE_SIM_PROFILE_LEGACY_Y05,
                                              state, frame, capacity);
}
