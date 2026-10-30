cmake_minimum_required(VERSION 3.20)
project(bw_mmio_host C)

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    message(FATAL_ERROR "These tests require macOS on Apple Silicon")
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")
if(NOT EXISTS "${XBOXRECOMP_DIR}/src/platform/mmio_decode_a64.h")
    message(FATAL_ERROR "The pinned xboxrecomp checkout was not found")
endif()
file(READ "${XBOXRECOMP_DIR}/src/platform/mmio_decode_a64.h" A64_DECODER)
string(REPLACE "\r\n" "\n" A64_DECODER "${A64_DECODER}")
string(SHA256 A64_DECODER_HASH "${A64_DECODER}")
if(NOT A64_DECODER_HASH STREQUAL
        "15d3ee583c3ab0135c63bf41840f1bc866df27ce3ccd9d6c24435ab9249ca618")
    message(FATAL_ERROR "The pinned ARM64 MMIO decoder changed; review the trap before testing")
endif()

find_package(Threads REQUIRED)
find_package(SDL2 2.0.14 REQUIRED)
add_executable(bw_ac97_fault_test test_ac97.c ../../src/bw_posix_fault.c)
add_executable(bw_contiguous_high_test test_contiguous_high.c)
add_executable(bw_phys_name_race_test test_phys_name_race.c)
add_executable(bw_phys_translation_race_test test_phys_translation_race.c)
add_executable(bw_heap_reclaim_race_test test_heap_reclaim_race.c)
add_executable(bw_ohci_lifecycle_test test_ohci.c)
foreach(target bw_ac97_fault_test bw_contiguous_high_test bw_phys_name_race_test
        bw_phys_translation_race_test bw_heap_reclaim_race_test bw_ohci_lifecycle_test)
    target_include_directories(${target} PRIVATE
        "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/kernel"
        "${XBOXRECOMP_DIR}/src/usb")
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
    target_link_libraries(${target} PRIVATE Threads::Threads)
endforeach()

enable_testing()
add_test(NAME ac97_signal_fault COMMAND bw_ac97_fault_test)
add_test(NAME contiguous_high_water COMMAND bw_contiguous_high_test)
add_test(NAME phys_name_concurrent_access COMMAND bw_phys_name_race_test)
add_test(NAME phys_translation_concurrent_access COMMAND bw_phys_translation_race_test)
add_test(NAME heap_reclaim_concurrent_enabled COMMAND bw_heap_reclaim_race_test 1)
add_test(NAME heap_reclaim_concurrent_zero COMMAND bw_heap_reclaim_race_test 0)
add_test(NAME heap_reclaim_concurrent_empty COMMAND bw_heap_reclaim_race_test "")
add_test(NAME heap_reclaim_concurrent_unset COMMAND bw_heap_reclaim_race_test unset)
add_test(NAME ohci_lifecycle_and_registers COMMAND bw_ohci_lifecycle_test)
set_tests_properties(ac97_signal_fault contiguous_high_water phys_name_concurrent_access
    phys_translation_concurrent_access heap_reclaim_concurrent_enabled heap_reclaim_concurrent_zero
    heap_reclaim_concurrent_empty heap_reclaim_concurrent_unset
    ohci_lifecycle_and_registers PROPERTIES TIMEOUT 30)

# This probe uses real APU callbacks but is not a production exception bridge.
add_subdirectory("../../src/xemu_dsp" "${CMAKE_CURRENT_BINARY_DIR}/xemu_dsp")
find_program(MIG_EXECUTABLE mig REQUIRED)
set(MACH_EXC_DIR "${CMAKE_CURRENT_BINARY_DIR}/mach_exc")
file(MAKE_DIRECTORY "${MACH_EXC_DIR}")
if(CMAKE_OSX_SYSROOT AND IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}")
    set(MACH_SDK "${CMAKE_OSX_SYSROOT}")
