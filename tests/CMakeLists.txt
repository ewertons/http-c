cmake_minimum_required (VERSION 3.10)
set(TARGET_NAME "test-http")

project (${TARGET_NAME} LANGUAGES C)

set(CMAKE_C_STANDARD 99)

find_package(cmocka CONFIG REQUIRED)

enable_testing()

include_directories(${CMAKE_CURRENT_LIST_DIR} ${CMOCKA_PUBLIC_INCLUDE_DIRS})

add_executable(${TARGET_NAME}
                main.c
                test_http_headers.c
                test_http_request.c
                test_http_response.c
                test_http_server.c
                test_http_endpoint.c
)

target_link_libraries(${TARGET_NAME} http common-lib-c cmocka pthread)

add_test(${TARGET_NAME} ${TARGET_NAME})

