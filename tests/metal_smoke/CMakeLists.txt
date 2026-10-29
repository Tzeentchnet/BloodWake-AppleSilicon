cmake_minimum_required(VERSION 3.20)
project(bw_metal_smoke OBJC)

if(NOT APPLE)
    message(FATAL_ERROR "The Metal smoke test requires macOS")
endif()

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)

add_executable(bw_metal_smoke test_main.m)
target_compile_options(bw_metal_smoke PRIVATE -fobjc-arc)
target_link_libraries(bw_metal_smoke PRIVATE "${FOUNDATION_FRAMEWORK}" "${METAL_FRAMEWORK}")

enable_testing()
add_test(NAME bw_metal_smoke COMMAND bw_metal_smoke)
