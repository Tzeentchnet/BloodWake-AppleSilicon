cmake_minimum_required(VERSION 3.20)
project(bw_gpu_capture_test C)

if(NOT UNIX)
    message(FATAL_ERROR "The private capture fixture currently requires POSIX")
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")

function(check_toolkit_source relative_path expected_sha)
    set(path "${XBOXRECOMP_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Pinned toolkit source not found: ${path}")
    endif()
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual_sha "${contents}")
    if(NOT actual_sha STREQUAL expected_sha)
        message(FATAL_ERROR "Pinned toolkit source changed: ${path} (${actual_sha})")
    endif()
endfunction()

check_toolkit_source("src/kernel/nv2a_pb_exec.c"
    "407369da9e1447c1a5dea9af343fd66877cb31e85a86cda30768f47b2625d573")
check_toolkit_source("src/kernel/nv2a_vsh_interp.c"
    "d1e1085b5ca3ed7afad5500e7e6c06b0f918545321f8b85e0a73b9e11933eb35")
check_toolkit_source("src/kernel/nv2a_combiner.c"
    "8992943fb3ca3128fca4f85f49255c0a1474b2ead93cddc085ab052e4cb44edf")
check_toolkit_source("src/kernel/nv2a_flip_hold.c"
    "852ddf28fc6795eac7a9b0820321112e216929373895eee2c49b0d102d047fe1")

find_package(Threads REQUIRED)

add_executable(bw_gpu_capture_test
    test_main.c
    fixture_executor.c
    ../../src/gpu/bw_gpu_capture.c
    ../../src/gpu/bw_gpu_audit.c
    ../../src/toolkit_patches/kernel/nv2a_pb_exec.c
    ../../src/toolkit_patches/kernel/nv2a_vsh_interp.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c"
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_flip_hold.c")
target_include_directories(bw_gpu_capture_test PRIVATE
    ../../src/gpu "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/kernel")
target_compile_definitions(bw_gpu_capture_test PRIVATE BW_GPU_CAPTURE_FIXTURE=1)
target_link_libraries(bw_gpu_capture_test PRIVATE Threads::Threads m)
set_target_properties(bw_gpu_capture_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

enable_testing()
foreach(case roundtrip audit-roundtrip bounds privacy corrupt unsupported missing-input
             mismatch concurrent-owner late-attach bulk-disabled)
    add_test(NAME "gpu_capture_${case}" COMMAND bw_gpu_capture_test "${case}")
    set_tests_properties("gpu_capture_${case}" PROPERTIES TIMEOUT 30)
endforeach()
