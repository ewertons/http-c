cmake_minimum_required (VERSION 3.18)
project(http)

add_subdirectory(deps/common-lib-c)

set(
    SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/http_headers.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_request.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_response.c
    ${CMAKE_CURRENT_LIST_DIR}/src/http_server.c
)

include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/inc
    ${COMMON_LIB_C_INC_DIR}
)
add_library(http ${SOURCES})

add_subdirectory(samples)
add_subdirectory(tests)
