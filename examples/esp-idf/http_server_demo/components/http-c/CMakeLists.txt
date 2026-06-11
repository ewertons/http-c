# ESP-IDF component wrapper for the http-c library.
#
# An ESP-IDF component is named after its directory, so this folder is
# deliberately named "http-c": the component name is therefore always
# "http-c" regardless of what the repository checkout directory is called.
# ESP-IDF auto-discovers components under <project>/components, so no
# EXTRA_COMPONENT_DIRS entry is needed for this one.
#
# It compiles the portable sources of both http-c and its bundled
# common-lib-c runtime into a single component. The microcontroller storage
# backend is used (the server-host backend statically allocates several
# megabytes and will not fit on an MCU). TLS is disabled (no OpenSSL on
# ESP-IDF; mbedTLS is used by the rest of the system) and the portable
# select() event-loop backend is selected (lwIP has no epoll/eventfd).

# Repository root, five levels up from this file:
#   examples/esp-idf/http_server_demo/components/http-c -> repo root
get_filename_component(HTTP_C_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        # http-c core
        "${HTTP_C_ROOT}/src/http_headers.c"
        "${HTTP_C_ROOT}/src/http_query.c"
        "${HTTP_C_ROOT}/src/http_request.c"
        "${HTTP_C_ROOT}/src/http_request_parser.c"
        "${HTTP_C_ROOT}/src/http_response.c"
        "${HTTP_C_ROOT}/src/http_endpoint.c"
        "${HTTP_C_ROOT}/src/http_connection.c"
        "${HTTP_C_ROOT}/src/http_server.c"
        "${HTTP_C_ROOT}/src/http_server_storage_for_microcontroller.c"
        "${HTTP_C_ROOT}/src/multipart.c"
        # common-lib-c portable runtime
        "${HTTP_C_ROOT}/deps/common-lib-c/src/arg_parser.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/base64.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/bst_redblack.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/circular_list.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/event_loop.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/hmac_sha256.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/json_reader.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/json_token.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/json_writer.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/list.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/logging.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/socket.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/socket_stream.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/span.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/stackx.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/stringx.c"
        "${HTTP_C_ROOT}/deps/common-lib-c/src/task.c"
    INCLUDE_DIRS
        "${HTTP_C_ROOT}/inc"
        "${HTTP_C_ROOT}/deps/common-lib-c/inc"
    REQUIRES
        lwip
        pthread
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    SOCKET_TLS_NONE=1
    EVENT_LOOP_BACKEND_SELECT=1
)
