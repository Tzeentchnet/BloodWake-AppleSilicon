# Upstream and portability notes

These are local findings, not submitted issues or claims of game compatibility.
Reproduce any upstream report with synthetic inputs; do not attach game files,
generated C, private logs, captures or screenshots.

## xboxrecomp

Baseline: commit `f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b`, macOS
27.0.1 ARM64, Apple Clang 21. Using the installed macOS 26.5 SDK, Homebrew
OpenSSL, SDL2 and libepoxy, its runtime libraries built successfully. Its
synthetic ARM64 MMIO, POSIX memory-layout, POSIX kernel-memory and NV2A backend
tests passed. These checks do not establish game compatibility.

### MMX multiply-add signed overflow

**Reproduced with synthetic input; candidate for an upstream fix.**
[`MMX_PMADDWD`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/templates/runtime/recomp_types.h)
adds two signed 32-bit products. When both adjacent lanes of both operands
are -32768, each product is 1073741824, and their sum overflows `int32_t`.
Apple Clang's undefined-behavior sanitizer reports signed integer overflow
at optimization level `-O1`; without the sanitizer, the same synthetic case
produced the expected x86 result, `0x80000000`. That apparent result does not
make the signed overflow defined, and this is not limited to Apple Silicon
by the C language rules. Runtime impact on this game has not been established.

An upstream regression should exercise both output lanes, ordinary signed
inputs and the all-minimum edge case with UBSan enabled. One possible fix is
to sum in `int64_t` and convert to the result's unsigned 32-bit lane, which
gives the instruction's wrapping bit pattern without signed overflow.

### MMX integer test cannot build on ARM64

**Reproduced test configuration issue.** The toolkit's
[`tests/mmx_integer/CMakeLists.txt`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/tests/mmx_integer/CMakeLists.txt)
adds `-mmmx -msse` for every non-MSVC compiler. Apple Clang rejects both flags
for ARM64, so the test cannot build there. Its non-x86 fallback prints that
it is skipped but returns zero, which CTest reports as a pass. Gate the
intrinsic flags by architecture and mark the fallback as an actual CTest
skip, or add architecture-independent lane-value tests. This is a test
portability issue, not evidence that the game runtime fails.

### Metal integration: OpenGL is mandatory at configuration time

**Feature request, confirmed build constraint.** On non-Windows platforms
[`src/d3d/CMakeLists.txt`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/src/d3d/CMakeLists.txt)
always requires libepoxy and builds the OpenGL D3D8 backend. Configuration
failed when libepoxy was absent and succeeded after it was installed. Even a
consumer intending to supply a Metal backend cannot omit this dependency with
the current CMake options. Consider an explicit backend selection or a
separable backend target; SDL2 is also used by other toolkit components.

### Render backend callbacks cannot return failure

**Confirmed interface constraint; candidate for an upstream API proposal.**
The pinned toolkit's
[`Nv2aBackend`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/src/kernel/nv2a_backend.h)
clear, draw and flip callbacks return `void`. After calling draw, the
[`pushbuffer executor`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/src/kernel/nv2a_pb_exec.c)
counts and clears the batch without receiving a success or failure result.
An unsupported operation or GPU error therefore cannot be propagated through
this interface. The local synthetic Metal backend records a sticky error that
its test harness checks; it is not enabled for the game.

A versioned error-reporting interface could let a host stop or report a
failed frame. A per-draw CPU fallback would additionally need a defined
surface ownership and synchronization contract; merely returning failure
would not make such a fallback correct.

### Framebuffer snapshot precedes backend flip

**Reproduced with synthetic input; candidate for an upstream fix.** At
`FLIP_STALL`, the pinned toolkit's pushbuffer executor calls
`xbox_FramebufferWindowPresent` and, when enabled, writes the flip dump before
calling the registered backend's `flip`. A backend that copies its finished
frame into guest memory during `flip` therefore updates that memory after the
window snapshot and dump have read it. A synthetic backend confirmed that the
window callback saw the old value while guest memory contained the new value
after `flip`. Backends that present independently or update guest memory
earlier may not be affected.

An upstream regression can use synthetic guest memory and a backend that
writes a distinctive value during `flip`, then assert that the snapshot and
dump observe the new value. Fixing the ordering must preserve CPU-renderer
behavior and avoid presenting twice.

### macOS video decoding

**Known feature gap, not a newly reported defect.** The toolkit documents that
the non-Windows framebuffer window exists but the shared FMV player has no
decoder on those platforms. Determine the title's actual needs with private
local testing before proposing a platform-independent or macOS solution.

