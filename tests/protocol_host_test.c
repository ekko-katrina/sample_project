#include "tmy05_protocol.h"

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
    ble_sim_profile_t profile = BLE_SIM_PROFILE_LEGACY_Y05;
    const ble_sim_profile_t expected[] = {
        BLE_SIM_PROFILE_NEW_Y17,
        BLE_SIM_PROFILE_NEW_Y16,
        BLE_SIM_PROFILE_NEW_TM310,
        BLE_SIM_PROFILE_NEW_Y05,
        BLE_SIM_PROFILE_LEGACY_Y05,
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        profile = ble_sim_profile_next(profile);
        CHECK(profile == expected[i]);
    }
    return 0;
}

static int test_initial_status_frames(void)
{
    static const uint8_t expected_y05[] = {
        0xA5, 0x5A, 0x09, 0x00, 0x40, 0x00,
        0xFF, 0xFB, 0x00, 0x03, 0xB1, 0xAA,
    };
    static const uint8_t expected_voltage_model[] = {
        0xA5, 0x5A, 0x0B, 0x00, 0x40, 0x00, 0xFF,
        0xFB, 0x00, 0x03, 0x04, 0xF3, 0x44, 0xAA,
    };
    uint8_t frame[FRIDGE_MAX_STATUS_FRAME_SIZE];
    tmy05_state_t state;

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_LEGACY_Y05, &state);
    size_t length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_LEGACY_Y05, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_y05));
    CHECK(bytes_equal(frame, expected_y05, length));

    length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_NEW_Y05, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_y05));
    CHECK(bytes_equal(frame, expected_y05, length));

    length = fridge_protocol_build_status_frame(
        BLE_SIM_PROFILE_NEW_Y17, &state, frame, sizeof(frame));
    CHECK(length == sizeof(expected_voltage_model));
    CHECK(bytes_equal(frame, expected_voltage_model, length));
    return 0;
}

static int test_model_specific_modes(void)
{
    static const uint8_t y05_heat[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x04, 0x00, 0x00, 0xFE, 0xAA,
    };
    static const uint8_t max_mode[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x01, 0x00, 0x00, 0xFB, 0xAA,
    };
    static const uint8_t heat_mode[] = {
        0xA5, 0x5A, 0x06, 0x03, 0x03, 0x00, 0x00, 0xF9, 0xAA,
    };
    tmy05_command_t command;
    tmy05_state_t state;

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y05, &state);
    CHECK(fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y05,
                                          y05_heat, sizeof(y05_heat),
                                          &state, &command));
    CHECK(command == TMY05_COMMAND_SET_MODE);
    CHECK(state.mode == TMY05_MODE_HEATING);

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y17, &state);
    CHECK(fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y17,
                                          max_mode, sizeof(max_mode),
                                          &state, &command));
    CHECK(state.mode == TMY05_MODE_MAX);

    fridge_protocol_make_initial_state(BLE_SIM_PROFILE_NEW_Y16, &state);
    CHECK(!fridge_protocol_apply_app_frame(BLE_SIM_PROFILE_NEW_Y16,
                                           heat_mode, sizeof(heat_mode),
                                           &state, &command));
    CHECK(state.mode == TMY05_MODE_COOLING);
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
    uint8_t frame[TMY05_OTA_VERSION_FRAME_SIZE];
    tmy05_ota_request_t request;

    CHECK(tmy05_protocol_parse_ota_request(query, sizeof(query), &request));
    CHECK(request.command == TMY05_OTA_COMMAND_QUERY_VERSION);
    CHECK(!tmy05_protocol_parse_ota_request(obsolete_start,
                                             sizeof(obsolete_start), &request));
    CHECK(tmy05_protocol_parse_ota_request(targeted_start,
                                            sizeof(targeted_start), &request));
    CHECK(request.command == TMY05_OTA_COMMAND_START);
    CHECK(request.model_id == 0x03U);
    CHECK(request.major == 0x01U && request.minor == 0x00U &&
          request.patch == 0x04U);

    uint8_t invalid[sizeof(query)];
    for (size_t i = 0; i < sizeof(query); ++i) {
        invalid[i] = query[i];
    }
    invalid[5] ^= 0x01U;
    CHECK(!tmy05_protocol_parse_ota_request(invalid, sizeof(invalid), NULL));

    size_t length = tmy05_protocol_build_ota_version_frame(
        0x03, 0x01, 0x03, 0x0A, frame, sizeof(frame));
    CHECK(length == sizeof(expected_version));
    CHECK(bytes_equal(frame, expected_version, length));

    length = tmy05_protocol_build_ota_status_frame(
        TMY05_OTA_STATUS_ERROR, TMY05_OTA_ERROR_DOWNLOAD,
        frame, sizeof(frame));
    CHECK(length == sizeof(expected_error));
    CHECK(bytes_equal(frame, expected_error, length));
    CHECK(tmy05_protocol_build_ota_status_frame(
              TMY05_OTA_STATUS_DOWNLOADING, 1U,
              frame, sizeof(frame)) == 0U);
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
