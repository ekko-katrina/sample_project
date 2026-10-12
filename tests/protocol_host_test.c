#include "tmy06_protocol.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static bool bytes_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

static int test_profile_cycle(void)
{
    const ble_sim_profile_info_t *legacy =
        ble_sim_profile_info(BLE_SIM_PROFILE_LEGACY_Y06);
    CHECK(legacy->write_uuid == 0xFFF7);
    CHECK(legacy->notify_uuid == 0xFFF7);

    ble_sim_profile_t profile = BLE_SIM_PROFILE_LEGACY_Y06;
    const ble_sim_profile_t expected[] = {
        BLE_SIM_PROFILE_NEW_Y17,
        BLE_SIM_PROFILE_NEW_Y16,
        BLE_SIM_PROFILE_NEW_TM310,
        BLE_SIM_PROFILE_NEW_Y06,
        BLE_SIM_PROFILE_LEGACY_Y06,
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        profile = ble_sim_profile_next(profile);
        CHECK(profile == expected[i]);
    }
    return 0;
}

static int test_initial_status_frames(void)
{
    static const uint8_t expected_y06[] = {
        0xA5, 0x5A, 0x10, 0x00, 0x40, 0x00,
        0xFF, 0xFB, 0x00, 0x03, 0x00, 0x00, 0x00, 0xFF,
        0xFF, 0x00, 0x00, 0xA8, 0xAA,
    };
    static const uint8_t expected_voltage_model[] = {
        0xA5, 0x5A, 0x0B, 0x00, 0x40, 0x00, 0xFF,
        0xFB, 0x00, 0x03, 0x04, 0xF3, 0x44, 0xAA,
    };
    uint8_t frame[FRIDGE_MAX_STATUS_FRAME_SIZE];
    tmy06_state_t state;

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_LEGACY_Y06, &state);
    size_t length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_LEGACY_Y06, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_y06));
    CHECK(bytes_equal(frame, expected_y06, length));

    length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_NEW_Y06, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_y06));
    CHECK(bytes_equal(frame, expected_y06, length));

    length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_NEW_Y17, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_voltage_model));
    CHECK(bytes_equal(frame, expected_voltage_model, length));
    return 0;
}

static int test_model_specific_modes(void)
{
    static const uint8_t y06_heat[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x04, 0x00, 0x00, 0xFE, 0xAA,
    };
    static const uint8_t max_mode[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x01, 0x00, 0x00, 0xFB, 0xAA,
    };
    static const uint8_t heat_mode[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x03, 0x00, 0x00, 0xF9, 0xAA,
    };
    tmy06_command_t command;
    tmy06_state_t state;

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y06, &state);
    CHECK(fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y06,
                                          y06_heat, sizeof(y06_heat),
                                          &state, &command));
    CHECK(command == TMY06_COMMAND_SET_MODE);
    CHECK(state.mode == TMY06_MODE_HEATING);

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y17, &state);
    CHECK(fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y17,
                                          max_mode, sizeof(max_mode),
                                          &state, &command));
    CHECK(state.mode == TMY06_MODE_MAX);

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y16, &state);
    CHECK(!fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y16,
                                           heat_mode, sizeof(heat_mode),
                                           &state, &command));
    CHECK(state.mode == TMY06_MODE_COOLING);
    return 0;
}

static int test_temperature_unit_conversion(void)
{
    static const uint8_t set_fahrenheit[] = {
        0xA5, 0x5A, 0x06, 0x04, 0x10, 0x00, 0x00, 0xED, 0xAA,
    };
    tmy06_state_t state;
    tmy06_command_t command;
    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y06, &state);

    CHECK(fridge_protocol_change_unit(&state, TMY06_UNIT_FAHRENHEIT));
    CHECK(state.unit == TMY06_UNIT_FAHRENHEIT);
    CHECK(state.target_temperature == 23);
    CHECK(state.actual_temperature == 37);
    CHECK(fridge_protocol_change_unit(&state, TMY06_UNIT_CELSIUS));
    CHECK(state.target_temperature == -5);
    CHECK(state.actual_temperature == 3);

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y06, &state);
    CHECK(fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y06,
                                          set_fahrenheit,
                                          sizeof(set_fahrenheit),
                                          &state, &command));
    CHECK(command == TMY06_COMMAND_SET_UNIT);
    CHECK(state.unit == TMY06_UNIT_FAHRENHEIT);
    CHECK(state.target_temperature == 23);
    CHECK(state.actual_temperature == 37);
    return 0;
}

