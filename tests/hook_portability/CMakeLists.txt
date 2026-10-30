cmake_minimum_required(VERSION 3.20)
project(bw_hook_portability C)

if(NOT APPLE)
    message(STATUS "The native hook portability tests require macOS")
    return()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()
find_package(Threads REQUIRED)

set(BW_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
find_library(CORE_GRAPHICS CoreGraphics REQUIRED)

add_executable(hook_integration
    test_hooks.c
    "${BW_SRC}/recomp_manual.c"
    "${BW_SRC}/bw_hifps_hooks.c"
    "${BW_SRC}/bw_widescreen.c"
    "${BW_SRC}/bw_remaster_hooks.c"
    "${BW_SRC}/bw_audio_hooks.c"
    "${BW_SRC}/bw_fix_hooks.c"
    "${BW_SRC}/bw_visextra_hooks.c")
target_include_directories(hook_integration PRIVATE "${BW_SRC}")
target_compile_options(hook_integration PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
target_link_options(hook_integration PRIVATE -Wl,-dead_strip)
target_link_libraries(hook_integration PRIVATE Threads::Threads "${CORE_GRAPHICS}")

add_executable(hook_markers test_markers.c "${BW_SRC}/bw_remaster_hooks.c")
target_include_directories(hook_markers PRIVATE "${BW_SRC}")
target_compile_options(hook_markers PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
target_link_options(hook_markers PRIVATE -Wl,-dead_strip)

add_executable(hook_ocean test_ocean.c)
target_include_directories(hook_ocean PRIVATE "${CMAKE_CURRENT_LIST_DIR}/fixtures" "${BW_SRC}")
target_compile_options(hook_ocean PRIVATE -ffunction-sections -fdata-sections
    -ffp-contract=off -frounding-math -UNDEBUG)
target_link_options(hook_ocean PRIVATE -Wl,-dead_strip)

add_executable(hook_mission_files test_mission_files.c)
target_include_directories(hook_mission_files PRIVATE "${BW_SRC}")
target_compile_options(hook_mission_files PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
target_link_options(hook_mission_files PRIVATE -Wl,-dead_strip)

add_test(NAME hooks-original-and-native COMMAND hook_integration passthrough)
add_test(NAME hooks-audio-forced COMMAND hook_integration forced)
add_test(NAME hooks-audio-host-surround COMMAND hook_integration surround)
add_test(NAME hooks-unpaced-warning COMMAND hook_integration unpaced)
set_tests_properties(hooks-unpaced-warning PROPERTIES
    PASS_REGULAR_EXPRESSION "RECOMP_FLIP_PACING is off, so the target is not enforced")
add_test(NAME hooks-widescreen-unavailable COMMAND hook_integration widescreen)
add_test(NAME hooks-marker-dispatch COMMAND hook_markers)
add_test(NAME hooks-ocean-fenv-and-fallback COMMAND hook_ocean)
add_test(NAME hooks-mission-file-case COMMAND hook_mission_files)
