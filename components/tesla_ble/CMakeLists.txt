# protobuf-c 生成源（proto 目录下执行 ./proto/gen_proto.sh 生成）
set(PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/proto")
set(PROTO_C_DIR "${CMAKE_CURRENT_LIST_DIR}/proto-c")
set(PROTO_FILES
    "${PROTO_DIR}/keys.proto"
    "${PROTO_DIR}/errors.proto"
    "${PROTO_DIR}/signatures.proto"
    "${PROTO_DIR}/vcsec.proto"
    "${PROTO_DIR}/universal_message.proto"
    "${PROTO_DIR}/car_server_slim.proto"
)
set(PROTO_C_SRCS
    "${PROTO_C_DIR}/keys.pb-c.c"
    "${PROTO_C_DIR}/errors.pb-c.c"
    "${PROTO_C_DIR}/signatures.pb-c.c"
    "${PROTO_C_DIR}/vcsec.pb-c.c"
    "${PROTO_C_DIR}/universal_message.pb-c.c"
    "${PROTO_C_DIR}/car_server_slim.pb-c.c"
)
set(PROTO_C_HDRS
    "${PROTO_C_DIR}/keys.pb-c.h"
    "${PROTO_C_DIR}/errors.pb-c.h"
    "${PROTO_C_DIR}/signatures.pb-c.h"
    "${PROTO_C_DIR}/vcsec.pb-c.h"
    "${PROTO_C_DIR}/universal_message.pb-c.h"
    "${PROTO_C_DIR}/car_server_slim.pb-c.h"
)

set(PROTO_C_NEEDS_GENERATION OFF)
foreach(PROTO_C_OUTPUT ${PROTO_C_SRCS} ${PROTO_C_HDRS})
    if(NOT EXISTS "${PROTO_C_OUTPUT}")
        set(PROTO_C_NEEDS_GENERATION ON)
    endif()
endforeach()

if(PROTO_C_NEEDS_GENERATION)
    find_program(PROTOC_C_EXECUTABLE protoc-c)
    if(NOT PROTOC_C_EXECUTABLE)
        message(FATAL_ERROR
            "Missing generated protobuf-c files in '${PROTO_C_DIR}'.\n"
            "Install protobuf-c first, then run:\n"
            "  cd ${PROTO_DIR} && ./gen_proto.sh\n"
            "On macOS with Homebrew:\n"
            "  brew install protobuf-c"
        )
    endif()

    file(MAKE_DIRECTORY "${PROTO_C_DIR}")
    execute_process(
        COMMAND "${PROTOC_C_EXECUTABLE}" "--c_out=${PROTO_C_DIR}" "-I" "${PROTO_DIR}" ${PROTO_FILES}
        RESULT_VARIABLE PROTO_C_RESULT
        OUTPUT_VARIABLE PROTO_C_STDOUT
        ERROR_VARIABLE PROTO_C_STDERR
    )
    if(NOT PROTO_C_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to generate protobuf-c files with ${PROTOC_C_EXECUTABLE}.\n"
            "${PROTO_C_STDOUT}\n${PROTO_C_STDERR}"
        )
    endif()
endif()

idf_component_register(
    SRCS
        "tesla_ble.c"
        "tesla_protocol.c"
        "tesla_crypto.c"
        "tesla_transport.c"
        "tesla_session.c"
        ${PROTO_C_SRCS}
    INCLUDE_DIRS
        "include"
        "proto-c"
    PRIV_REQUIRES
        bt
        nvs_flash
        mbedtls
        protobuf-c
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE TESLA_BLE_EXTERNAL_HOST=1)
