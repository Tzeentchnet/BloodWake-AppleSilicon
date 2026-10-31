cmake_minimum_required(VERSION 3.20)
project(bw_input_integration C)

if(WIN32)
    message(FATAL_ERROR "This test covers the POSIX SDL input path")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
if(NOT EXISTS "${XBOXRECOMP_DIR}/src/input/CMakeLists.txt")
    message(FATAL_ERROR "The pinned xboxrecomp checkout was not found: ${XBOXRECOMP_DIR}")
endif()
function(check_input_source name expected)
    file(READ "${XBOXRECOMP_DIR}/src/input/${name}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual "${contents}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "The ${name} wrapper must be reviewed for this xboxrecomp revision")
    endif()
endfunction()
check_input_source(xinput_device.c dba2708b822e5269748558e0d55898082d7478197324b52a56bd9224409ec05f)
check_input_source(keyboard_sdl.c 4c3d0c320f1e678fa3a1ae9db1052affe32f2ebe03489e14871a02af13e18bbe)
check_input_source(keyboard.c cf4cc96718246341823e40e2ae276e49f1ec2fc550f2a115cabec81c1a1d1424)

find_package(SDL2 2.0.14 REQUIRED)
add_subdirectory("${XBOXRECOMP_DIR}/src/platform" platform)
add_subdirectory("${XBOXRECOMP_DIR}/src/input" input)
get_target_property(INPUT_SOURCES xbox_input SOURCES)
list(REMOVE_ITEM INPUT_SOURCES xinput_device.c)
list(REMOVE_ITEM INPUT_SOURCES keyboard.c)
list(REMOVE_ITEM INPUT_SOURCES keyboard_sdl.c)
list(APPEND INPUT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/xinput_device.c")
list(APPEND INPUT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/keyboard.c")
list(APPEND INPUT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../../src/toolkit_patches/input/keyboard_sdl.c")
set_target_properties(xbox_input PROPERTIES SOURCES "${INPUT_SOURCES}")

add_executable(bw_input_integration
    test_main.c
    ../../src/toolkit_patches/usb/usb_gamepad.c)
target_include_directories(bw_input_integration PRIVATE
    "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/input"
    "${XBOXRECOMP_DIR}/src/usb")
target_link_libraries(bw_input_integration PRIVATE xbox_input platform SDL2::SDL2)

add_executable(bw_macos_input_bridge_test
    test_macos_bridge.c ../../src/bw_macos_input_bridge.c)
target_include_directories(bw_macos_input_bridge_test PRIVATE
    ../../src
    "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/input")
target_link_libraries(bw_macos_input_bridge_test PRIVATE xbox_input platform SDL2::SDL2)

add_executable(bw_macos_input_diagnostic_test
    test_macos_bridge.c ../../src/bw_macos_input_bridge.c)
target_include_directories(bw_macos_input_diagnostic_test PRIVATE
    ../../src
    "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/input")
target_compile_definitions(bw_macos_input_diagnostic_test PRIVATE BW_EXPERIMENTAL_SYNTHETIC_INPUT=1)
target_link_libraries(bw_macos_input_diagnostic_test PRIVATE xbox_input platform SDL2::SDL2)

enable_testing()
add_test(NAME input_macos_bridge_abi COMMAND bw_macos_input_bridge_test)
add_test(NAME input_macos_bridge_diagnostic COMMAND bw_macos_input_diagnostic_test)
add_test(NAME input_macos_bridge_telemetry COMMAND bw_macos_input_diagnostic_test)
add_test(NAME input_keyboard_usb COMMAND bw_input_integration keyboard)
add_test(NAME input_keyboard_disabled COMMAND bw_input_integration keyboard-disabled)
add_test(NAME input_controller_usb COMMAND bw_input_integration controller)
add_test(NAME input_controller_without_keyboard COMMAND bw_input_integration controller-only)
add_test(NAME input_controller_hotplug COMMAND bw_input_integration hotplug)
add_test(NAME input_controller_slots COMMAND bw_input_integration slots)
add_test(NAME input_controller_concurrent COMMAND bw_input_integration concurrent)
set_tests_properties(input_macos_bridge_abi input_macos_bridge_diagnostic input_macos_bridge_telemetry input_keyboard_usb input_controller_usb input_controller_hotplug input_controller_slots input_controller_concurrent PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;RECOMP_KEYBOARD=1")
set_tests_properties(input_macos_bridge_telemetry PROPERTIES
    PASS_REGULAR_EXPRESSION "synthetic Start/Down/A state")
set_tests_properties(input_keyboard_disabled input_controller_without_keyboard PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;RECOMP_KEYBOARD=0")
set_tests_properties(input_keyboard_usb input_keyboard_disabled input_controller_usb input_controller_without_keyboard input_controller_hotplug input_controller_slots input_controller_concurrent
    PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(input_controller_concurrent PROPERTIES TIMEOUT 20)
