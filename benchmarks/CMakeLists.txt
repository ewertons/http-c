# Benchmark targets. Built only when invoked via benchmarks/run_bench.sh.

cmake_minimum_required(VERSION 3.16)

# http-c bench server: links against the parent's http-c library.
add_executable(http_c_bench sample_bench.c)
target_link_libraries(http_c_bench PRIVATE http common-lib-c ${OPENSSL_LIBRARIES} pthread)

# Mongoose bench: requires the mongoose source files to be present at
# benchmarks/external/mongoose. run_bench.sh fetches them.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/external/mongoose/mongoose.c")
    add_executable(mongoose_bench
        mongoose_bench.c
        external/mongoose/mongoose.c)
    target_include_directories(mongoose_bench PRIVATE external/mongoose)
    target_compile_definitions(mongoose_bench PRIVATE
        MG_TLS=MG_TLS_OPENSSL
        MG_ENABLE_OPENSSL=1
        MG_ENABLE_LINES=1
        MG_ARCH=MG_ARCH_UNIX)
    target_link_libraries(mongoose_bench PRIVATE ssl crypto)
endif()
