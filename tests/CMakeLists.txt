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
                test_http_request_parser.c
                test_http_response.c
                test_http_server.c
                test_http_endpoint.c
                test_http_server_storage.c
)

target_link_libraries(${TARGET_NAME} http common-lib-c cmocka pthread)

add_test(${TARGET_NAME} ${TARGET_NAME})

# Shared-object linkage validation. When configured with
# -DBUILD_SHARED_LIBS=ON this test exercises the .so version of
# libhttp and libcommon-lib-c via a separate consumer binary.
add_subdirectory(shared_link)

