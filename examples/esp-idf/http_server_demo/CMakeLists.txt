# Top-level project file for the http-c ESP-IDF example.
#
# Build with:
#   idf.py set-target esp32
#   idf.py build
#
# This project consumes the http-c repository (three levels up) directly as
# an ESP-IDF component. The repository's root CMakeLists.txt registers itself
# as a component when ESP_PLATFORM is defined. The Wi-Fi/Ethernet bring-up
# uses the protocol_examples_common helper component shipped with ESP-IDF.

cmake_minimum_required(VERSION 3.18)

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../../..
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(http_server_demo)
