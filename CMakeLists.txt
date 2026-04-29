cmake_minimum_required (VERSION 3.18)
project(http)

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

add_subdirectory(deps/common-lib-c)

set(
    SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/http_headers.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_request.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_request_parser.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_response.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_endpoint.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_connection.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server_storage_for_server_host.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server_storage_for_microcontroller.c
)

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/inc
    ${COMMON_LIB_C_INC_DIR}
)
add_library(http ${SOURCES})

add_subdirectory(samples)
add_subdirectory(tests)

option(HTTP_C_BUILD_BENCHMARKS "Build benchmark executables" OFF)
if(HTTP_C_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
