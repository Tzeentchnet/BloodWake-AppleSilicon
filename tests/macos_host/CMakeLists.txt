cmake_minimum_required(VERSION 3.20)
project(bw_macos_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Pinned xboxrecomp checkout")

find_package(Threads REQUIRED)
add_executable(bw_fault_test test_fault.c ../../src/bw_posix_fault.c)
target_include_directories(bw_fault_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/src/platform")
target_link_libraries(bw_fault_test PRIVATE Threads::Threads)
target_compile_options(bw_fault_test PRIVATE -Wall -Wextra -Werror
    -Wno-deprecated-declarations)

enable_testing()
add_test(NAME bw_fault_test COMMAND bw_fault_test --non-nested)
add_test(NAME bw_fault_nested_test COMMAND bw_fault_test --nested-only)
set_tests_properties(bw_fault_test bw_fault_nested_test PROPERTIES TIMEOUT 20)
set_tests_properties(bw_fault_nested_test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(bw_boot_test test_boot.c ../../src/bw_macos_boot.c)
target_include_directories(bw_boot_test PRIVATE ../../src)
target_compile_options(bw_boot_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME bw_boot_test COMMAND bw_boot_test)
set_tests_properties(bw_boot_test PROPERTIES TIMEOUT 30)

add_executable(bw_guest_mem_test test_guest_mem.c ../../src/bw_posix_guest_mem.c)
target_include_directories(bw_guest_mem_test PRIVATE ../../src)
target_link_libraries(bw_guest_mem_test PRIVATE Threads::Threads)
target_compile_options(bw_guest_mem_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME bw_guest_mem_test COMMAND bw_guest_mem_test)
set_tests_properties(bw_guest_mem_test PROPERTIES TIMEOUT 20)

add_executable(bw_ke_event_test test_ke_event.c
    ../../src/bw_posix_guest_mem.c ../../src/bw_posix_ke_event.c)
target_include_directories(bw_ke_event_test PRIVATE ../../src)
target_compile_definitions(bw_ke_event_test PRIVATE BW_KE_EVENT_TESTING=1)
target_link_libraries(bw_ke_event_test PRIVATE Threads::Threads)
target_compile_options(bw_ke_event_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME bw_ke_event_test COMMAND bw_ke_event_test)
set_tests_properties(bw_ke_event_test PROPERTIES TIMEOUT 30)

add_executable(bw_ke_bridge_test test_ke_bridge.c
    ../../src/bw_posix_guest_mem.c ../../src/bw_posix_ke_event.c
    "${XBOXRECOMP_DIR}/src/kernel/kernel_sync.c"
    "${XBOXRECOMP_DIR}/src/platform/win32_compat.c")
target_include_directories(bw_ke_bridge_test PRIVATE ../../src
    "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/kernel"
    "${XBOXRECOMP_DIR}/src/platform" "${XBOXRECOMP_DIR}/src/apu"
    "${XBOXRECOMP_DIR}/src/usb" "${XBOXRECOMP_DIR}/src/input"
    ../../src/xemu_dsp)
target_compile_definitions(bw_ke_bridge_test PRIVATE BW_KE_BRIDGE_TESTING=1)
target_compile_options(bw_ke_bridge_test PRIVATE -ffunction-sections
    -fdata-sections -Wno-comment)
target_link_options(bw_ke_bridge_test PRIVATE -Wl,-dead_strip)
target_link_libraries(bw_ke_bridge_test PRIVATE Threads::Threads)
add_test(NAME bw_ke_bridge_test COMMAND bw_ke_bridge_test)
set_tests_properties(bw_ke_bridge_test PROPERTIES TIMEOUT 30)

add_library(bw_native_main_under_test OBJECT ../../src/bw_macos_main.c)
target_compile_definitions(bw_native_main_under_test PRIVATE main=bw_native_main)
target_include_directories(bw_native_main_under_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src/d3d"
    "${XBOXRECOMP_DIR}/src/audio" "${XBOXRECOMP_DIR}/src/input"
    "${XBOXRECOMP_DIR}/src/apu" "${XBOXRECOMP_DIR}/src/nv2a"
    "${XBOXRECOMP_DIR}/src/video")
target_compile_options(bw_native_main_under_test PRIVATE -Wall -Wextra -Werror
    -Wno-comment)

add_executable(bw_native_startup_test test_native_main.c ../../src/bw_macos_boot.c
    ../../src/toolkit_patches/kernel/nv2a_pb_scan.c
    $<TARGET_OBJECTS:bw_native_main_under_test>)
target_include_directories(bw_native_startup_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src/d3d"
    "${XBOXRECOMP_DIR}/src/audio" "${XBOXRECOMP_DIR}/src/input"
    "${XBOXRECOMP_DIR}/src/apu" "${XBOXRECOMP_DIR}/src/nv2a"
    "${XBOXRECOMP_DIR}/src/video")
target_link_libraries(bw_native_startup_test PRIVATE Threads::Threads)
target_compile_options(bw_native_startup_test PRIVATE -Wall -Wextra -Werror
    -Wno-comment)
add_test(NAME bw_native_startup_test COMMAND bw_native_startup_test)
set_tests_properties(bw_native_startup_test PROPERTIES TIMEOUT 30)

add_library(bw_native_synthetic_under_test OBJECT ../../src/bw_macos_main.c)
target_compile_definitions(bw_native_synthetic_under_test PRIVATE main=bw_native_main
    BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=1 BW_EXPERIMENTAL_SYNTHETIC_INPUT=1)
target_include_directories(bw_native_synthetic_under_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src/d3d"
    "${XBOXRECOMP_DIR}/src/audio" "${XBOXRECOMP_DIR}/src/input"
    "${XBOXRECOMP_DIR}/src/apu" "${XBOXRECOMP_DIR}/src/nv2a"
    "${XBOXRECOMP_DIR}/src/video")
target_compile_options(bw_native_synthetic_under_test PRIVATE -Wall -Wextra -Werror
    -Wno-comment)

add_executable(bw_native_synthetic_startup_test test_native_main.c ../../src/bw_macos_boot.c
    ../../src/toolkit_patches/kernel/nv2a_pb_scan.c
    $<TARGET_OBJECTS:bw_native_synthetic_under_test>)
target_compile_definitions(bw_native_synthetic_startup_test PRIVATE BW_EXPERIMENTAL_SYNTHETIC_INPUT=1)
target_include_directories(bw_native_synthetic_startup_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/include" "${XBOXRECOMP_DIR}/src"
    "${XBOXRECOMP_DIR}/src/kernel" "${XBOXRECOMP_DIR}/src/d3d"
    "${XBOXRECOMP_DIR}/src/audio" "${XBOXRECOMP_DIR}/src/input"
    "${XBOXRECOMP_DIR}/src/apu" "${XBOXRECOMP_DIR}/src/nv2a"
    "${XBOXRECOMP_DIR}/src/video")
target_link_libraries(bw_native_synthetic_startup_test PRIVATE Threads::Threads)
target_compile_options(bw_native_synthetic_startup_test PRIVATE -Wall -Wextra -Werror
    -Wno-comment)
add_test(NAME bw_native_synthetic_startup_test COMMAND bw_native_synthetic_startup_test)
set_tests_properties(bw_native_synthetic_startup_test PROPERTIES TIMEOUT 30)
set_tests_properties(bw_native_startup_test bw_native_synthetic_startup_test
    PROPERTIES RESOURCE_LOCK bw_native_fixture)
