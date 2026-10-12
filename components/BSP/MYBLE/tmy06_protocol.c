#include "tmy06_protocol.h"

#define TMY06_FRAME_HEADER_HIGH 0xA5U
#define TMY06_FRAME_HEADER_LOW 0x5AU
#define TMY06_FRAME_TAIL 0xAAU
#define TMY06_APP_DATA_LENGTH 0x06U
#define TMY06_STATUS_DATA_LENGTH 0x10U
#define FRIDGE_STATUS_DATA_LENGTH 0x0BU
#define TMY06_STATUS_COMMAND 0x00U
#define MODE_MASK(mode) (1U << (unsigned int)(mode))

static const ble_sim_profile_info_t g_profiles[BLE_SIM_PROFILE_COUNT] = {
    [BLE_SIM_PROFILE_LEGACY_Y06] = {
        .id = BLE_SIM_PROFILE_LEGACY_Y06,
        .display_name = "旧版 Y06",
        .has_ambient_light = true,
        .device_name_prefix = "Aurora Y06_",
        .model_id = 0x04,
        .write_uuid = 0xFFF7,
        .notify_uuid = 0xFFF7,
        .is_legacy = true,
        .has_child_lock = true,
        .has_voltage = false,
        .supported_mode_mask = MODE_MASK(TMY06_MODE_COOLING) |
                               MODE_MASK(TMY06_MODE_HEATING),
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
        .supported_mode_mask = MODE_MASK(TMY06_MODE_COOLING) |
                               MODE_MASK(TMY06_MODE_MAX) |
                               MODE_MASK(TMY06_MODE_ECO) |
                               MODE_MASK(TMY06_MODE_HEATING),
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
        .supported_mode_mask = MODE_MASK(TMY06_MODE_COOLING) |
                               MODE_MASK(TMY06_MODE_MAX) |
                               MODE_MASK(TMY06_MODE_ECO),
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
        .supported_mode_mask = MODE_MASK(TMY06_MODE_COOLING) |
                               MODE_MASK(TMY06_MODE_MAX) |
                               MODE_MASK(TMY06_MODE_ECO) |
                               MODE_MASK(TMY06_MODE_HEATING),
        .maximum_fault_code = 0x07,
    },
    [BLE_SIM_PROFILE_NEW_Y06] = {
        .id = BLE_SIM_PROFILE_NEW_Y06,
        .display_name = "新版 Y06",
        .has_ambient_light = true,
        .device_name_prefix = "AURORA Y06_",
        .model_id = 0x04,
        .write_uuid = 0xFFF1,
        .notify_uuid = 0xFFF2,
        .is_legacy = false,
        .has_child_lock = true,
        .has_voltage = false,
        .supported_mode_mask = MODE_MASK(TMY06_MODE_COOLING) |
                               MODE_MASK(TMY06_MODE_HEATING),
        .maximum_fault_code = 0x03,
    },
};

const ble_sim_profile_info_t *ble_sim_profile_info(ble_sim_profile_t profile)
{
    if ((unsigned int)profile >= BLE_SIM_PROFILE_COUNT) {
        return &g_profiles[BLE_SIM_PROFILE_LEGACY_Y06];
    }
    return &g_profiles[profile];
}

ble_sim_profile_t ble_sim_profile_next(ble_sim_profile_t profile)
{
    if ((unsigned int)profile >= BLE_SIM_PROFILE_COUNT - 1U) {
        return BLE_SIM_PROFILE_LEGACY_Y06;
    }
    return (ble_sim_profile_t)(profile + 1);
}

void fridge_protocol_make_initial_state(ble_sim_profile_t profile,
                                        tmy06_state_t *state)
{
    (void)profile;
    if (state == NULL) {
        return;
    }

    *state = (tmy06_state_t) {
        .power = TMY06_POWER_ON,
        .unit = TMY06_UNIT_CELSIUS,
        .mode = TMY06_MODE_COOLING,
        .child_lock = TMY06_CHILD_LOCK_OFF,
        .fault_code = TMY06_FAULT_NONE,
        .target_temperature = -5,
        .actual_temperature = 3,
        .actual_voltage_centi_volts = 1267,
        .ambient = { .effect = 0, .colors = {{0, 0, 255}, {255, 0, 0}} },
    };
}

