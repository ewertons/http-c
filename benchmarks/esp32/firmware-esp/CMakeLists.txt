# Benchmark firmware B: esp_http_server (ESP-IDF built-in).
cmake_minimum_required(VERSION 3.18)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_esp)
