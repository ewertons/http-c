# Top-level project file for the http-c ESP-IDF example.
#
# Build with:
#   idf.py set-target esp32
#   idf.py build
#
# The library is consumed through two reusable, standalone ESP-IDF
# components that live in the library repositories (not in this sample):
#   - http-c          -> <repo>/components/http-c
#   - common-lib-c    -> <repo>/deps/common-lib-c/components/common-lib-c
# Both directories are added to EXTRA_COMPONENT_DIRS below; the sample's
# "main" component then simply does `REQUIRES http-c` (http-c in turn
# REQUIRES common-lib-c). The Wi-Fi/Ethernet bring-up uses the
# protocol_examples_common helper component shipped with ESP-IDF.

cmake_minimum_required(VERSION 3.18)

# Repository root, three levels up from this sample directory:
#   examples/esp-idf/http_server_demo -> repo root
get_filename_component(HTTP_C_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
    "${HTTP_C_REPO_ROOT}/components"
    "${HTTP_C_REPO_ROOT}/deps/common-lib-c/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(http_server_demo)
