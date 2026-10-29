cmake_minimum_required(VERSION 3.20)
project(bw_metal_backend_test C OBJC)

if(NOT APPLE)
    message(FATAL_ERROR "The Metal backend test requires macOS")
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
check_toolkit_source("src/kernel/nv2a_backend.h"
    "588025bae07f2edcd713391c4d0f865daf31080f8b830d12f0d3f1e78e5e6bd8")
check_toolkit_source("src/kernel/nv2a_combiner.c"
    "8992943fb3ca3128fca4f85f49255c0a1474b2ead93cddc085ab052e4cb44edf")
check_toolkit_source("src/kernel/nv2a_combiner.h"
    "a374412a230e78e9b5289951fc5094f657a4f0915d40063e608171f39d789d70")
check_toolkit_source("src/kernel/nv2a_flip_hold.c"
    "852ddf28fc6795eac7a9b0820321112e216929373895eee2c49b0d102d047fe1")
check_toolkit_source("src/kernel/nv2a_flip_hold.h"
    "6b82607098ced2f925aec2085eabaf9b81d24d6b1c65dc689ee1a1739f47c5bb")
check_toolkit_source("src/kernel/nv2a_vsh_interp.h"
    "4bcb9ca34700b06ecf245b564af3ec90ebd4763f5c3cdaa8d5042ee7282fb545")
check_toolkit_source("src/d3d/d3d8_swizzle.h"
    "65e7523f651900eed17b29cf12ba6c51fbeb6c70fc5616a3218b9a9ab205b64d")

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)

add_executable(bw_metal_backend_test
    test_main.c
    ../../src/gpu/bw_metal_backend.m
    ../../src/toolkit_patches/kernel/nv2a_pb_exec.c
    ../../src/toolkit_patches/kernel/nv2a_vsh_interp.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c"
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_flip_hold.c")
target_include_directories(bw_metal_backend_test PRIVATE
    ../../src/gpu
    "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/kernel")
target_compile_options(bw_metal_backend_test PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
target_link_libraries(bw_metal_backend_test PRIVATE
    "${FOUNDATION_FRAMEWORK}" "${METAL_FRAMEWORK}" m)
set_target_properties(bw_metal_backend_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

enable_testing()
foreach(case visual seed-order perspective full-rect extent-boundary texture-boundary lifecycle invalid-base
             unsupported-state unsupported-fog unsupported-texture cube-texture texture-extent
             invalid-surface unsupported-surface unsupported-origin
             invalid-address surface-extent invalid-texture unaligned-texture unsupported-combiner
             screen-combiner unsupported-stage unsupported-clear unsupported-rect incomplete-rect
             surface-switch feedback)
    add_test(NAME "metal_${case}" COMMAND bw_metal_backend_test "${case}")
    set_tests_properties("metal_${case}" PROPERTIES TIMEOUT 20)
endforeach()
