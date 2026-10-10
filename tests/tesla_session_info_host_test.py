"""Run the actual session parser on the host using real protobuf-c messages.

Usage: python3 tests/tesla_session_info_host_test.py /path/to/esp-idf
Only the parser is extracted; ESP logging is stubbed and protobuf-c is linked.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
idf = pathlib.Path(sys.argv[1])
proto = root / "components/tesla_ble"
source = (proto / "tesla_protocol.c").read_text()
parser = source[source.index("tesla_session_info_result_t tesla_parse_session_info("):
                source.index("bool tesla_parse_vehicle_status(")]
types = (proto / "include/tesla_ble.h").read_text()
session = re.search(r"typedef struct \{[^{}]*\} tesla_session_t;", types).group()
header = (proto / "include/tesla_protocol.h").read_text()
result = re.search(r"typedef enum \{[^{}]*\} tesla_session_info_result_t;", header).group()
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "universal_message.pb-c.h"
#include "signatures.pb-c.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
'''
harness += session + "\n" + result + "\n" + parser
harness += r'''
static void check(int status, size_t key_len, uint8_t prefix, size_t epoch_len,
                  tesla_session_info_result_t expected)
{
    uint8_t key[66] = {0}, epoch[17] = {0}, inner[256], wire[512];
    key[0] = prefix;
    key[1] = 0x42;
    epoch[0] = 0x23;
    Signatures__SessionInfo info = SIGNATURES__SESSION_INFO__INIT;
    info.status = status;
    info.publickey = (ProtobufCBinaryData){key_len, key};
    info.epoch = (ProtobufCBinaryData){epoch_len, epoch};
    info.counter = 17;
    info.clock_time = 123;
    size_t inner_len = signatures__session_info__pack(&info, inner);
    UniversalMessage__RoutableMessage msg = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__INIT;
    msg.payload_case = UNIVERSAL_MESSAGE__ROUTABLE_MESSAGE__PAYLOAD_SESSION_INFO;
    msg.session_info = (ProtobufCBinaryData){inner_len, inner};
    size_t wire_len = universal_message__routable_message__pack(&msg, wire);
    tesla_session_t session, before;
    memset(&session, 0, sizeof(session));
    session.counter = 99;
    before = session;
    assert(tesla_parse_session_info(wire, wire_len, &session) == expected);
    if (expected == TESLA_SESSION_INFO_OK) {
        assert(session.counter == 17 && session.clock_time == 123);
        assert(memcmp(session.vehicle_public_key, key, 65) == 0);
        assert(memcmp(session.epoch, epoch, 16) == 0);
    } else {
        assert(memcmp(&session, &before, sizeof(session)) == 0);
    }
}
int main(void)
{
    check(1, 0, 0, 0, TESLA_SESSION_INFO_KEY_NOT_ON_WHITELIST);
    check(1, 65, 4, 16, TESLA_SESSION_INFO_KEY_NOT_ON_WHITELIST);
    check(2, 65, 4, 16, TESLA_SESSION_INFO_INVALID);
    check(0, 0, 0, 0, TESLA_SESSION_INFO_INVALID);
    check(0, 64, 4, 16, TESLA_SESSION_INFO_INVALID);
    check(0, 66, 4, 16, TESLA_SESSION_INFO_INVALID);
    check(0, 65, 3, 16, TESLA_SESSION_INFO_INVALID);
    check(0, 65, 4, 0, TESLA_SESSION_INFO_INVALID);
    check(0, 65, 4, 17, TESLA_SESSION_INFO_INVALID);
    check(0, 65, 4, 16, TESLA_SESSION_INFO_OK);
    tesla_session_t session = {0};
    uint8_t malformed[] = {0xff, 0xff};
    assert(tesla_parse_session_info(NULL, 2, &session) == TESLA_SESSION_INFO_INVALID);
    assert(tesla_parse_session_info(malformed, 2, NULL) == TESLA_SESSION_INFO_INVALID);
    assert(tesla_parse_session_info(malformed, 2, &session) == TESLA_SESSION_INFO_INVALID);
    puts("tesla_session_info_host_test: PASS (13 cases)");
}
'''
runtime = idf / "components/protobuf-c/protobuf-c/protobuf-c"
with tempfile.TemporaryDirectory(prefix="tesla-session-test-") as directory:
    directory = pathlib.Path(directory)
    test = directory / "test.c"
    test.write_text(harness)
    binary = directory / "test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(runtime.parent), "-I", str(proto / "proto-c"),
                    str(test), str(runtime / "protobuf-c.c"),
                    *map(str, sorted((proto / "proto-c").glob("*.pb-c.c"))),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
