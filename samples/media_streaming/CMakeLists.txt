cmake_minimum_required (VERSION 3.18)

add_executable(media_streaming_sample
    ${CMAKE_CURRENT_LIST_DIR}/main.c)

target_link_libraries(media_streaming_sample
    http
    common-lib-c
    ${OPENSSL_LIBRARIES}
    pthread)
