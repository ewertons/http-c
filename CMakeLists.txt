cmake_minimum_required (VERSION 3.18)

# ---------------------------------------------------------------------------
# ESP-IDF component mode.
#
# When this repository is consumed as an ESP-IDF component the IDF build
# system defines ESP_PLATFORM and provides idf_component_register(). In that
# case we register the portable sources of both http-c and its bundled
# common-lib-c runtime as a single component and return early -- the
# standalone host project() below is skipped.
#
# The microcontroller storage backend is used (the server-host backend
# statically allocates several megabytes and will not fit on an MCU). TLS is
# disabled (no OpenSSL on ESP-IDF) and the portable select() event-loop
# backend is selected (lwIP has no epoll/eventfd).
# ---------------------------------------------------------------------------
if(ESP_PLATFORM)
    idf_component_register(
        SRCS
            # http-c core
            src/http_headers.c
            src/http_query.c
            src/http_request.c
            src/http_request_parser.c
            src/http_response.c
            src/http_endpoint.c
            src/http_connection.c
            src/http_server.c
            src/http_server_storage_for_microcontroller.c
            src/multipart.c
            # common-lib-c portable runtime
            deps/common-lib-c/src/arg_parser.c
            deps/common-lib-c/src/base64.c
            deps/common-lib-c/src/bst_redblack.c
            deps/common-lib-c/src/circular_list.c
            deps/common-lib-c/src/event_loop.c
            deps/common-lib-c/src/hmac_sha256.c
            deps/common-lib-c/src/json_reader.c
            deps/common-lib-c/src/json_token.c
            deps/common-lib-c/src/json_writer.c
            deps/common-lib-c/src/list.c
            deps/common-lib-c/src/logging.c
            deps/common-lib-c/src/socket.c
            deps/common-lib-c/src/socket_stream.c
            deps/common-lib-c/src/span.c
            deps/common-lib-c/src/stack.c
            deps/common-lib-c/src/stringx.c
            deps/common-lib-c/src/task.c
        INCLUDE_DIRS
            inc
            deps/common-lib-c/inc
        REQUIRES
            lwip
            pthread
    )
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        SOCKET_TLS_NONE=1
        EVENT_LOOP_BACKEND_SELECT=1
    )
    return()
endif()

project(http-c)

enable_testing()

# ---------------------------------------------------------------------------
# Optional code coverage instrumentation.
#
#   cmake -S . -B build-cov -DHTTP_C_COVERAGE=ON
#   cmake --build build-cov
#   (cd build-cov && ctest --output-on-failure)
#   gcovr -r . --html --html-details -o build-cov/coverage.html build-cov
#
# scripts/run-coverage.sh wraps all of that. The flags propagate to every
# target in this build (including deps/common-lib-c) via the directory-
# scoped add_compile_options/add_link_options below.
# ---------------------------------------------------------------------------
option(HTTP_C_COVERAGE "Build with gcov instrumentation" OFF)

if(HTTP_C_COVERAGE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "HTTP_C_COVERAGE only tested with gcc/clang; you have ${CMAKE_C_COMPILER_ID}")
    endif()
    add_compile_options(-O0 -g --coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
    message(STATUS "HTTP_C_COVERAGE=ON -- instrumenting for gcov")
endif()

if ((NOT TARGET common-lib-c))
    add_subdirectory(deps/common-lib-c)
endif()

set(
    HEADER_FILES
    ${CMAKE_CURRENT_LIST_DIR}/inc/common.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_connection.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_headers.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_query.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_request_parser.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_server.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_versions.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_codes.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_endpoint.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_methods.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_request.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_response.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/http_server_storage.h
    ${CMAKE_CURRENT_LIST_DIR}/inc/multipart.h
)

set(
    SOURCE_FILES
    ${CMAKE_CURRENT_LIST_DIR}/src/http_headers.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_query.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_request.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_request_parser.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_response.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_endpoint.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_connection.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server_storage_for_server_host.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server_storage_for_microcontroller.c
    ${CMAKE_CURRENT_LIST_DIR}/src/multipart.c
)

add_library(http-c ${HEADER_FILES} ${SOURCE_FILES})

target_include_directories(http-c
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/inc
    PRIVATE
        ${COMMON_LIB_C_INC_DIR}
)

target_link_libraries(http-c PUBLIC common-lib-c)

option(HTTP_C_BUILD_SAMPLES "Build sample executables" ON)
option(HTTP_C_BUILD_TESTS   "Build test executables (requires cmocka)" ON)

if(HTTP_C_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
if(HTTP_C_BUILD_TESTS)
    add_subdirectory(tests)
endif()

option(HTTP_C_BUILD_BENCHMARKS "Build benchmark executables" OFF)
if(HTTP_C_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
