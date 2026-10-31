cmake_minimum_required(VERSION 3.20)
project(bw_metal_backend_test C OBJC)

if(NOT APPLE)
    message(FATAL_ERROR "The Metal backend test requires macOS")
endif()

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/xboxrecomp" CACHE PATH
    "Path to the pinned xboxrecomp checkout")

function(check_toolkit_source relative_path expected_sha)
    set(path "${XBOXRECOMP_DIR}/${relative_path}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Pinned toolkit source not found: ${path}")
    endif()
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual_sha "${contents}")
    if(NOT actual_sha STREQUAL expected_sha)
        message(FATAL_ERROR "Pinned toolkit source changed: ${path} (${actual_sha})")
    endif()
endfunction()

check_toolkit_source("src/kernel/nv2a_pb_exec.c"
    "407369da9e1447c1a5dea9af343fd66877cb31e85a86cda30768f47b2625d573")
check_toolkit_source("src/kernel/nv2a_vsh_interp.c"
    "d1e1085b5ca3ed7afad5500e7e6c06b0f918545321f8b85e0a73b9e11933eb35")
check_toolkit_source("src/kernel/nv2a_backend.h"
    "588025bae07f2edcd713391c4d0f865daf31080f8b830d12f0d3f1e78e5e6bd8")
check_toolkit_source("src/kernel/nv2a_combiner.c"
    "8992943fb3ca3128fca4f85f49255c0a1474b2ead93cddc085ab052e4cb44edf")
check_toolkit_source("src/kernel/nv2a_combiner.h"
    "a374412a230e78e9b5289951fc5094f657a4f0915d40063e608171f39d789d70")
check_toolkit_source("src/kernel/nv2a_flip_hold.c"
    "852ddf28fc6795eac7a9b0820321112e216929373895eee2c49b0d102d047fe1")
check_toolkit_source("src/kernel/nv2a_flip_hold.h"
    "6b82607098ced2f925aec2085eabaf9b81d24d6b1c65dc689ee1a1739f47c5bb")
check_toolkit_source("src/kernel/nv2a_vsh_interp.h"
    "4bcb9ca34700b06ecf245b564af3ec90ebd4763f5c3cdaa8d5042ee7282fb545")
check_toolkit_source("src/d3d/d3d8_swizzle.h"
    "65e7523f651900eed17b29cf12ba6c51fbeb6c70fc5616a3218b9a9ab205b64d")

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_package(Threads REQUIRED)

set(METAL_BACKEND_TEST_SOURCES
    test_main.c
    ../../src/gpu/bw_metal_backend.m
    ../../src/toolkit_patches/kernel/nv2a_pb_exec.c
    ../../src/toolkit_patches/kernel/nv2a_vsh_interp.c
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_combiner.c"
    "${XBOXRECOMP_DIR}/src/kernel/nv2a_flip_hold.c")
function(add_metal_backend_test_executable target)
    add_executable(${target} ${METAL_BACKEND_TEST_SOURCES})
    target_include_directories(${target} PRIVATE
        ../../src/gpu
        "${XBOXRECOMP_DIR}/src"
        "${XBOXRECOMP_DIR}/src/kernel")
    target_compile_definitions(${target} PRIVATE BW_METAL_TESTING=1)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    target_link_libraries(${target} PRIVATE
        "${FOUNDATION_FRAMEWORK}" "${METAL_FRAMEWORK}" Threads::Threads m)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

add_metal_backend_test_executable(bw_metal_backend_test)
add_metal_backend_test_executable(bw_metal_backend_legacy_math_test)
target_compile_definitions(bw_metal_backend_legacy_math_test PRIVATE BW_METAL_TEST_FORCE_LEGACY_MATH=1)

