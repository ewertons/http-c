cmake_minimum_required (VERSION 3.18)

add_executable(server_client_sample
    ${CMAKE_CURRENT_LIST_DIR}/main.c)

target_link_libraries(server_client_sample
    http
    common-lib-c
    ${OPENSSL_LIBRARIES}
    pthread)
