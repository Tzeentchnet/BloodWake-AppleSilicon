cmake_minimum_required(VERSION 3.20)
project(bw_dma_ownership C)

find_package(Threads REQUIRED)
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
if(NOT EXISTS "${XBOXRECOMP_DIR}/src/kernel/xbox_memory_layout.h")
    message(FATAL_ERROR "The pinned xboxrecomp checkout was not found")
endif()

add_executable(dma_existing_counterexamples existing_counterexamples.c)
target_include_directories(dma_existing_counterexamples PRIVATE
    "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/kernel")
target_compile_options(dma_existing_counterexamples PRIVATE -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(dma_existing_counterexamples PRIVATE -Wl,-dead_strip)
else()
    target_link_options(dma_existing_counterexamples PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(dma_existing_counterexamples PRIVATE Threads::Threads)
target_compile_features(dma_existing_counterexamples PRIVATE c_std_11)

add_executable(dma_reference_model reference_model.c)
target_link_libraries(dma_reference_model PRIVATE Threads::Threads)
target_compile_features(dma_reference_model PRIVATE c_std_11)

add_executable(dma_physical_authority
    physical_authority_model.c physical_authority_tests.c)
target_link_libraries(dma_physical_authority PRIVATE Threads::Threads)
target_compile_features(dma_physical_authority PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dma_physical_authority PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME existing_heap_reuse_default COMMAND dma_existing_counterexamples heap)
add_test(NAME existing_contiguous_reuse COMMAND dma_existing_counterexamples contiguous)
add_test(NAME existing_alias_ambiguity COMMAND dma_existing_counterexamples alias)
add_test(NAME reference_pin_contract COMMAND dma_reference_model)
set_tests_properties(existing_heap_reuse_default existing_contiguous_reuse
    existing_alias_ambiguity reference_pin_contract PROPERTIES TIMEOUT 30)

foreach(case IN ITEMS aliases subpage batch limits shutdown handoff churn release claim)
    add_test(NAME physical_authority_${case} COMMAND dma_physical_authority ${case})
    set_tests_properties(physical_authority_${case} PROPERTIES TIMEOUT 30)
endforeach()