enable_testing()
add_executable(bw_metal_shared_backing_test shared_backing_test.m)
target_compile_options(bw_metal_shared_backing_test PRIVATE -fobjc-arc)
target_link_libraries(bw_metal_shared_backing_test PRIVATE "${FOUNDATION_FRAMEWORK}" "${METAL_FRAMEWORK}")
add_test(NAME metal_shared-backing-experiment COMMAND bw_metal_shared_backing_test)
set_tests_properties(metal_shared-backing-experiment PROPERTIES LABELS "experiment")
add_test(NAME metal_shared-backing-reuse-counterexample
    COMMAND bw_metal_shared_backing_test reuse-counterexample)
set_tests_properties(metal_shared-backing-reuse-counterexample PROPERTIES
    LABELS "experiment;counterexample" TIMEOUT 20)
add_test(NAME metal_shared-backing-protected-alias
    COMMAND bw_metal_shared_backing_test protected-alias)
set_tests_properties(metal_shared-backing-protected-alias PROPERTIES
    LABELS "experiment" TIMEOUT 20)
add_test(NAME metal_shared-backing-protected-all-aliases
    COMMAND bw_metal_shared_backing_test protected-all-aliases)
set_tests_properties(metal_shared-backing-protected-all-aliases PROPERTIES
    LABELS "experiment" TIMEOUT 20)
add_test(NAME metal_shared-backing-unprotected-alias-counterexample
    COMMAND bw_metal_shared_backing_test unprotected-alias-counterexample)
set_tests_properties(metal_shared-backing-unprotected-alias-counterexample PROPERTIES
    LABELS "experiment;counterexample" TIMEOUT 20)

