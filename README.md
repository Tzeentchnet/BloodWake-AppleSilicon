# Blood Wake Recomp for Apple Silicon

An experimental native macOS/Apple Silicon fork of
[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp), built
with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a Metal
renderer. It runs a user-provided retail copy of Blood Wake through static
recompilation.

This project is not affiliated with the game's or hardware's rights holders.

## Current status

The current arm64 build boots into the game and has been user-confirmed for
menu navigation and interactive play. The current verified direct-Metal
configuration uses 4x scaling to render into 2560x1920 private colour and
depth targets, then presents at 2560x1920. The guest framebuffer contract
remains 640x480, but that is not the current internal rendering or
presentation resolution.

This is still a development build:

- The Metal path now presents directly through `CAMetalLayer` and can render
  into private, output-scaled colour and depth targets. The latest interactive
  4x run reached 3,963 post-relaunch flips and 1,747,600 draws, then exited
  normally when the window closed. No sticky Metal or command-buffer error
  was logged.
- The game-specific keyboard bridge and Metal quincunx approximation require
  explicit build or runtime opt-ins.
- F1 now opens a native settings panel. It applies proven-live LOD, Metal
  filtering, the optional realistic-water preset, and mouse controls through
  subsystem-owned callbacks, labels restart-required choices, and disables
  unsupported Metal settings with a reason.
- A versioned in-stream marker now identifies validated ocean submissions and
  is copied into executor batches without changing retail rendering. The
  protocol can now represent water-view begin/end plus lit, reflection, and
  foam draws and persistent pass runs, carries pass/view identity separately
  from fingerprints, and still accepts the old lit marker. Validated
  Remastered wrappers now bracket each title ocean view and every lit,
  reflection, and foam submission group; Original and invalid contexts execute
  the title body without markers. A post-fix one-player Battle run recorded
  186,893 scopes for each pass, 2,915 balanced view-0 pairs, zero rejection,
  and clean fingerprints. Metal private reflection depth also activated with
  no fallback, and the 337-test Metal suite passes. Split-screen/view-1
  validation is deferred. Earlier lit-only diagnostics observed
  41,348 markers and 512 clean scopes in one four-boot run; a later
  deterministic run observed 12,670 markers, 154 clean scopes, and 297
  fingerprinted batches across 10 aggregate hashes with no rejection,
  overflow, or within-scope mismatch. Representative calm/storm, lighting,
  shore, split-screen, reflection, and wake coverage remains outstanding.
  Remastered now also has an experimental, default-off
  realistic-water preset. It selects a separate Metal fragment pipeline only
  for supported marked ocean batches, preserves retail alpha, and falls back
  per batch; Retail remains the exact default for every unmarked or
  unsupported draw. A private four-boot scripted run activated the preset on
  real ocean batches with no observed fallback or Metal error. A later
  diagnostic-only same-scene ABBA capture completed two independent
  100-Retail/100-Realistic GPU samples. Realistic increased water-containing
  command-buffer mean time by 4.548%-4.822%, median by 1.865%-3.570%, and p95
  by 7.787%-10.092% in that exercised scene. This is not isolated shader cost
  or a visual-quality judgment. Initial hands-on review found the treatment
  almost identical to Retail, and separate bounded captures were not
  sufficiently phase-aligned for pixel A/B evidence. The dedicated
  [water-rendering dossier](docs/WATER_RENDERING.md) explains why the current
  pass-1-only treatment is subtle, maps the title's later Retail reflection
  and foam passes, and defines mechanics-safe next steps. The preset remains
  experimental and default-off. A fail-closed MSAA evidence tool records
  memory, timing,
  correctness, and visual-review gates. This Apple M5 reports
  2x/4x/8x capability and passes the standalone color/depth/resolve smoke
  path, but Metal MSAA remains unsupported.
- Optional gameplay mouse input supports sensitivity, inversion, response,
  steering/aim stick selection, and hold/toggle relative capture. Native
  point-and-click menu hitboxes are still a planned reverse-engineering task.
- The quincunx approximation is not verified against NV2A hardware.
- Native guest audio now uses deferred same-thread APU MMIO dispatch and SDL
  output. Its dispatch, startup, failure, and shutdown paths have synthetic
  and sanitizer coverage. A recorded CoreAudio run reached 427,500 APU DSP
  frames and 53,436 EP output frames with no SDL underrun or deferred-dispatch
  error logged. Audible correctness and output quality have not yet been
  explicitly confirmed. A diagnostic-only headless sink can preserve guest
  audio timing when CoreAudio is unavailable; it retains no samples, reports
  aggregate signal/cadence/queue metrics, and never claims that sound is
  audible.
