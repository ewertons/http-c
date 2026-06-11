# Top-level project file for the http-c ESP-IDF example.
#
# Build with:
#   idf.py set-target esp32
#   idf.py build
#
# The http-c library is vendored as an auto-discovered component under
# components/http-c (its CMakeLists compiles the library sources from the
# repository root). The Wi-Fi/Ethernet bring-up uses the
# protocol_examples_common helper component shipped with ESP-IDF.

cmake_minimum_required(VERSION 3.18)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(http_server_demo)
