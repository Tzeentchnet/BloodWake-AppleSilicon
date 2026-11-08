cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(bloodwake C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(APPLE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "The native target requires macOS on Apple Silicon.")
    endif()
    if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        message(FATAL_ERROR "The native target requires CMAKE_OSX_ARCHITECTURES=arm64.")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        message(FATAL_ERROR "The native target requires an Apple Silicon host or an arm64 toolchain.")
    endif()
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "macOS build architecture" FORCE)
    endif()
    enable_language(OBJC)
endif()

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)


# The generated C (tools/run_pipeline.py --gen-dir). By default in the source
# tree, as always; tools/build_from_disc.py points it at its work folder.
set(BW_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Folder holding the generated C (recomp_*.c, recomp_funcs.h, recomp_types.h)")

option(BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK
    "Build the default-off, game-specific macOS keyboard hook" OFF)
option(BW_EXPERIMENTAL_SYNTHETIC_INPUT
    "Build the default-off synthetic SDL input diagnostic" OFF)
if(BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK AND NOT APPLE)
    message(FATAL_ERROR "The experimental keyboard hook is available only on macOS.")
endif()
if(BW_EXPERIMENTAL_SYNTHETIC_INPUT AND
   (NOT APPLE OR NOT BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK))
    message(FATAL_ERROR "Synthetic input requires the experimental macOS keyboard hook.")
endif()

# Build-time translators (tools/bw_ocean_xlate.py) run with the Python CMake
# finds, or the one given as -DPython3_EXECUTABLE=... (no `py` launcher needed).
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(APPLE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BwMetalShaders.cmake")
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Blood Wake: debug symbols in Release (no change to the generated machine code;
# the linker's REF/ICF optimisations are kept), so the profiler (BW_PROFILE,
# src/bw_profile.c) and the toolkit's fault handler can name functions. Set
# before the toolkit is added so its libraries carry symbols too.
if(MSVC)
    string(APPEND CMAKE_C_FLAGS_RELEASE " /Zi /FS")
    string(APPEND CMAKE_EXE_LINKER_FLAGS_RELEASE " /DEBUG /OPT:REF /OPT:ICF")
endif()

# The native host uses the project's Metal backend. xboxrecomp v0.17 makes its
# bundled D3D8 renderer optional, so use the normal component integration with
# that renderer disabled rather than synthesizing a dummy xbox_d3d8 target.
if(APPLE)
    find_package(SDL2 2.0.14 REQUIRED)
    set(XBOXRECOMP_BUILD_D3D8 OFF)
    add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
else()
    # Preserve the upstream Windows toolkit and all of its runtime libraries.
    add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
endif()

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${BW_GEN_DIR}/*.c")
if(NOT RECOMP_GEN_SOURCES)
    message(FATAL_ERROR "No generated C in ${BW_GEN_DIR}. Run tools/run_pipeline.py first "
                        "(or tools/build_from_disc.py), or pass -DBW_GEN_DIR=<its --gen-dir>.")
endif()

if(BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK)
    set(BW_INPUT_GENERATED_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/bw_input/recomp_0032.c")
    set(BW_INPUT_PREPARED_SOURCE "${BW_GEN_DIR}/recomp_0032.c")
    if(NOT BW_INPUT_PREPARED_SOURCE IN_LIST RECOMP_GEN_SOURCES)
        message(FATAL_ERROR "The experimental input hook requires recomp_0032.c.")
    endif()
    add_custom_command(
        OUTPUT "${BW_INPUT_GENERATED_SOURCE}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/bw_input"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_macos_input_hooks.py"
            --source "${BW_INPUT_PREPARED_SOURCE}"
            --fingerprint "${CMAKE_CURRENT_SOURCE_DIR}/fingerprints/generated.json"
            --output "${BW_INPUT_GENERATED_SOURCE}"
        DEPENDS "${BW_INPUT_PREPARED_SOURCE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_macos_input_hooks.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/fingerprints/generated.json"
        VERBATIM)
    list(REMOVE_ITEM RECOMP_GEN_SOURCES "${BW_INPUT_PREPARED_SOURCE}")
    list(APPEND RECOMP_GEN_SOURCES "${BW_INPUT_GENERATED_SOURCE}")
endif()

# Core game sources
if(APPLE)
set(GAME_SOURCES
    src/bw_macos_main.c
    src/bw_macos_boot.c
    src/bw_macos_settings_panel.m
    src/bw_posix_fault.c
    src/recomp_manual.c
    src/bw_profile.c
    src/bw_framelog.c
    src/bw_host.c
    src/bw_wait.c
    src/bw_state.c
    src/bw_debugflags.c
    src/bw_edition.c
    src/bw_settings.c
    src/bw_hifps_hooks.c
    src/bw_water_hooks.c
    src/bw_widescreen.c
    src/bw_remaster_hooks.c
    src/bw_audio_hooks.c
    src/bw_fix_hooks.c
    src/bw_visextra_hooks.c
    src/bw_ocean_native.c
    src/gpu/bw_metal_backend.m
    src/gpu/bw_metal_pipeline_artifact.c
    src/gpu/bw_metal_pipeline_trace.c
    src/gpu/bw_gpu_audit.c
    src/gpu/bw_ab.c
    src/gpu/bw_gamma.c
    src/gpu/bw_vfx_lod_native.c
)
else()
set(BW_EDITION_EXTRA_SOURCES "")
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/bw_profile.c
    src/bw_framelog.c         # BW_FRAMELOG: per-frame events and long-frame stacks (P2b, test aid)
    src/bw_host.c
    src/bw_wait.c
    src/bw_state.c
    src/bw_debugflags.c
    src/bw_edition.c          # BW_EDITION: the editions (D-015)
    src/bw_settings.c         # the launcher's settings file (bw_settings.h)
    src/bw_hifps_hooks.c      # Remastered high frame rate (D-016): D-011 wrappers, see the file
    src/bw_water_hooks.c       # Remastered semantic water view/pass boundaries
    src/bw_widescreen.c       # Remastered widescreen: the picture's shape
    src/bw_remaster_hooks.c   # edition hooks (D-011 wrappers; run_pipeline scans it)
    ${BW_EDITION_EXTRA_SOURCES}
    src/bw_audio_hooks.c      # audio D-011 wrappers: the speaker configuration (docs/day1/D2.md)
    src/bw_fix_hooks.c        # Remastered (D-016): the original game's own bugs fixed (docs/day1/D11.md)
    src/bw_visextra_hooks.c   # Remastered: faithful game-side visual extras (docs/day1/T3.md)
    src/bw_ocean_native.c     # Remastered: the native ocean (docs/day1/P2a.md, OCEAN_NATIVE)
    src/gpu/bw_d3d11.c
    src/gpu/bw_rc_hlsl.c
    src/gpu/bw_vsh_hlsl.c
    src/gpu/bw_gpu_audit.c    # BW_GPU_AUDIT=1: what the game asks of the GPU, counted
    src/gpu/bw_ab.c           # BW_GPU_AB: same-run A/B for GPU fixes (test aid)
    src/gpu/bw_gamma.c        # the title's DAC gamma ramp (D3DDevice_SetGammaRamp hook)
    src/gpu/bw_vfx.c          # Remastered visual suite (D-016): settings, render scale, helpers
    src/gpu/bw_vfx_post.c     # Remastered visual suite: the frame through its effects to the screen
    src/gpu/bw_texrestore.c   # Remastered texture restoration (T1, VFX_TEX_RESTORE)
    src/gpu/bw_pscache.c      # persistent pixel-shader cache (T1, every edition)
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)
endif()

if(BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK)
    list(APPEND GAME_SOURCES src/bw_macos_input_bridge.c)
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(APPLE)
    set(BW_METAL_SHADER_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/bw_metal_shaders.metal")
    set(BW_METAL_SHADER_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/metal_shaders")
    bw_prepare_metal_shaders(
        BW_METAL_SHADER_DATA
        "${BW_METAL_SHADER_SOURCE}"
        "${BW_METAL_SHADER_GENERATED_DIR}")
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    bw_attach_metal_shaders(
        ${PROJECT_NAME}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/bw_metal_backend.m"
        "${BW_METAL_SHADER_DATA}")
    if(BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK)
        target_compile_definitions(${PROJECT_NAME} PRIVATE BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=1)
    endif()
    if(BW_EXPERIMENTAL_SYNTHETIC_INPUT)
        target_compile_definitions(${PROJECT_NAME} PRIVATE BW_EXPERIMENTAL_SYNTHETIC_INPUT=1)
        target_compile_definitions(xbox_video PRIVATE BW_EXPERIMENTAL_SYNTHETIC_INPUT=1)
    endif()
    set_source_files_properties(
        src/gpu/bw_metal_backend.m
        src/bw_macos_settings_panel.m
        PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(src/bw_ocean_native.c PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-frounding-math")
else()
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

# The native ocean (docs/day1/P2a.md): tools/bw_ocean_xlate.py translates the
# water's render-vertex functions from the generated C into an include consumed
# by src/bw_ocean_native.c. Windows keeps it alongside the generated C; macOS
# puts it in the build tree. Rebuilt whenever the generated C or translator changes.
if(APPLE)
    # Keep verified prepared C read-only; the derived translation is local
    # to this build, not a new file under BW_GEN_DIR.
    set(BW_OCEAN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/bw_ocean")
    file(MAKE_DIRECTORY "${BW_OCEAN_GENERATED_DIR}")
    set(BW_OCEAN_NATIVE_INC "${BW_OCEAN_GENERATED_DIR}/bw_ocean_native.inc")
else()
    set(BW_OCEAN_NATIVE_INC ${BW_GEN_DIR}/bw_ocean_native.inc)
endif()
add_custom_command(
    OUTPUT ${BW_OCEAN_NATIVE_INC}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_ocean_xlate.py
            --gen ${BW_GEN_DIR} --out ${BW_OCEAN_NATIVE_INC}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/bw_ocean_xlate.py ${RECOMP_GEN_SOURCES}
    COMMENT "native ocean: translating the water's render-vertex functions"
    VERBATIM)
target_sources(${PROJECT_NAME} PRIVATE ${BW_OCEAN_NATIVE_INC})
set_source_files_properties(${BW_OCEAN_NATIVE_INC} PROPERTIES HEADER_FILE_ONLY TRUE)
set_source_files_properties(src/bw_ocean_native.c PROPERTIES OBJECT_DEPENDS ${BW_OCEAN_NATIVE_INC})

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${BW_GEN_DIR}                   # recomp_types.h, and the ocean include on Windows
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
if(APPLE)
    find_library(BW_APPKIT_FRAMEWORK AppKit REQUIRED)
    find_library(BW_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(BW_METAL_FRAMEWORK Metal REQUIRED)
    find_library(BW_QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(BW_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${BW_OCEAN_GENERATED_DIR}")
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        "${XBOXRECOMP_DIR}/src"
        "${XBOXRECOMP_DIR}/src/d3d"
        "${XBOXRECOMP_DIR}/src/kernel")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${BW_APPKIT_FRAMEWORK}" "${BW_FOUNDATION_FRAMEWORK}" "${BW_METAL_FRAMEWORK}"
        "${BW_QUARTZCORE_FRAMEWORK}" "${BW_COREGRAPHICS_FRAMEWORK}")
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        d3dcompiler     # src/gpu/bw_d3d11.c compiles its shaders at start-up
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
        winmm
        shell32         # src/bw_edition.c: SHGetFolderPathA, SHCreateDirectoryExA
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)

    # Every call to an undeclared function is an error in our own code too (a community
    # build failed: fabsf called without <math.h> links on some toolsets, not others).
    target_compile_options(${PROJECT_NAME} PRIVATE /we4013)

    # Blood Wake (D-010, TOOLKIT_ISSUES TK-3): Ghidra can name a guest function after
    # one of the CRT's legacy POSIX names (flushall, itoa, strlwr...), which the UCRT
    # headers also declare, and the build stops on C2371. The generated code never
    # calls those, so it doesn't get them declared. /we4013 makes any call that would
    # have relied on one a hard error, instead of an implicit int that truncates a
    # pointer on x64.
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "_CRT_DECLARE_NONSTDC_NAMES=0"
        COMPILE_OPTIONS "/we4013")
endif()

# Blood Wake (M0.7, FIRST_RUN_PLAN): record every indirect-branch target the
# game reaches, for the toolkit's feedback loop (docs/technical/indirect-calls.md).
# Both sides need the define: the generated code stores into the array, and the
# toolkit's kernel library (icall_feedback.c) defines it and writes the dump.
# Set from here so the toolkit's files stay untouched (D-010). Costs one byte
# store per indirect call; turn it off for a release-style build.
# Blood Wake (D-010): carried patches. A bug inside one of the toolkit's runtime
# libraries is fixed in a patched copy of that one source file under
# src/toolkit_patches/, compiled in place of the toolkit's copy. The toolkit's
# checkout stays untouched. Each entry checks the SHA-256 of the original it was
# made from, so a toolkit upgrade stops the build until the patch is re-checked
# (dropped if upstream fixed it, remade if not). The .diff beside each copy is
# the exact change.
function(bw_carried_patch target toolkit_rel_src original_sha256)
    set(_orig "${XBOXRECOMP_DIR}/${toolkit_rel_src}")
    # Hashed with LF line endings, so a git checkout (CRLF on Windows) and
    # GitHub's archive of the same commit (LF; tools/build_from_disc.py) agree.
    file(READ "${_orig}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(SHA256 _sha "${_text}")
    if(NOT _sha STREQUAL original_sha256)
        message(FATAL_ERROR "Carried patch for ${toolkit_rel_src}: the toolkit's copy changed "
                            "(sha256 ${_sha}). Re-check it against docs/TOOLKIT_ISSUES.md.")
    endif()
    get_filename_component(_name "${toolkit_rel_src}" NAME)
    get_filename_component(_dir "${toolkit_rel_src}" DIRECTORY)
    string(REGEX REPLACE "^src/" "" _sub "${_dir}")
    set(_patched "${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/${_sub}/${_name}")
    get_target_property(_srcs ${target} SOURCES)
    list(FIND _srcs "${_name}" _i)
    if(_i LESS 0)
        message(FATAL_ERROR "Carried patch: ${_name} is not a source of ${target}")
    endif()
    list(REMOVE_AT _srcs ${_i})
    list(APPEND _srcs "${_patched}")
    set_target_properties(${target} PROPERTIES SOURCES "${_srcs}")
    message(STATUS "Carried patch: ${target} builds ${_patched}")
endfunction()

# TK-13: OHCI resolved heap buffers named by MmGetPhysicalAddress into the
# contiguous window, so the pad's input reports never reached the game.
bw_carried_patch(xbox_usb "src/usb/ohci.c"
                 "640cd338c6fb6f1347cf7377429219989c22e4d46532bb0a193066fee3597723")

# Test input: live pad lines starting with '!' take effect at once instead of
# queueing behind a 200 ms release (tools/bw_autopilot2.py). Not a toolkit bug;
# a test-harness extension, carried like the fixes.
bw_carried_patch(xbox_usb "src/usb/usb_gamepad.c"
                 "36a29f6c38739bf1b8aa31f299fef483f1d0cc6fe03ef1e65c46de80ae512424")

if(APPLE)
    # v0.17 serializes controller state with SDL's joystick lock, but its guest
    # polling thread can still enumerate SDL2-compat's device-list cache while
    # the main thread handles hotplug. Keep discovery in the SDL event pump.
    bw_carried_patch(xbox_input "src/input/xinput_device.c"
                     "3a6192fbe452d89a695025c9ab86c0b49ffa69a00f2ce9ce7cf794ea95a99deb")
    # Keep the macOS mouse bridge and drive controller hotplug discovery from
    # the presenter's main-thread event pump.
    bw_carried_patch(xbox_input "src/input/keyboard_sdl.c"
                     "4c3d0c320f1e678fa3a1ae9db1052affe32f2ebe03489e14871a02af13e18bbe")
    bw_carried_patch(xbox_input "src/input/keyboard.c"
                     "cf4cc96718246341823e40e2ae276e49f1ec2fc550f2a115cabec81c1a1d1424")
    target_sources(xbox_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/bw_macos_mouse.c")
    target_include_directories(xbox_input PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${XBOXRECOMP_DIR}/src/input")
endif()

# APU standalone host integration and downstream audio diagnostics. v0.17
# preserves queued frames across the title's short sequencer restart, so the
# former TK-14 sample-limit interrupt suppression is no longer carried.
bw_carried_patch(xbox_apu "src/apu/apu_core.c"
                 "8d55ae0e734475e74a8ed130fa13a7898e6239b9619f89edd96e6b0110d6d1ce")

# TK-16: the stubbed DSP's mixdown (every mixbin at unity, even bins left, odd
# bins right). Diagnostics first (BW_APU_BINSTAT).
bw_carried_patch(xbox_apu "src/apu/apu_dsp.c"
                 "cff1bcb52ab2cad2019e803cb5ad96c5e47e52bce7a0f91731b71bcd33ff8d86")

# TK-17: one physical number can name a heap page and a window page; the APU
# read the heap copy of the music stream's window buffer (noise). The kernel
# records each translation's kind, and the APU tags posted buffer pages and
# stream segments with it.
bw_carried_patch(xbox_kernel "src/kernel/kernel_memory.c"
                 "caf09aa153563e0b8ddf55be27eb31beca450e3d6906926375d1b0b4f47e87d0")
bw_carried_patch(xbox_apu "src/apu/apu_vp.c"
                 "6e6916de6cee30424f11a4e51229a46368773c30b720da406a706eb2d59ce14e")

# Remastered surround (D-016, docs/day1/D2.md part 3): a 6-channel (5.1)
# source voice when the device has 6+ channels; stereo exactly as before
# otherwise. Not a toolkit bug; an output feature, carried like the fixes.
bw_carried_patch(xbox_apu "src/apu/apu_xaudio2.c"
                 "2ce2b436500784ac158fb2270190fd78f748c0b0ce9ca685f1d9e991f05f46c1")
if(APPLE)
    bw_carried_patch(xbox_apu "src/apu/apu_sdl2.c"
                     "50fb404a92489a66d34d59447c6e16bf07d24c393dcb0a5afbba3b41f1d710d2")
    target_sources(xbox_apu PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_audio_audit.c")
endif()

# D-003 B: the executor hands transformed batches to the GPU back end with
# everything its CPU combiner path reads (src/toolkit_patches/kernel/
# bw_backend_ext.h), so the back end can draw the register combiners.
bw_carried_patch(xbox_kernel "src/kernel/nv2a_pb_exec.c"
                 "d0671693d5d41ea71b3ac2ff631268d7ce19858ce10fad7289a6d33463c40552")

# Point sprites: the vertex-program interpreter hands out the program's point
# size (oPts), which it computed and dropped, so the executor can draw points
# as sprites (both renderers dropped every point; BW_GPU_AUDIT, 2026-10-27).
bw_carried_patch(xbox_kernel "src/kernel/nv2a_vsh_interp.c"
                 "af72a7f90beee744ebb34e70bbf14fd7f79bbe117f241dea7aae2d7f1272ed8a")

# The NV2A thread's loop ended each pass with Sleep(0), holding a whole core
# between frames; it now naps when idle and the KickOff replacement wakes it
# (src/bw_wait.c, bw_ack_idle / bw_ack_kick; BW_ACK_NAP=0 restores the spin).
bw_carried_patch(xbox_kernel "src/kernel/xbox_memory_layout.c"
                 "e61aab614c3eb77069bd84c0129b6e042337d6e075ba20a244644f3e0a9543d5")

# TK-38: the segment scan lost commands at every pushbuffer ring wrap (it
# guessed the ring's ends from the PUTs seen) and cut methods whose parameters
# ran past PUT. The ring walk follows the JUMP and keeps the split method
# (BW_PB_RINGWALK=0 restores the old scan; BW_PB_SEGSTAT=1 counts).
bw_carried_patch(xbox_kernel "src/kernel/nv2a_pb_scan.c"
                 "81edf44a0165f522e0395fca07775fa4e4a0dcd7b2fc573314fa12ec10450300")

# D-012: the GP/EP DSPs on xemu's DSP56300 interpreter (vendored in
# src/xemu_dsp, xemu v0.8.139, GPL-2.0-or-later), switched on with
# BW_APU_DSP=xemu; without it the stub's fold-down runs as before.
add_subdirectory(src/xemu_dsp)
target_sources(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolkit_patches/apu/bw_apu_xdsp.c)
target_link_libraries(xbox_apu PUBLIC bw_xemu_dsp)
# TK-36: PsCreateSystemThreadEx ignored CreateSuspended, so Bink's workers ran
# before their mutex and exit event existed (BW_THREAD_SUSPEND=0 restores).
# Also the D3 wait/handle diagnostics ([BWD3] lines; BW_D3_DIAG=0 silences).
bw_carried_patch(xbox_kernel "src/kernel/kernel_bridge.c"
                 "6bb3cb58c1edb84525a13afc9ad24af0f51ebb53d53ee760d2b421d57c260beb")
if(APPLE)
    target_sources(xbox_kernel PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bw_posix_guest_mem.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/bw_posix_ke_event.c")
endif()

# One window for a play session (src/bw_host.c): with BW_PARENT_HWND set, the
# frame window is a child of the launcher's host window, so the game's
# self-reboots no longer open a new window each.
bw_carried_patch(xbox_video "src/video/fb_present.c"
                 "ab93d83a3ad4122e388137676778944891a7d8c56cca3eafc76eb12efaca10e6")

if(APPLE)
    bw_carried_patch(xbox_video "src/video/fb_present_sdl.c"
                     "61fa4711b3965b65ef3b80bbb71fb47a4b50ecbef454009d340ec83e5eaf4b12")
endif()

option(BW_ICALL_FEEDBACK "Record indirect-call targets (icall_targets.dump)" ON)
if(BW_ICALL_FEEDBACK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_ICALL_FEEDBACK)
    target_compile_definitions(xbox_kernel PRIVATE RECOMP_ICALL_FEEDBACK)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
