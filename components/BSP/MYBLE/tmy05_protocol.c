#include "tmy05_protocol.h"

#define TMY05_FRAME_HEADER_HIGH 0xA5U
#define TMY05_FRAME_HEADER_LOW 0x5AU
#define TMY05_FRAME_TAIL 0xAAU
#define TMY05_APP_DATA_LENGTH 0x06U
#define TMY05_STATUS_DATA_LENGTH 0x09U
#define TMY05_STATUS_COMMAND 0x00U

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

static bool apply_control_value(tmy05_command_t command,
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
        value = (control >> 2U) & 0x03U;
        if (value > TMY05_MODE_HEATING) {
            return false;
        }
        state->mode = (tmy05_mode_t)value;
        return true;

    case TMY05_COMMAND_SET_CHILD_LOCK:
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

bool tmy05_protocol_apply_app_frame(const uint8_t *frame,
                                    size_t length,
                                    tmy05_state_t *state,
                                    tmy05_command_t *command)
{
    if (state == NULL || length != TMY05_APP_FRAME_SIZE ||
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
        accepted = apply_control_value(received_command, frame[4], state);
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

size_t tmy05_protocol_build_status_frame(const tmy05_state_t *state,
                                         uint8_t *frame,
                                         size_t capacity)
{
    if (state == NULL || frame == NULL || capacity < TMY05_STATUS_FRAME_SIZE) {
        return 0;
    }

    uint8_t control = ((uint8_t)state->power << 6U) |
                      ((uint8_t)state->unit << 4U) |
                      ((uint8_t)state->mode << 2U) |
                      (uint8_t)state->child_lock;
    uint16_t target = (uint16_t)state->target_temperature;
    uint16_t actual = (uint16_t)state->actual_temperature;

    frame[0] = TMY05_FRAME_HEADER_HIGH;
    frame[1] = TMY05_FRAME_HEADER_LOW;
    frame[2] = TMY05_STATUS_DATA_LENGTH;
    frame[3] = TMY05_STATUS_COMMAND;
    frame[4] = control;
    frame[5] = state->fault_code;
    frame[6] = (uint8_t)(target >> 8U);
    frame[7] = (uint8_t)target;
    frame[8] = (uint8_t)(actual >> 8U);
    frame[9] = (uint8_t)actual;
    frame[10] = tmy05_protocol_checksum(frame, 10U);
    frame[11] = TMY05_FRAME_TAIL;

    return TMY05_STATUS_FRAME_SIZE;
}
