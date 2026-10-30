cmake_minimum_required(VERSION 3.20)
project(bw_macos_presenter_tests C)

if(NOT APPLE)
    message(FATAL_ERROR "The native SDL presenter tests require macOS")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
set(PINNED_PRESENTER "${XBOXRECOMP_DIR}/src/video/fb_present_sdl.c")
if(NOT EXISTS "${PINNED_PRESENTER}")
    message(FATAL_ERROR "Pinned SDL presenter not found: ${PINNED_PRESENTER}")
endif()
file(READ "${PINNED_PRESENTER}" contents)
string(REPLACE "\r\n" "\n" contents "${contents}")
string(SHA256 actual_sha "${contents}")
if(NOT actual_sha STREQUAL "0a047f77977654eed6e6dd9164f37102144aab234470bd0bf2362addc9f2a66c")
    message(FATAL_ERROR "Pinned SDL presenter changed: ${actual_sha}")
endif()

find_package(SDL2 2.0.14 REQUIRED)
find_package(Threads REQUIRED)
get_target_property(SDL_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
set(PRESENTER "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/video/fb_present_sdl.c")
add_executable(bw_macos_presenter_test test_main.c fake_presenter.c)
target_compile_definitions(bw_macos_presenter_test PRIVATE SDL_MAIN_HANDLED)
target_include_directories(bw_macos_presenter_test PRIVATE
    ${SDL_INCLUDE_DIRS}
    "${XBOXRECOMP_DIR}/src/video"
    "${XBOXRECOMP_DIR}/src/input")
target_link_libraries(bw_macos_presenter_test PRIVATE Threads::Threads)

enable_testing()
foreach(case
        headless headless-window-off no-callback wrong-thread
        init-fail dummy-driver offscreen-driver window-fail hidden-window
        renderer-fail logical-size-fail event-fail thread-fail
        frame-32 frame-16 frame-contig lost-completion blocked-copy late-worker
        close texture-fail update-fail render-fail invalid-format invalid-address
        late-present-error metal-headless metal-1x metal-xzero-1x metal-contig metal-2x1
        metal-1x2 metal-2x2 metal-batched metal-resize metal-replace
        metal-descriptor-copy metal-late-worker metal-guard-padding metal-guarded metal-outside
        metal-span-wrap metal-missing metal-missing-zero metal-mismatch-va
        metal-mismatch-pitch metal-wrong-thread metal-reuse metal-bad-null
        metal-bad-format metal-bad-aa metal-bad-sy metal-bad-geometry
        metal-bad-pitch metal-bad-dimensions metal-bad-output)
    add_test(NAME "presenter_${case}" COMMAND bw_macos_presenter_test "${case}")
    set_tests_properties("presenter_${case}" PROPERTIES TIMEOUT 20)
endforeach()

function(check_input_source name expected)
    file(READ "${XBOXRECOMP_DIR}/src/input/${name}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual "${source}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Pinned input source changed: ${name} (${actual})")
    endif()
endfunction()
check_input_source(xinput_device.c dba2708b822e5269748558e0d55898082d7478197324b52a56bd9224409ec05f)
check_input_source(keyboard_sdl.c 4c3d0c320f1e678fa3a1ae9db1052affe32f2ebe03489e14871a02af13e18bbe)
check_input_source(keyboard.c cf4cc96718246341823e40e2ae276e49f1ec2fc550f2a115cabec81c1a1d1424)

add_subdirectory("${XBOXRECOMP_DIR}/src/platform" platform)
add_subdirectory("${XBOXRECOMP_DIR}/src/input" input)
get_target_property(INPUT_SOURCES xbox_input SOURCES)
list(REMOVE_ITEM INPUT_SOURCES xinput_device.c keyboard.c keyboard_sdl.c)
list(APPEND INPUT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/xinput_device.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/keyboard.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/keyboard_sdl.c")
set_target_properties(xbox_input PROPERTIES SOURCES "${INPUT_SOURCES}")

add_executable(bw_macos_presenter_real_test
    test_real_sdl.c "${PRESENTER}"
    ../../src/gpu/bw_gamma.c ../../src/gpu/bw_ab.c)
target_compile_definitions(bw_macos_presenter_real_test PRIVATE SDL_MAIN_HANDLED)
target_include_directories(bw_macos_presenter_real_test PRIVATE
    "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/video"
    "${XBOXRECOMP_DIR}/src/input" ../../src/gpu)
target_link_libraries(bw_macos_presenter_real_test PRIVATE xbox_input platform SDL2::SDL2 Threads::Threads)
add_test(NAME presenter_real_headless COMMAND bw_macos_presenter_real_test headless)
add_test(NAME presenter_real_dummy_rejected COMMAND bw_macos_presenter_real_test dummy)
set_tests_properties(presenter_real_headless presenter_real_dummy_rejected PROPERTIES TIMEOUT 20)
