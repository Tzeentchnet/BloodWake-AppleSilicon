cmake_minimum_required(VERSION 3.20)
project(bw_support_port_tests C)

if(NOT APPLE)
    message(FATAL_ERROR "These tests exercise the macOS game-support port")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
if(NOT EXISTS "${XBOXRECOMP_DIR}/src/platform/CMakeLists.txt")
    message(FATAL_ERROR "The pinned xboxrecomp checkout was not found: ${XBOXRECOMP_DIR}")
endif()

add_subdirectory("${XBOXRECOMP_DIR}/src/platform" platform)
add_executable(bw_support_port_test
    test_main.c
    ../../src/bw_edition.c
    ../../src/bw_settings.c
    ../../src/bw_host.c
    ../../src/bw_state.c
    ../../src/bw_debugflags.c
    ../../src/bw_wait.c
    ../../src/bw_profile.c
    ../../src/bw_framelog.c)
target_include_directories(bw_support_port_test PRIVATE
    ../../src "${XBOXRECOMP_DIR}/src")
target_link_libraries(bw_support_port_test PRIVATE platform)
target_compile_options(bw_support_port_test PRIVATE
    -Wall -Wextra -Werror -fno-omit-frame-pointer)

enable_testing()
foreach(case settings edition save_retry save_race save_fallback save_conflict wait state debug diagnostics events_only host)
    add_test(NAME "support_${case}" COMMAND bw_support_port_test "${case}")
    set_tests_properties("support_${case}" PROPERTIES TIMEOUT 20)
endforeach()