bool fridge_protocol_validate_state(ble_sim_profile_t profile,
                                    const tmy06_state_t *state)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    if (state == NULL || state->power > TMY06_POWER_ON ||
        state->unit > TMY06_UNIT_FAHRENHEIT ||
        (unsigned int)state->mode > TMY06_MODE_HEATING ||
        (info->supported_mode_mask & MODE_MASK(state->mode)) == 0U ||
        state->child_lock > TMY06_CHILD_LOCK_ON ||
        state->fault_code > info->maximum_fault_code ||
        (info->has_ambient_light && state->ambient.effect > 1U)) {
        return false;
    }

    return info->has_child_lock || state->child_lock == TMY06_CHILD_LOCK_OFF;
}

static int32_t divide_round_nearest(int32_t numerator, int32_t denominator)
{
    if (numerator >= 0) {
        return (numerator + denominator / 2) / denominator;
    }
    return -((-numerator + denominator / 2) / denominator);
}

static bool convert_temperature(int16_t input, tmy06_temperature_unit_t from,
                                int16_t *output)
{
    int32_t converted;
    if (from == TMY06_UNIT_CELSIUS) {
        converted = divide_round_nearest((int32_t)input * 9, 5) + 32;
    } else {
        converted = divide_round_nearest(((int32_t)input - 32) * 5, 9);
    }
    if (converted < INT16_MIN || converted > INT16_MAX) {
        return false;
    }
    *output = (int16_t)converted;
    return true;
}

bool fridge_protocol_change_unit(tmy06_state_t *state,
                                 tmy06_temperature_unit_t unit)
{
    if (state == NULL || unit > TMY06_UNIT_FAHRENHEIT ||
        state->unit > TMY06_UNIT_FAHRENHEIT) {
        return false;
    }
    if (state->unit == unit) {
        return true;
    }

    int16_t target;
    int16_t actual;
    if (!convert_temperature(state->target_temperature, state->unit, &target) ||
        !convert_temperature(state->actual_temperature, state->unit, &actual)) {
        return false;
    }
    state->target_temperature = target;
    state->actual_temperature = actual;
    state->unit = unit;
    return true;
}

uint8_t tmy06_protocol_checksum(const uint8_t *data, size_t length)
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

bool tmy06_protocol_validate_frame(const uint8_t *frame, size_t length)
{
    if (frame == NULL || length < 6U) {
        return false;
    }

    if (frame[0] != TMY06_FRAME_HEADER_HIGH ||
        frame[1] != TMY06_FRAME_HEADER_LOW ||
        frame[length - 1U] != TMY06_FRAME_TAIL) {
        return false;
    }

    if ((size_t)frame[2] + 3U != length) {
        return false;
    }

    return frame[length - 2U] == tmy06_protocol_checksum(frame, length - 2U);
}

bool tmy06_protocol_parse_ota_request(const uint8_t *frame,
                                      size_t length,
                                      tmy06_ota_request_t *request)
{
    if (frame == NULL || !tmy06_protocol_validate_frame(frame, length)) {
        return false;
    }

    tmy06_ota_request_t parsed = {
        .command = (tmy06_ota_command_t)frame[3],
    };
    if (parsed.command == TMY06_OTA_COMMAND_QUERY_VERSION) {
        if (length != TMY06_OTA_QUERY_COMMAND_FRAME_SIZE ||
            frame[2] != 0x04U || frame[4] != 0x00U) {
            return false;
        }
    } else if (parsed.command == TMY06_OTA_COMMAND_START) {
        if (length != TMY06_OTA_TARGET_COMMAND_FRAME_SIZE ||
            frame[2] != 0x07U) {
            return false;
        }
        parsed.model_id = frame[4];
        parsed.major = frame[5];
        parsed.minor = frame[6];
        parsed.patch = frame[7];
    } else {
        return false;
    }

    if (request != NULL) {
        *request = parsed;
    }
    return true;
}

static size_t build_ota_response(uint8_t command,
                                 const uint8_t *payload,
                                 size_t payload_length,
                                 uint8_t *frame,
                                 size_t capacity)
{
    size_t frame_length = payload_length + 6U;
    if (frame == NULL || payload == NULL || payload_length > 252U ||
        capacity < frame_length) {
        return 0U;
    }

    frame[0] = TMY06_FRAME_HEADER_HIGH;
    frame[1] = TMY06_FRAME_HEADER_LOW;
    frame[2] = (uint8_t)(payload_length + 3U);
    frame[3] = command;
    for (size_t i = 0; i < payload_length; ++i) {
        frame[4U + i] = payload[i];
    }
    frame[frame_length - 2U] = tmy06_protocol_checksum(frame,
                                                       frame_length - 2U);
    frame[frame_length - 1U] = TMY06_FRAME_TAIL;
    return frame_length;
}