foreach(case semaphore-release semaphore-concurrent visual seed-order command-batching perspective blend color-mask alpha-test pipeline-cache cull full-rect depth-clear
             depth-draw stencil-draw polygon-offset polygon-offset-disabled polygon-offset-zero
             partial-color partial-color-masks partial-color-default-rect partial-clip partial-empty
             partial-depth partial-depth-seed partial-depth-empty partial-depth-unbound
             partial-depth-flip-only partial-depth-unbound-flip-only partial-depth-rebind
             partial-mask-full-depth partial-restart
             extent-boundary texture-boundary lifecycle invalid-base
             production-memory snapshot-normal snapshot-immediate
             snapshot-preflip-normal snapshot-preflip-immediate
             snapshot-rebind-aa-normal snapshot-rebind-aa-immediate error-thread registration
             xrgb-aa-clear xrgb-aa-depth xrgb-aa-draw
             xrgb-single-clear xrgb-single-depth-clear xrgb-single-draw xrgb-single-extended
             xrgb-single-reject-default-layout xrgb-single-reject-swizzled
             xrgb-single-reject-fixed-one xrgb-single-reject-2d-aa
             xrgb-single-reject-pitch xrgb-single-reject-origin xrgb-single-reject-unmapped
             xrgb-single-reject-incomplete-rect xrgb-single-reject-depth
             extended-basic extended-texture-cache
             extended-combiner extended-multistage extended-combiner-mappings
             extended-combiner-operations extended-combiner-constants
             extended-combiner-secondary extended-fog extended-passthrough-stage
             extended-texture-stages extended-dot-reflection extended-bc1 extended-dxt-alpha
             extended-dxt-mips extended-dxt-nearest-mip extended-dxt-mip-limit
             extended-swizzled extended-swizzled-mips
             extended-linear
             extended-texture-address
             extended-bc1-inactive-kernel-normal extended-bc1-inactive-kernel-immediate
             extended-projective extended-projective-zero extended-projective-negative
             extended-projective-subnormal
             extended-nonfinite-texture-coordinate
             extended-blend
             extended-alpha-pipeline
             extended-reject-stage-count extended-reject-late-stage extended-reject-control
             extended-reject-input extended-reject-destination extended-reject-duplicate
             extended-reject-rgb-flags extended-reject-alpha-flags
             extended-reject-rgb-mapping extended-reject-alpha-mapping
             extended-reject-final-special extended-reject-final-self-reference
             extended-reject-final-flags
             extended-reject-program extended-reject-later-program
             extended-reject-stage1-format extended-reject-stage1-alias
             extended-reject-stage1-projective extended-reject-format
             extended-reject-dxt3-span extended-reject-dxt5-span
             extended-reject-dxt5-stage3-alias extended-reject-mips extended-reject-mip-limit
             extended-reject-cube-span extended-reject-cube-layout
             extended-reject-swizzled-span extended-reject-swizzled-alias
             extended-reject-swizzled-alignment
             extended-reject-mip-span extended-reject-mip-stage3-alias
             extended-reject-mip-lod-bias extended-reject-mip-lod-min extended-reject-mip-lod-max
             extended-reject-dimensions extended-reject-address extended-reject-border extended-reject-filter
             extended-reject-conv-min-quincunx extended-reject-conv-mag-quincunx
             extended-reject-conv-both-quincunx extended-reject-conv-min-gaussian
             extended-reject-conv-mag-gaussian extended-reject-conv-min-reserved
             extended-reject-conv-mag-reserved
             extended-reject-signed extended-reject-disabled extended-reject-alphakill
             extended-reject-colorkey extended-reject-anisotropy extended-reject-span
             extended-reject-alignment
             extended-reject-linear-pitch extended-reject-linear-span
             extended-reject-linear-alignment
             extended-reject-active-alias extended-render-target-texture
             extended-reject-depth-alias extended-reject-projective-infinite
             extended-reject-projective-overflow
             framebuffer-info-normal framebuffer-info-immediate framebuffer-info-passthrough
             framebuffer-info-depth-only framebuffer-info-stale framebuffer-info-reject framebuffer-info-lifecycle
             framebuffer-info-xrgb-from-aa-to-single-normal framebuffer-info-xrgb-from-aa-to-single-immediate
             framebuffer-info-xrgb-from-single-to-aa-normal framebuffer-info-xrgb-from-single-to-aa-immediate
             framebuffer-info-xrgb-from-argb-to-single-normal framebuffer-info-xrgb-from-single-to-argb-normal
             framebuffer-info-xrgb-single-passthrough framebuffer-info-xrgb-single-empty-rect
             framebuffer-info-xrgb-single-depth-only framebuffer-info-xrgb-convolution-reject
             framebuffer-info-xrgb-convolution-reject-immediate
             framebuffer-info-xrgb-convolution-reject-min-normal
             framebuffer-info-xrgb-convolution-reject-min-immediate
             framebuffer-info-xrgb-convolution-reject-mag-normal
             framebuffer-info-xrgb-convolution-reject-mag-immediate
             framebuffer-info-xrgb-convolution-approx-normal
             framebuffer-info-xrgb-convolution-approx-immediate
             quincunx-approx-reject-format quincunx-approx-reject-cube
             quincunx-approx-reject-mips quincunx-approx-reject-wrap
             quincunx-approx-reject-signed quincunx-approx-reject-disabled
             quincunx-approx-reject-target-argb quincunx-approx-reject-target-aa
             quincunx-approx-reject-blend
             quincunx-approx-reject-width quincunx-approx-reject-height
             quincunx-approx-reject-pitch quincunx-approx-reject-unaligned
             quincunx-approx-reject-span quincunx-approx-reject-active-alias
             quincunx-approx-render-target-resolve quincunx-approx-reject-depth-alias
             quincunx-approx-lifecycle quincunx-approx-reference-sweep
             mock-checked-normal mock-checked-immediate mock-checked-failure-normal mock-checked-failure-immediate
             mock-checked-remap-normal mock-checked-remap-immediate
             mock-checked-preflip-normal mock-checked-preflip-immediate
             mock-checked-rebind-aa-normal mock-checked-rebind-aa-immediate
             mock-legacy-normal mock-legacy-pre mock-legacy-immediate
             mock-legacy-rebind-aa-normal mock-legacy-rebind-aa-immediate
             unsupported-state invalid-blend-src invalid-blend-dst invalid-blend-equation invalid-alpha-func
             invalid-cull-face invalid-cull-winding
             unsupported-fog invalid-polygon-offset-factor invalid-polygon-offset-units
             unsupported-texture cube-texture texture-extent
             invalid-surface unsupported-surface unsupported-origin
             argb-with-aa xrgb-other-aa aa-other-format xrgb-reserved-layout
             invalid-address surface-extent invalid-texture unaligned-texture unsupported-combiner
             screen-combiner unsupported-stage unsupported-clear incomplete-rect
             depth-unbound depth-format depth-address depth-invalid-combined depth-invalid-partial depth-pitch depth-color-alias
             depth-pending-color-alias depth-color-after depth-color-after-empty depth-unknown-flags
             depth-overlap depth-reinterpret depth-texture depth-cache-limit
             surface-switch feedback basic-feedback-exact basic-feedback-leading
             basic-feedback-trailing basic-feedback-adjacent)
    add_test(NAME "metal_${case}" COMMAND bw_metal_backend_test "${case}")
    set_tests_properties("metal_${case}" PROPERTIES TIMEOUT 20)
