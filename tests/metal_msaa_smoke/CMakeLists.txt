cmake_minimum_required(VERSION 3.20)
project(bw_metal_msaa_smoke C OBJC)

if(NOT APPLE)
    message(FATAL_ERROR "The real-Metal MSAA smoke test requires macOS")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)

add_library(bw_metal_msaa_smoke_core STATIC metal_msaa_smoke.c)
target_include_directories(bw_metal_msaa_smoke_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}")

add_executable(bw_metal_msaa_smoke main.m)
target_compile_options(bw_metal_msaa_smoke PRIVATE -fobjc-arc)
target_link_libraries(bw_metal_msaa_smoke PRIVATE
    bw_metal_msaa_smoke_core
    "${FOUNDATION_FRAMEWORK}"
    "${METAL_FRAMEWORK}")

add_executable(bw_metal_msaa_smoke_test test_main.c)
target_link_libraries(bw_metal_msaa_smoke_test PRIVATE
    bw_metal_msaa_smoke_core)

enable_testing()
add_test(NAME metal_msaa_smoke_contract
    COMMAND bw_metal_msaa_smoke_test)
add_test(NAME metal_msaa_smoke_device
    COMMAND bw_metal_msaa_smoke)
set_tests_properties(
    metal_msaa_smoke_contract
    metal_msaa_smoke_device
    PROPERTIES TIMEOUT 20)
