# Benchmark firmware A: http-c server.
cmake_minimum_required(VERSION 3.18)

get_filename_component(HTTP_C_REPO_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

set(EXTRA_COMPONENT_DIRS
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common
    "${HTTP_C_REPO_ROOT}/components"
    "${HTTP_C_REPO_ROOT}/deps/common-lib-c/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bench_httpc)