size_t tmy06_protocol_build_ota_version_frame(uint8_t model_id,
                                              uint8_t major,
                                              uint8_t minor,
                                              uint8_t patch,
                                              uint8_t *frame,
                                              size_t capacity)
{
    const uint8_t payload[] = {model_id, major, minor, patch};
    return build_ota_response(TMY06_OTA_COMMAND_VERSION,
                              payload, sizeof(payload), frame, capacity);
}

size_t tmy06_protocol_build_ota_status_frame(tmy06_ota_status_t status,
                                             uint8_t information,
                                             uint8_t *frame,
                                             size_t capacity)
{
    if (status > TMY06_OTA_STATUS_ERROR ||
        (status != TMY06_OTA_STATUS_ERROR && information != 0U)) {
        return 0U;
    }

    const uint8_t payload[] = {(uint8_t)status, information};
    return build_ota_response(TMY06_OTA_COMMAND_STATUS,
                              payload, sizeof(payload), frame, capacity);
}

static bool apply_control_value(const ble_sim_profile_info_t *info,
                                tmy06_command_t command,
                                uint8_t control,
                                tmy06_state_t *state)
{
    uint8_t value;

    switch (command) {
    case TMY06_COMMAND_SET_POWER:
        value = (control >> 6U) & 0x03U;
        if (value > TMY06_POWER_ON) {
            return false;
        }
        state->power = (tmy06_power_t)value;
        return true;

    case TMY06_COMMAND_SET_UNIT:
        value = (control >> 4U) & 0x03U;
        if (value > TMY06_UNIT_FAHRENHEIT) {
            return false;
        }
        return fridge_protocol_change_unit(state, (tmy06_temperature_unit_t)value);

    case TMY06_COMMAND_SET_MODE:
        if (info->has_child_lock) {
            value = (control >> 2U) & 0x03U;
            if (value > 1U) {
                return false;
            }
            state->mode = value == 0U ? TMY06_MODE_COOLING : TMY06_MODE_HEATING;
        } else {
            value = control & 0x0FU;
            if (value > TMY06_MODE_HEATING ||
                (info->supported_mode_mask & MODE_MASK(value)) == 0U) {
                return false;
            }
            state->mode = (tmy06_mode_t)value;
        }
        return true;

    case TMY06_COMMAND_SET_CHILD_LOCK:
        if (!info->has_child_lock) {
            return false;
        }
        value = control & 0x03U;
        if (value > TMY06_CHILD_LOCK_ON) {
            return false;
        }
        state->child_lock = (tmy06_child_lock_t)value;
        return true;

    default:
        return false;
    }
}

static bool apply_ambient_value(const uint8_t *data, tmy06_state_t *state)
{
    uint8_t operation = data[0];
    if (operation == 0x00U || operation == 0x01U) {
        for (size_t i = 0; i < 3U; ++i) state->ambient.colors[operation][i] = data[1U + i];
    } else if (operation == 0x10U || operation == 0x11U) {
        state->ambient.effect = operation & 1U;
    } else if (operation != 0xFFU) {
        return false;
    }
    return true;
}

bool fridge_protocol_apply_app_frame(ble_sim_profile_t profile,
                                     const uint8_t *frame,
                                     size_t length,
                                     tmy06_state_t *state,
                                     tmy06_command_t *command)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    if (state == NULL || frame == NULL ||
        !tmy06_protocol_validate_frame(frame, length)) {
        return false;
    }

    tmy06_command_t received_command = (tmy06_command_t)frame[3];
    bool merged = info->has_ambient_light && length == TMY06_CONTROL_FRAME_SIZE && frame[2] == 0x0AU;
    bool old_ambient = received_command == TMY06_COMMAND_SET_AMBIENT_LIGHT &&
        info->has_ambient_light && length == TMY06_AMBIENT_FRAME_SIZE && frame[2] == 0x07U;
    if (!merged && !old_ambient &&
        (length != TMY06_APP_FRAME_SIZE || frame[2] != TMY06_APP_DATA_LENGTH)) return false;
    if (received_command == TMY06_COMMAND_SET_AMBIENT_LIGHT && !merged && !old_ambient) return false;
    // Validate and apply to a temporary state: a bad appended light operation
    // must not partially apply the temperature, mode, power or unit change.
    tmy06_state_t candidate = *state;
    if (old_ambient) {
        if (!apply_ambient_value(&frame[4], &candidate)) return false;
        *state = candidate;
        if (command != NULL) *command = received_command;
        return true;
    }
    bool accepted = false;

    switch (received_command) {
    case TMY06_COMMAND_GET_STATE:
        accepted = !merged || frame[7] == 0xFFU; /* A query never changes settings. */
        break;

    case TMY06_COMMAND_SET_POWER:
    case TMY06_COMMAND_SET_MODE:
    case TMY06_COMMAND_SET_UNIT:
    case TMY06_COMMAND_SET_CHILD_LOCK:
        accepted = apply_control_value(info, received_command, frame[4], &candidate);
        break;

    case TMY06_COMMAND_SET_TARGET_TEMPERATURE:
        candidate.target_temperature = (int16_t)(((uint16_t)frame[5] << 8U) |
                                               (uint16_t)frame[6]);
        accepted = true;
        break;

    case TMY06_COMMAND_SET_AMBIENT_LIGHT:
        accepted = merged;
        break;

    default:
        break;
    }

    if (accepted && merged) accepted = apply_ambient_value(&frame[7], &candidate);
    if (accepted) {
        *state = candidate;
        if (command != NULL) *command = received_command;
    }

    return accepted;
}