static int test_ota_frames(void)
{
    static const uint8_t query[] = {
        0xA5, 0x5A, 0x04, 0xA0, 0x00, 0x5B, 0xAA,
    };
    static const uint8_t targeted_start[] = {
        0xA5, 0x5A, 0x07, 0xA2, 0x03, 0x01, 0x00, 0x04, 0x5C, 0xAA,
    };
    static const uint8_t obsolete_start[] = {
        0xA5, 0x5A, 0x04, 0xA2, 0x00, 0x59, 0xAA,
    };
    static const uint8_t expected_version[] = {
        0xA5, 0x5A, 0x07, 0xA1, 0x03, 0x01, 0x03, 0x0A, 0x52, 0xAA,
    };
    static const uint8_t expected_error[] = {
        0xA5, 0x5A, 0x05, 0xA3, 0x04, 0x02, 0x5F, 0xAA,
    };
    uint8_t frame[TMY06_OTA_VERSION_FRAME_SIZE];
    tmy06_ota_request_t request;

    CHECK(tmy06_protocol_parse_ota_request(query, sizeof(query), &request));
    CHECK(request.command == TMY06_OTA_COMMAND_QUERY_VERSION);
    CHECK(!tmy06_protocol_parse_ota_request(obsolete_start,
                                             sizeof(obsolete_start), &request));
    CHECK(tmy06_protocol_parse_ota_request(targeted_start,
                                            sizeof(targeted_start), &request));
    CHECK(request.command == TMY06_OTA_COMMAND_START);
    CHECK(request.model_id == 0x03U);
    CHECK(request.major == 0x01U && request.minor == 0x00U &&
          request.patch == 0x04U);

    uint8_t invalid[sizeof(query)];
    for (size_t i = 0; i < sizeof(query); ++i) {
        invalid[i] = query[i];
    }
    invalid[5] ^= 0x01U;
    CHECK(!tmy06_protocol_parse_ota_request(invalid, sizeof(invalid), NULL));

    size_t length = tmy06_protocol_build_ota_version_frame(
        0x03, 0x01, 0x03, 0x0A, frame, sizeof(frame));
    CHECK(length == sizeof(expected_version));
    CHECK(bytes_equal(frame, expected_version, length));

    length = tmy06_protocol_build_ota_status_frame(
        TMY06_OTA_STATUS_ERROR, TMY06_OTA_ERROR_DOWNLOAD,
        frame, sizeof(frame));
    CHECK(length == sizeof(expected_error));
    CHECK(bytes_equal(frame, expected_error, length));
    CHECK(tmy06_protocol_build_ota_status_frame(
              TMY06_OTA_STATUS_DOWNLOADING, 1U,
              frame, sizeof(frame)) == 0U);
    return 0;
}