### Replay placeholder warning

**Candidate only; runtime impact not established.** Apple Clang warned that
the default one-element `menu_pushbuffer_data` placeholder is indexed at
7455 in
[`nv2a_pb_replay.c`](https://github.com/sp00nznet/xboxrecomp/blob/f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b/src/nv2a/nv2a_pb_replay.c).
The replay branch is guarded by a separate menu buffer's length, and the
default menu buffers are empty. Before reporting this as a bug, reproduce
the reachable case using synthetic buffers and check the intended replay API.

### Existing Blood Wake patches

The upstream Blood Wake project already carries numerous toolkit fixes in
[`src/toolkit_patches/`](../src/toolkit_patches/) and
[`tools/xbr_patches.py`](../tools/xbr_patches.py). Assess each against the
toolkit's current revision and prepare a minimal game-independent regression
before proposing a separate PR. The patches have not all been validated on
macOS.

## Blood Wake Recomp

### Mixed constant-color and constant-alpha blend factors

**Code-inspection candidate; Windows reproduction is still needed.** The
D3D11 renderer maps both NV2A constant-color and constant-alpha factors to
D3D11's blend constant. If either factor requests constant alpha, the
renderer replaces the constant's RGB components with its alpha component.
When the other factor simultaneously requests constant color and those
components differ, it consequently uses alpha where the NV2A CPU blend
equation uses RGB. It is not known whether Blood Wake uses this combination.

A synthetic regression can use source factor `0x8001` (constant color),
destination factor `0x8004` (one minus constant alpha), and a blend constant
such as `0x604080C0`, then compare the D3D11 output with the executor's CPU
blend equation. The local Metal test covers this combination; it does not
establish Windows behavior or a game-visible defect.

### Generated fingerprints depend on Windows line endings

**Reproduced on macOS; candidate for an upstream PR.** The pipeline completed
with the pinned toolkit and Python 3.13, but the upstream verifier rejected
all 37 generated files. Their hashes all matched the published values when LF
was converted to CRLF for comparison; the generated files themselves were not
changed. This fork keeps exact-byte verification on Windows and retries the
comparison with Windows line endings on POSIX. Tests also check that changed
content, missing or extra files, and Windows byte mismatches remain failures.

The fingerprint's recorded input hashes for five unchanged C sources also
did not match the clean upstream checkout in either LF or CRLF form. Those
hashes are diagnostic hints rather than a condition for accepting generated
output; investigate whether the public fingerprint metadata should be
refreshed before reporting that separately.

### Preparation without Visual Studio

**Feature proposal.** The existing script supports checking a disc but requires
Visual Studio before it will unpack and translate it. This fork adds
`--prepare-only` to run and verify the portable steps without claiming an
executable was built. It also works as a diagnostic step on Windows.

### Commit guard executable bit

At upstream commit `0bee18112cf0fd28373b3addd9bc83cc137acd0f`,
`tools/git-hooks/pre-commit` is tracked with mode `100644`. On macOS Git skips
a non-executable hook even after `core.hooksPath` is configured, so the intended
guard does not run. This fork changes the mode to `100755`; a temporary-repo
test checks that Git invokes it and refuses a synthetic `.iso` file. The fork
also protects ignored build-output directories, because toolkit analysis
created there can contain game-derived data even when it is not generated C.
Candidate for a small upstream PR; no game data is needed.

### Missing DSP trace shim

The DSP interpreter calls `trace_dsp56k_execute_instruction_disasm`, but the
project's no-op trace shim did not define it. Apple Clang rejects the undeclared
function even though tracing is disabled. This fork adds the missing no-op,
matching the other disabled trace events, and builds the DSP library with Apple
Clang. Candidate for an upstream PR; Windows toolchain validation is still
needed.

### Host and renderer

The game entry point, window host and D3D11 renderer include Windows headers and
use Windows APIs. Four carried APU source files now compile individually on
macOS, and a synthetic SDL dummy-device test checks their stereo output
adapter. The complete carried APU still fails to build because its DSP
integration uses Windows-specific APIs. Its worker synchronization and
lifetime need a real portable implementation, not no-op substitutes. These
are porting tasks in this fork, not defects proven in the Windows build.

## Local toolchain note

The machine's default macOS 27 SDK failed even a CMake compiler link test:
the installed linker rejected an `arm64e.x1` architecture in its SDK stub.
Selecting its already-installed macOS 26.5 SDK allowed the baseline toolkit
build and tests to run. This is a local SDK/toolchain mismatch, not an
xboxrecomp issue.
