cmake_minimum_required (VERSION 3.18)
project(http_listener)

find_package(OpenSSL REQUIRED)

add_subdirectory(deps/common-lib-c)

set(
    SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/http_listener.c
    ${CMAKE_CURRENT_LIST_DIR}/src/ssl.c
)

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/inc
    ${COMMON_LIB_C_INC_DIR}
)
add_library(http_listener ${SOURCES})

add_subdirectory(samples)
