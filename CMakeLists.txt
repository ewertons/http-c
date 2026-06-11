cmake_minimum_required (VERSION 3.18)
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
