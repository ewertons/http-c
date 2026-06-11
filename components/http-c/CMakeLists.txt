# Reusable ESP-IDF component for http-c.
#
# An ESP-IDF component is named after its directory, so this folder is
# deliberately named "http-c": the component name is therefore always
# "http-c" regardless of the repository checkout directory name.
#
# To consume this component from an ESP-IDF project, add this directory's
# parent (the repo's "components" folder) to EXTRA_COMPONENT_DIRS and list
# "http-c" in your component's REQUIRES, e.g.:
#
#   set(EXTRA_COMPONENT_DIRS
#       path/to/http-c/components
#       path/to/http-c/deps/common-lib-c/components)
#   # in your component CMakeLists.txt: REQUIRES http-c
#
# http-c depends on common-lib-c (declared below via REQUIRES), so the
# common-lib-c component must also be discoverable. The microcontroller
# storage backend is compiled here; the server-host backend statically
# allocates several megabytes and will not fit on an MCU.

# http-c repository root, two levels up from this file:
#   components/http-c -> repo root
get_filename_component(HTTP_C_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${HTTP_C_ROOT}/src/http_headers.c"
        "${HTTP_C_ROOT}/src/http_query.c"
        "${HTTP_C_ROOT}/src/http_request.c"
        "${HTTP_C_ROOT}/src/http_request_parser.c"
        "${HTTP_C_ROOT}/src/http_response.c"
        "${HTTP_C_ROOT}/src/http_endpoint.c"
        "${HTTP_C_ROOT}/src/http_connection.c"
        "${HTTP_C_ROOT}/src/http_server.c"
        "${HTTP_C_ROOT}/src/http_server_storage_for_microcontroller.c"
        "${HTTP_C_ROOT}/src/multipart.c"
    INCLUDE_DIRS
        "${HTTP_C_ROOT}/inc"
    REQUIRES
        common-lib-c
        lwip
        pthread
)