static uint8_t build_control(const ble_sim_profile_info_t *info,
                             const tmy06_state_t *state)
{
    uint8_t control = ((uint8_t)state->power << 6U) |
                      ((uint8_t)state->unit << 4U);
    if (info->has_child_lock) {
        uint8_t y06_mode = state->mode == TMY06_MODE_HEATING ? 1U : 0U;
        control |= (uint8_t)(y06_mode << 2U) | (uint8_t)state->child_lock;
    } else {
        control |= (uint8_t)state->mode;
    }
    return control;
}

const uint8_t *tmy06_protocol_active_ambient_color(const tmy06_state_t *state)
{
    if (state == NULL) return NULL;
    return state->ambient.colors[state->mode == TMY06_MODE_HEATING ? 1 : 0];
}

size_t fridge_protocol_build_status_frame(ble_sim_profile_t profile,
                                          const tmy06_state_t *state,
                                          uint8_t *frame,
                                          size_t capacity)
{
    const ble_sim_profile_info_t *info = ble_sim_profile_info(profile);
    size_t frame_size = info->has_voltage ? FRIDGE_STATUS_FRAME_SIZE
                                          : TMY06_STATUS_FRAME_SIZE;
    if (!fridge_protocol_validate_state(profile, state) || frame == NULL ||
        capacity < frame_size) {
        return 0;
    }

    uint16_t target = (uint16_t)state->target_temperature;
    uint16_t actual = (uint16_t)state->actual_temperature;

    frame[0] = TMY06_FRAME_HEADER_HIGH;
    frame[1] = TMY06_FRAME_HEADER_LOW;
    frame[2] = info->has_voltage ? FRIDGE_STATUS_DATA_LENGTH
                                 : TMY06_STATUS_DATA_LENGTH;
    frame[3] = TMY06_STATUS_COMMAND;
    frame[4] = build_control(info, state);
    frame[5] = state->fault_code;
    frame[6] = (uint8_t)(target >> 8U);
    frame[7] = (uint8_t)target;
    frame[8] = (uint8_t)(actual >> 8U);
    frame[9] = (uint8_t)actual;

    if (info->has_voltage) {
        frame[10] = (uint8_t)(state->actual_voltage_centi_volts >> 8U);
        frame[11] = (uint8_t)state->actual_voltage_centi_volts;
        frame[12] = tmy06_protocol_checksum(frame, 12U);
        frame[13] = TMY06_FRAME_TAIL;
    } else {
        frame[10] = state->ambient.effect;
        for (size_t i = 0; i < 3U; ++i) {
            frame[11U + i] = state->ambient.colors[0][i];
            frame[14U + i] = state->ambient.colors[1][i];
        }
        frame[17] = tmy06_protocol_checksum(frame, 17U);
        frame[18] = TMY06_FRAME_TAIL;
    }

    return frame_size;
}

bool tmy06_protocol_apply_app_frame(const uint8_t *frame,
                                    size_t length,
                                    tmy06_state_t *state,
                                    tmy06_command_t *command)
{
    return fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_LEGACY_Y06,
                                           frame, length, state, command);
}

size_t tmy06_protocol_build_status_frame(const tmy06_state_t *state,
                                         uint8_t *frame,
                                         size_t capacity)
{
    return fridge_protocol_build_status_frame(BLE_SIM_PROFILE_LEGACY_Y06,
                                              state, frame, capacity);
}