- USB gamepads, physical-controller support, and broad mission, save, FMV,
  and edition coverage are incomplete.
- Current regression results include 375/375 Metal backend tests, all 62
  presenter CTest entries (including the real-window AppKit case in the
  current WindowServer session), 35/35 MMIO host tests, 8/8 normal and ASan/UBSan
  macOS host tests, 11/11 input integration tests, 6/6 APU/audio tests,
  13/13 support tests, and 100/100 concurrent APU restore stress runs. The
  complete Python suite passes 117/117.
- A post-roadmap 30-second isolated retail boot reached its configured limit
  with direct Metal at 2560x1920, native audio/DSP activity, 1,490 audited
  flips, 4,503 draws, 1,252,771 vertices, unchanged input trees/executable,
  and no sticky Metal error. This is automated integration evidence, not a
  hands-on F1/mouse/quality or audible-output check.
- A built-in poll-epoch Remastered script now provides deterministic,
  diagnostic-only menu-to-gameplay navigation without OS keystrokes. Combined
  with the headless audio sink, it completed three private boots and two
  validated relaunches, reached ocean rendering, and stopped at the configured
  boot limit. Default physical input and CoreAudio behavior are unchanged.
- Bounded Metal ownership now keeps at most three submissions in flight,
  gives mutable upload, feedback-resolve, and readback resources to their
  submission, keeps exact tracked XRGB feedback GPU-private while guest bytes
  remain unchanged, and lets safe target switches share one ordered command.
  Immutable NV2A texture-stage and combiner routing is specialized with Metal
  function constants while material values remain dynamic. Two exact
  Native/auto, match-display boots selected 2560x1920 and produced
  post-warm-up windows at 59.98-60.01 FPS, 16.66-16.68 ms wall medians,
  14.18-14.84 ms GPU medians, and 14.87-15.26 ms GPU p95. Two fixed-4x,
  cap-120 boots independently produced consecutive post-warm-up windows above
  60 FPS without changing scale, filtering, LOD, draw coverage, quincunx
  behavior, or Retail-water defaults. Representative sustained Native 4x/60
  is therefore claimed for the measured Apple M5 Battle workload. It is not
  yet a universal locked-60 claim: exact-default wall p95 remains
  17.06-17.46 ms, cold windows contain 296-330 ms shader/workload warm-up
  stalls, and broader mission, device, power, temperature, and formal visual
  coverage remain open.

## Current performance priorities

The steady-state representative target is closed. Work now proceeds in this
order:

1. Qualify two clean cold exact-default gameplay boots with the validated
   offline-library, device-local preload, and exact asynchronous rare-miss
   path. Missing or rejected artifacts retain synchronous runtime creation.
   Retail-derived traces and archives remain private.
2. Obtain representative gameplay evidence from the new bounded presentation
   telemetry before evaluating a gated `CAMetalDisplayLink` match-display
   path. A paused-renderer smoke validated real drawable, command-completion,
   presented-callback, and explicit-unavailability handling, but supplied no
   usable actual-present or display-target timestamp. The current path remains
   the fallback.
3. Qualify cold start, frame pacing, missions, split-screen, heavy effects,
   long-duration behavior, and available Apple Silicon device classes with
   direct power and temperature evidence where approved.
4. Profile any failing workload. Port NV2A vertex programs to Metal only when
   a miss is demonstrably CPU-bound; use Metal counters to guide any
   additional GPU-pass work.

Lower scale, dynamic resolution, upscaling, frame generation, reduced
filtering or LOD, changed Retail water, inexact quincunx, and unmeasured MSAA
are not Native 4x/60 optimizations. The active dependency-gated work is in
[PERFTODO.md](PERFTODO.md); measured evidence and rejected experiments are in
[PERFTESTS.md](PERFTESTS.md).

See [Apple Silicon status](docs/APPLE_SILICON_HANDOFF.md),
[work completed](docs/WORK_COMPLETED.md), and
[validation](docs/VALIDATION.md) for details.

## Requirements

