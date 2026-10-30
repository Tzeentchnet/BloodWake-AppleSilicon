cmake_minimum_required(VERSION 3.20)
project(bw_apu_output_test C)

if(WIN32)
    message(FATAL_ERROR "These tests exercise the POSIX SDL audio backend and pthread workers")
endif()

get_filename_component(BW_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory("${BW_ROOT}/src/xemu_dsp" "${CMAKE_CURRENT_BINARY_DIR}/xemu_dsp")

add_executable(bw_apu_output_test
    test_main.c
    "${BW_ROOT}/src/toolkit_patches/apu/apu_sdl2.c"
    "${BW_ROOT}/src/toolkit_patches/apu/apu_xaudio2.c")
target_include_directories(bw_apu_output_test PRIVATE "${XBOXRECOMP_DIR}/src/apu")
target_link_libraries(bw_apu_output_test PRIVATE SDL2::SDL2)
set_target_properties(bw_apu_output_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_executable(bw_dsp_test test_dsp.c)
target_include_directories(bw_dsp_test PRIVATE "${BW_ROOT}/src/xemu_dsp")
target_link_libraries(bw_dsp_test PRIVATE bw_xemu_dsp Threads::Threads)
set_target_properties(bw_dsp_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_executable(bw_apu_lifecycle_test
    test_lifecycle.c
    test_support.c
    "${BW_ROOT}/src/toolkit_patches/apu/apu_core.c"
    "${BW_ROOT}/src/toolkit_patches/apu/apu_vp.c"
    "${BW_ROOT}/src/toolkit_patches/apu/apu_dsp.c"
    "${BW_ROOT}/src/toolkit_patches/apu/bw_apu_xdsp.c"
    "${BW_ROOT}/src/toolkit_patches/apu/apu_sdl2.c"
    "${BW_ROOT}/src/toolkit_patches/apu/apu_xaudio2.c"
    "${XBOXRECOMP_DIR}/src/apu/apu_wav.c"
    "${XBOXRECOMP_DIR}/src/platform/win32_compat.c")
target_include_directories(bw_apu_lifecycle_test PRIVATE
    "${BW_ROOT}/src"
    "${BW_ROOT}/src/toolkit_patches/apu"
    "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/apu"
    "${XBOXRECOMP_DIR}/src/nv2a")
target_link_libraries(bw_apu_lifecycle_test PRIVATE bw_xemu_dsp SDL2::SDL2 Threads::Threads)
if(NOT APPLE)
    target_link_libraries(bw_dsp_test PRIVATE m)
    target_link_libraries(bw_apu_lifecycle_test PRIVATE m)
endif()
set_target_properties(bw_apu_lifecycle_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

enable_testing()
add_test(NAME bw_apu_output COMMAND bw_apu_output_test)
set_tests_properties(bw_apu_output PROPERTIES ENVIRONMENT
    "SDL_AUDIODRIVER=dummy;RECOMP_AUDIO_BUF_COUNT=8;RECOMP_AUDIO_BUF_SAMPLES=512")
add_test(NAME bw_dsp COMMAND bw_dsp_test)
add_test(NAME bw_apu_lifecycle COMMAND bw_apu_lifecycle_test)
set_tests_properties(bw_apu_lifecycle PROPERTIES ENVIRONMENT
    "SDL_AUDIODRIVER=dummy;BW_APU_DSP=xemu;BW_APU_DSP_FALLBACK=0"
    TIMEOUT 20)
add_test(NAME bw_apu_no_device COMMAND bw_apu_lifecycle_test --no-device)
set_tests_properties(bw_apu_no_device PROPERTIES ENVIRONMENT
    "SDL_AUDIODRIVER=invalid-bw-test-driver;BW_APU_DSP=xemu"
    TIMEOUT 10)
