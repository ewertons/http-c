cmake_minimum_required(VERSION 3.10)

# ---------------------------------------------------------------------------
# Shared-object linkage smoke test.
#
# Builds a tiny consumer program against the http-c and common-lib-c
# libraries and registers it as a ctest. The CMake `add_library` calls
# in the parent projects do not specify STATIC/SHARED, so they obey
# BUILD_SHARED_LIBS:
#
#   - cmake -S . -B build           (default)             -> .a
#   - cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON   -> .so
#
# The consumer below is identical in either case. When the parent build
# is configured with BUILD_SHARED_LIBS=ON, executing the test binary at
# ctest time validates that the dynamic linker resolves every symbol
# referenced from the public headers -- i.e. http-c works as a .so.
# ---------------------------------------------------------------------------

set(TARGET_NAME shared_link_consumer)
project(${TARGET_NAME} LANGUAGES C)
set(CMAKE_C_STANDARD 99)

add_executable(${TARGET_NAME} consumer.c)
target_link_libraries(${TARGET_NAME} http common-lib-c)

# Embed an RPATH pointing at the build's library output dirs so that,
# when BUILD_SHARED_LIBS=ON, the binary finds libhttp.so / libcommon-lib-c.so
# at runtime without the user having to set LD_LIBRARY_PATH.
set_target_properties(${TARGET_NAME} PROPERTIES
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH_USE_LINK_PATH TRUE)

add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