- An Apple Silicon Mac
- Xcode Command Line Tools
- CMake 3.20 or newer
- SDL2 2.0.14 or newer
- OpenSSL 3
- Python 3.13 and Git
- Your own North American retail Blood Wake disc image or extracted disc

The repository does not include or download the game.

## Prepare and build

From the repository root:

```sh
git config core.hooksPath tools/git-hooks
python3.13 -m venv build/venv
build/venv/bin/python -m pip install capstone==5.0.9

git clone https://github.com/sp00nznet/xboxrecomp.git build/xboxrecomp
git -C build/xboxrecomp checkout --detach f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b

build/venv/bin/python tools/build_from_disc.py \
  --disc "/path/to/your/disc.iso" \
  --install-dir game/prepare \
  --toolkit build/xboxrecomp \
  --prepare-only

cmake -S . -B build/bloodwake-macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DXBOXRECOMP_DIR="$PWD/build/xboxrecomp" \
  -DBW_GEN_DIR="$PWD/game/prepare/work/gen" \
  -DPython3_EXECUTABLE="$PWD/build/venv/bin/python" \
  -DBW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON

cmake --build build/bloodwake-macos --target bloodwake --parallel
```

If CMake cannot find Homebrew SDL2 or OpenSSL:

```sh
export CMAKE_PREFIX_PATH="$(brew --prefix sdl2):$(brew --prefix openssl@3)"
```

Run the prepared build:

```sh
BW_GAME_DIR="$PWD/game/prepare/game/disc" \
RECOMP_KEYBOARD=1 \
BW_MACOS_KEYBOARD_HOOK=1 \
BW_MOUSE=1 \
BW_METAL_QUINCUNX_APPROX=1 \
BW_METAL_RENDER_SCALE=auto \
./build/bloodwake-macos/bloodwake
```

Detailed preparation, alternate install, diagnostic, and Windows instructions
are in [BUILDING.md](docs/BUILDING.md).

## Documentation

- [BUILDING.md](docs/BUILDING.md) - build, run, and diagnostic instructions
- [APPLE_SILICON_HANDOFF.md](docs/APPLE_SILICON_HANDOFF.md) - current status
  and remaining priorities
- [WORK_COMPLETED.md](docs/WORK_COMPLETED.md) - implementation history
- [VALIDATION.md](docs/VALIDATION.md) - performance evidence, tests, and
  integrity checks
- [SETTINGS.md](docs/SETTINGS.md) - runtime and edition settings
- [UPSTREAM_ISSUES.md](docs/UPSTREAM_ISSUES.md) - portability findings
- [NATIVE_FEATURE_ROADMAP.md](docs/NATIVE_FEATURE_ROADMAP.md) - F1, mouse,
  modding, quality, upstream, and water work plan
- [METAL_QUALITY.md](docs/METAL_QUALITY.md) - LOD/filtering behavior, the
  realistic-water GPU benchmark, and the Metal MSAA go/no-go gate
- [METAL_PIPELINE_ARTIFACT.md](docs/METAL_PIPELINE_ARTIFACT.md) - versioned
  device-local Metal pipeline-artifact compatibility and rejection contract
- [WATER_RENDERING.md](docs/WATER_RENDERING.md) - Blood Wake water simulation
  boundaries, three-pass Retail renderer, Metal shader inputs, and safe future
  work
- [NEWWATER.md](NEWWATER.md) - modern water architecture and phased
  implementation plan
- [NEWWATER_TODO.md](NEWWATER_TODO.md) - dependency-aware modern water task
  tracker and validation status
- [LIGHTING_RENDERING.md](docs/LIGHTING_RENDERING.md) - current title/NV2A/
  Metal lighting path, available shader semantics, and staged real-time
  lighting, PBR, and renderer-replacement options
- [GPU_CAPTURE.md](docs/GPU_CAPTURE.md) - synthetic capture fixture
- [GUEST_MEMORY_INVENTORY.md](docs/GUEST_MEMORY_INVENTORY.md) - memory-access
  inventory

## Privacy and legal

Keep disc images, unpacked files, generated C, binaries, saves, logs, and
captures private. Do not commit or share game-derived material. See
[LEGAL.md](LEGAL.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

Blood Wake is by Stormfront Studios and was published by Microsoft Game
Studios. This fork builds on Blood Wake Recomp, xboxrecomp, and xemu. See
[CREDITS.md](CREDITS.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE), and
[LICENSES/](LICENSES/) for attribution and licence details.
