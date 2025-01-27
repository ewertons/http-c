cmake_minimum_required (VERSION 3.18)

set(
    SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/main.c
)

add_executable(
    sample
        ${SOURCES})

target_link_libraries(
    sample
        http
        common-lib-c
        ${OPENSSL_LIBRARIES}
        pthread
)