else()
    execute_process(COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE MACH_SDK OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE MACH_SDK_RESULT)
    if(NOT MACH_SDK_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not locate a macOS SDK")
    endif()
endif()
set(MACH_EXC_DEFS "${MACH_SDK}/usr/include/mach/mach_exc.defs")
if(NOT EXISTS "${MACH_EXC_DEFS}")
    message(FATAL_ERROR "A macOS SDK containing mach/mach_exc.defs is required")
endif()
add_custom_command(
    OUTPUT "${MACH_EXC_DIR}/mach_exc_server.c" "${MACH_EXC_DIR}/mach_exc.h"
    BYPRODUCTS "${MACH_EXC_DIR}/mach_exc_user.c"
    COMMAND "${CMAKE_COMMAND}" -E env "TMPDIR=${MACH_EXC_DIR}"
        "${MIG_EXECUTABLE}" -arch arm64 -isysroot "${MACH_SDK}"
        -header "${MACH_EXC_DIR}/mach_exc.h"
        -server "${MACH_EXC_DIR}/mach_exc_server.c"
        -user "${MACH_EXC_DIR}/mach_exc_user.c" "${MACH_EXC_DEFS}"
    DEPENDS "${MACH_EXC_DEFS}"
    VERBATIM)
add_library(bw_mmio_real_apu STATIC
    ../../src/toolkit_patches/apu/apu_core.c
    ../../src/toolkit_patches/apu/apu_vp.c
    ../../src/toolkit_patches/apu/apu_dsp.c
    ../../src/toolkit_patches/apu/bw_apu_xdsp.c
    ../../src/toolkit_patches/apu/apu_sdl2.c
    ../../src/toolkit_patches/apu/apu_xaudio2.c
    "${XBOXRECOMP_DIR}/src/apu/apu_wav.c"
    "${XBOXRECOMP_DIR}/src/platform/win32_compat.c")
target_include_directories(bw_mmio_real_apu PUBLIC
    ../../src ../../src/toolkit_patches/apu
    "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/apu"
    "${XBOXRECOMP_DIR}/src/nv2a" "${XBOXRECOMP_DIR}/src/usb"
    "${XBOXRECOMP_DIR}/src/input")
target_link_libraries(bw_mmio_real_apu PUBLIC bw_xemu_dsp SDL2::SDL2 Threads::Threads)
target_compile_features(bw_mmio_real_apu PUBLIC c_std_11)

# These manual probes reproduce known shutdown hazards; a zero exit status
# means the counterexample was observed, not that shutdown is safe. Keep them
# out of the default build and CTest so they cannot be mistaken for safety tests.
add_executable(bw_ohci_irq_shutdown_probe EXCLUDE_FROM_ALL
    test_irq_shutdown_ohci_probe.c
    "${XBOXRECOMP_DIR}/src/kernel/kernel_hal.c"
    "${XBOXRECOMP_DIR}/src/platform/win32_compat.c"
    "${XBOXRECOMP_DIR}/src/platform/host_time.c")
add_executable(bw_apu_irq_shutdown_probe EXCLUDE_FROM_ALL
    test_irq_shutdown_apu_probe.c test_irq_shutdown_apu_fixture.c
    "${XBOXRECOMP_DIR}/src/kernel/kernel_hal.c"
    "${XBOXRECOMP_DIR}/src/platform/host_time.c")
foreach(target bw_ohci_irq_shutdown_probe bw_apu_irq_shutdown_probe)
    target_include_directories(${target} PRIVATE
        "${XBOXRECOMP_DIR}/src" "${XBOXRECOMP_DIR}/src/kernel"
        "${XBOXRECOMP_DIR}/src/usb")
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
endforeach()
target_link_libraries(bw_ohci_irq_shutdown_probe PRIVATE Threads::Threads)
target_link_libraries(bw_apu_irq_shutdown_probe PRIVATE bw_mmio_real_apu)

add_executable(bw_mach_apu_probe test_mach_apu.c
    "${MACH_EXC_DIR}/mach_exc_server.c" ../apu_output/test_support.c)
target_include_directories(bw_mach_apu_probe PRIVATE "${MACH_EXC_DIR}")
target_link_libraries(bw_mach_apu_probe PRIVATE bw_mmio_real_apu)
add_test(NAME mach_exception_apu_probe COMMAND bw_mach_apu_probe)
set_tests_properties(mach_exception_apu_probe PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
    ENVIRONMENT "SDL_AUDIODRIVER=dummy;BW_APU_DSP=xemu;BW_APU_DSP_FALLBACK=0")

# Rename just the synthetic support getter so the fixture can inject one
# same-thread reentry during a real device callback.
add_library(bw_checked_test_support OBJECT ../apu_output/test_support.c)
target_include_directories(bw_checked_test_support PRIVATE ../../src)
target_compile_definitions(bw_checked_test_support PRIVATE
    xbox_ContiguousAllocatedBytes=bw_checked_test_contig_high_base)
add_executable(bw_checked_mmio_experiment test_checked_dispatch.c
    $<TARGET_OBJECTS:bw_checked_test_support>
    ../../src/toolkit_patches/usb/usb_gamepad.c)
target_compile_options(bw_checked_mmio_experiment PRIVATE -ffunction-sections -fdata-sections)
target_link_options(bw_checked_mmio_experiment PRIVATE -Wl,-dead_strip)
target_link_libraries(bw_checked_mmio_experiment PRIVATE bw_mmio_real_apu)
add_test(NAME checked_mmio_experiment COMMAND bw_checked_mmio_experiment)
add_test(NAME ohci_invalid_dma
    COMMAND bw_checked_mmio_experiment --invalid-dma)
set_tests_properties(checked_mmio_experiment ohci_invalid_dma PROPERTIES TIMEOUT 30
    ENVIRONMENT "SDL_AUDIODRIVER=dummy;BW_APU_DSP=xemu;BW_APU_DSP_FALLBACK=0")

# This tests state restoration and conservative access spans only; it is not
# connected to the native host or evidence of production dispatcher safety.
add_executable(bw_signal_trampoline_experiment test_signal_trampoline.c
    ../apu_output/test_support.c)
target_include_directories(bw_signal_trampoline_experiment PRIVATE
    "${XBOXRECOMP_DIR}/src")
target_compile_options(bw_signal_trampoline_experiment PRIVATE
    -ffunction-sections -fdata-sections)
target_link_options(bw_signal_trampoline_experiment PRIVATE -Wl,-dead_strip)
target_link_libraries(bw_signal_trampoline_experiment PRIVATE bw_mmio_real_apu)
target_compile_features(bw_signal_trampoline_experiment PRIVATE c_std_11)
add_test(NAME signal_trampoline_experiment COMMAND bw_signal_trampoline_experiment)
add_test(NAME signal_trampoline_real_apu
    COMMAND bw_signal_trampoline_experiment --real-apu)
add_test(NAME signal_trampoline_real_ohci
    COMMAND bw_signal_trampoline_experiment --real-ohci)
add_test(NAME signal_trampoline_nested_fault
    COMMAND bw_signal_trampoline_experiment --nested)
add_test(NAME signal_trampoline_nested_apu
    COMMAND bw_signal_trampoline_experiment --nested-apu)
add_test(NAME signal_trampoline_blocked_nested_counterexample
    COMMAND bw_signal_trampoline_experiment --blocked-nested-apu)
add_test(NAME signal_trampoline_stack_guard
    COMMAND bw_signal_trampoline_experiment --stack-guard)
add_test(NAME signal_trampoline_unsupported_opcode
    COMMAND bw_signal_trampoline_experiment --unsupported)
add_test(NAME signal_trampoline_unaligned
    COMMAND bw_signal_trampoline_experiment --unaligned)
add_test(NAME signal_trampoline_crossing_probe
    COMMAND bw_signal_trampoline_experiment --crossing)
add_test(NAME signal_trampoline_crossing_store_probe
    COMMAND bw_signal_trampoline_experiment --crossing-store)
add_test(NAME signal_trampoline_crossing_store_first_ram
    COMMAND bw_signal_trampoline_experiment --crossing-store-first-ram)
add_test(NAME signal_trampoline_span_synthetic
    COMMAND bw_signal_trampoline_experiment --span-synthetic)
add_test(NAME signal_trampoline_span_hardware
    COMMAND bw_signal_trampoline_experiment --span-hardware)
add_test(NAME signal_trampoline_span_pair_interior
    COMMAND bw_signal_trampoline_experiment --span-interior)
add_test(NAME signal_trampoline_unrelated_fault
    COMMAND bw_signal_trampoline_experiment --unrelated)
add_test(NAME signal_trampoline_cancel
    COMMAND bw_signal_trampoline_experiment --cancel)
add_test(NAME signal_trampoline_async_cancel
    COMMAND bw_signal_trampoline_experiment --async-cancel)
set_tests_properties(signal_trampoline_experiment signal_trampoline_real_apu
    signal_trampoline_real_ohci signal_trampoline_nested_fault
    signal_trampoline_nested_apu signal_trampoline_blocked_nested_counterexample
    signal_trampoline_stack_guard
    signal_trampoline_unsupported_opcode signal_trampoline_unaligned
    signal_trampoline_crossing_probe
    signal_trampoline_crossing_store_probe
    signal_trampoline_crossing_store_first_ram
    signal_trampoline_span_synthetic signal_trampoline_span_hardware
    signal_trampoline_span_pair_interior
    signal_trampoline_unrelated_fault signal_trampoline_cancel
    signal_trampoline_async_cancel PROPERTIES
    TIMEOUT 20 ENVIRONMENT "SDL_AUDIODRIVER=dummy;BW_APU_DSP=xemu;BW_APU_DSP_FALLBACK=0")
