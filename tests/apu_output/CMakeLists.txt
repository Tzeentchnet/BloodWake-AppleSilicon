cmake_minimum_required(VERSION 3.20)
project(bw_apu_output_test C)

if(WIN32)
    message(FATAL_ERROR "This test covers the POSIX SDL audio backend")
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
find_package(SDL2 REQUIRED)

add_executable(bw_apu_output_test
    test_main.c
    "${XBOXRECOMP_DIR}/src/apu/apu_sdl2.c"
    ../../src/toolkit_patches/apu/apu_xaudio2.c)
target_include_directories(bw_apu_output_test PRIVATE "${XBOXRECOMP_DIR}/src/apu")
target_link_libraries(bw_apu_output_test PRIVATE SDL2::SDL2)
set_target_properties(bw_apu_output_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

enable_testing()
add_test(NAME bw_apu_output COMMAND bw_apu_output_test)
set_tests_properties(bw_apu_output PROPERTIES ENVIRONMENT
    "SDL_AUDIODRIVER=dummy;RECOMP_AUDIO_BUF_COUNT=8;RECOMP_AUDIO_BUF_SAMPLES=512")