static int test_ambient_operations(void)
{
    const ble_sim_profile_t profiles[] = {BLE_SIM_PROFILE_LEGACY_Y06, BLE_SIM_PROFILE_NEW_Y06};
    for (size_t p = 0; p < sizeof(profiles) / sizeof(profiles[0]); ++p) {
        tmy06_state_t state;
        tmy06_command_t command;
        fridge_protocol_make_initial_state(profiles[p], &state);
        uint8_t frame[] = {0xA5, 0x5A, 0x07, 0x07, 0x00, 0x12, 0x34, 0x56, 0, 0xAA};
        frame[8] = tmy06_protocol_checksum(frame, 8);
        CHECK(fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, &command));
        CHECK(command == TMY06_COMMAND_SET_AMBIENT_LIGHT);
        CHECK(state.mode == TMY06_MODE_COOLING && state.ambient.effect == 0);
        CHECK(bytes_equal(state.ambient.colors[0], &frame[5], 3));
        CHECK(state.ambient.colors[1][0] == 255);

        frame[4] = 0x01; frame[5] = 0xAB; frame[6] = 0xCD; frame[7] = 0xEF;
        frame[8] = tmy06_protocol_checksum(frame, 8);
        CHECK(fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, NULL));
        CHECK(state.mode == TMY06_MODE_COOLING && state.ambient.effect == 0);
        CHECK(tmy06_protocol_active_ambient_color(&state)[0] == 0x12);
        state.mode = TMY06_MODE_HEATING;
        CHECK(tmy06_protocol_active_ambient_color(&state)[0] == 0xAB);
        tmy06_ambient_settings_t before = state.ambient;

        // Effect operations ignore even nonzero RGB and preserve both colors.
        for (uint8_t operation = 0x10; operation <= 0x11; ++operation) {
            frame[4] = operation; frame[8] = tmy06_protocol_checksum(frame, 8);
            CHECK(fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, NULL));
            CHECK(state.ambient.effect == (operation & 1));
            CHECK(bytes_equal(&state.ambient.colors[0][0], &before.colors[0][0], 6));
            CHECK(state.mode == TMY06_MODE_HEATING);
        }
        // Black is an actual color, not a no-op, and cannot reset the effect.
        frame[4] = 0x01; frame[5] = 0; frame[6] = 0; frame[7] = 0;
        frame[8] = tmy06_protocol_checksum(frame, 8);
        CHECK(fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, NULL));
        CHECK(state.ambient.effect == 1 && state.ambient.colors[1][0] == 0);
        CHECK(state.ambient.colors[0][0] == 0x12);
        before = state.ambient;
        frame[4] = 0x02; frame[8] = tmy06_protocol_checksum(frame, 8);
        CHECK(!fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, NULL));
        CHECK(bytes_equal((const uint8_t *)&before, (const uint8_t *)&state.ambient, sizeof(before)));
        frame[4] = 0x00; frame[8] = tmy06_protocol_checksum(frame, 8) ^ 1;
        CHECK(!fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame), &state, NULL));
        CHECK(!fridge_protocol_apply_app_frame(profiles[p], frame, sizeof(frame) - 1, &state, NULL));
        frame[8] = tmy06_protocol_checksum(frame, 8);
        CHECK(!fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y17, frame, sizeof(frame), &state, NULL));
        uint8_t status[TMY06_STATUS_FRAME_SIZE];
        CHECK(fridge_protocol_build_status_frame(profiles[p], &state, status, sizeof(status)) == sizeof(status));
        CHECK(status[10] == 1 && status[11] == 0x12 && status[14] == 0);
        CHECK(tmy06_protocol_validate_frame(status, sizeof(status)));
    }
    return 0;
}

int protocol_run_tests(void)
{
    int result = test_profile_cycle();
    if (result != 0) {
        return result;
    }
    result = test_initial_status_frames();
    if (result != 0) {
        return result;
    }
    result = test_model_specific_modes();
    if (result != 0) {
        return result;
    }
    result = test_temperature_unit_conversion();
    if (result != 0) {
        return result;
    }
    result = test_ambient_operations();
    if (result != 0) return result;
    return test_ota_frames();
}

#if defined(PROTOCOL_TEST_NO_CRT)
int mainCRTStartup(void)
{
    return protocol_run_tests();
}
#elif !defined(PROTOCOL_TEST_WASM)
#include <stdio.h>

int main(void)
{
    int result = protocol_run_tests();
    if (result != 0) {
        printf("protocol_host_test: FAIL at line %d\n", result);
        return 1;
    }
    puts("protocol_host_test: PASS");
    return 0;
}
#endif