endforeach()

add_test(NAME metal_cpu_write_counterexample
    COMMAND bw_metal_backend_test cpu-write-counterexample)
set_tests_properties(metal_cpu_write_counterexample PROPERTIES
    TIMEOUT 20 LABELS "counterexample;ownership")

foreach(case extended-reject-conv-min-quincunx extended-reject-conv-mag-quincunx
             extended-reject-conv-both-quincunx extended-reject-conv-min-gaussian
             extended-reject-conv-mag-gaussian extended-reject-conv-min-reserved
             extended-reject-conv-mag-reserved
             framebuffer-info-xrgb-convolution-reject
             framebuffer-info-xrgb-convolution-reject-immediate
             framebuffer-info-xrgb-convolution-reject-min-normal
             framebuffer-info-xrgb-convolution-reject-min-immediate
             framebuffer-info-xrgb-convolution-reject-mag-normal
             framebuffer-info-xrgb-convolution-reject-mag-immediate)
    set_tests_properties("metal_${case}" PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=0")
endforeach()

foreach(case framebuffer-info-xrgb-convolution-approx-normal
             framebuffer-info-xrgb-convolution-approx-immediate
             quincunx-approx-reject-format quincunx-approx-reject-cube
             quincunx-approx-reject-mips quincunx-approx-reject-wrap
             quincunx-approx-reject-signed quincunx-approx-reject-disabled
             quincunx-approx-reject-target-argb quincunx-approx-reject-target-aa
             quincunx-approx-reject-blend
             quincunx-approx-reject-width quincunx-approx-reject-height
             quincunx-approx-reject-pitch quincunx-approx-reject-unaligned
             quincunx-approx-reject-span quincunx-approx-reject-active-alias
             quincunx-approx-render-target-resolve quincunx-approx-reject-depth-alias
             quincunx-approx-lifecycle quincunx-approx-reference-sweep)
    set_tests_properties("metal_${case}" PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1")
endforeach()

add_test(NAME metal_quincunx-strict-unset
    COMMAND "${CMAKE_COMMAND}" -E env --unset=BW_METAL_QUINCUNX_APPROX
            $<TARGET_FILE:bw_metal_backend_test> framebuffer-info-xrgb-convolution-reject)
add_test(NAME metal_quincunx-strict-invalid
    COMMAND bw_metal_backend_test framebuffer-info-xrgb-convolution-reject)
set_tests_properties(metal_quincunx-strict-invalid PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=true")
add_test(NAME metal_quincunx-strict-invalid-prefix
    COMMAND bw_metal_backend_test framebuffer-info-xrgb-convolution-reject)
set_tests_properties(metal_quincunx-strict-invalid-prefix PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=10")
foreach(case min mag)
    add_test(NAME "metal_quincunx-approx-reject-${case}-only"
        COMMAND bw_metal_backend_test "framebuffer-info-xrgb-convolution-reject-${case}-normal")
    set_tests_properties("metal_quincunx-approx-reject-${case}-only"
        PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1")
endforeach()
foreach(case extended-bc1-inactive-kernel-normal extended-bc1-inactive-kernel-immediate
             extended-reject-conv-min-gaussian extended-reject-conv-mag-gaussian
             extended-reject-conv-min-reserved extended-reject-conv-mag-reserved)
    add_test(NAME "metal_optin-${case}" COMMAND bw_metal_backend_test "${case}")
    set_tests_properties("metal_optin-${case}" PROPERTIES ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1")
endforeach()
add_test(NAME metal_quincunx-approx-warning COMMAND bw_metal_backend_test visual)
set_tests_properties(metal_quincunx-approx-warning PROPERTIES
    ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1"
    PASS_REGULAR_EXPRESSION "WARNING: BW_METAL_QUINCUNX_APPROX=1 uses a legacy Gaussian approximation")
add_test(NAME metal_quincunx-approx-use-diagnostic
    COMMAND bw_metal_backend_test framebuffer-info-xrgb-convolution-approx-normal)
set_tests_properties(metal_quincunx-approx-use-diagnostic PROPERTIES
    ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1"
    PASS_REGULAR_EXPRESSION "quincunx approximation used: legacy Gaussian")
set_tests_properties(metal_quincunx-approx-reference-sweep PROPERTIES TIMEOUT 120)
foreach(case framebuffer-info-xrgb-convolution-approx-normal
             framebuffer-info-xrgb-convolution-approx-immediate
             quincunx-approx-reference-sweep quincunx-approx-lifecycle
             quincunx-approx-render-target-resolve extended-bc1-inactive-kernel-normal
             extended-texture-stages)
    add_test(NAME "metal_legacy-math-${case}" COMMAND bw_metal_backend_legacy_math_test "${case}")
    set_tests_properties("metal_legacy-math-${case}" PROPERTIES
        ENVIRONMENT "BW_METAL_QUINCUNX_APPROX=1" TIMEOUT 120)
endforeach()
add_test(NAME metal_legacy-math-strict-unset
    COMMAND "${CMAKE_COMMAND}" -E env --unset=BW_METAL_QUINCUNX_APPROX
            $<TARGET_FILE:bw_metal_backend_legacy_math_test> framebuffer-info-xrgb-convolution-reject)
set_tests_properties(metal_legacy-math-strict-unset PROPERTIES TIMEOUT 20)

add_executable(bw_metal_gamma_test
    test_gamma.c ../../src/gpu/bw_gamma.c ../../src/gpu/bw_ab.c)
target_include_directories(bw_metal_gamma_test PRIVATE
    ../../src/gpu "${XBOXRECOMP_DIR}/src")
target_link_libraries(bw_metal_gamma_test PRIVATE Threads::Threads)
set_target_properties(bw_metal_gamma_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
foreach(case basic disabled stress ab)
    add_test(NAME "metal_gamma_${case}" COMMAND bw_metal_gamma_test "${case}")
endforeach()

add_executable(bw_metal_audit_test test_audit.c ../../src/gpu/bw_gpu_audit.c)
target_include_directories(bw_metal_audit_test PRIVATE
    ../../src/gpu "${XBOXRECOMP_DIR}/src")
target_link_libraries(bw_metal_audit_test PRIVATE Threads::Threads)
set_target_properties(bw_metal_audit_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_test(NAME metal_gpu_audit COMMAND bw_metal_audit_test)

add_executable(bw_metal_lod_test test_lod.c ../../src/gpu/bw_vfx_lod_native.c)
target_include_directories(bw_metal_lod_test PRIVATE ../../src)
target_link_libraries(bw_metal_lod_test PRIVATE Threads::Threads)
set_target_properties(bw_metal_lod_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
foreach(case original default off fix-off override clamp named-off)
    add_test(NAME "metal_lod_${case}" COMMAND bw_metal_lod_test "${case}")
endforeach()
