# Apple Silicon validation

Updated 2026-10-31. This document records measured behavior and regression
coverage. Implementation history is in
[WORK_COMPLETED.md](WORK_COMPLETED.md), and current priorities are in
[APPLE_SILICON_HANDOFF.md](APPLE_SILICON_HANDOFF.md).

## Playable retail run

The current arm64 build completed an isolated two-boot retail run with the
experimental keyboard bridge and quincunx approximation enabled.

- Boot 2 sustained the `attract_01` scripted 3D workload for 23.7 seconds.
- The run produced 607 flips, 319,133 draws, and 44,934,511 vertices at a
  640x480 presentation size.
- Three active rolling 300-frame windows measured 29.99, 29.96, and 30.00 FPS
  using monotonic host time.
- Median frame times were 32.73, 33.27, and 33.27 ms.
- p95 frame times were 45.68, 45.59, and 45.76 ms.
- The worst active-window interval was 58.45 ms.
- The first window included a 2.71-second loading stall and averaged 23.62
  FPS.
- The title exited normally through `HalReturnToFirmware` routine 2 with no
  sticky Metal or command-buffer error.

The older `[GPU] ... fps` line uses C `clock()` CPU time and is not accepted as
wall-clock performance evidence.

## Hands-on confirmation

- The guest completed device initialization, enumeration, controller open,
  device-change queries, and continuous state polling with the keyboard bridge
  enabled.
- A synthetic diagnostic verified that a guest state query observed Start,
  Down, and A.
- The user subsequently confirmed menu navigation, entering the game, and
  interactive play with the current input-enabled build.
- Physical gamepads, feedback, reconnect behavior, focus-loss recovery, and
  the complete input lifecycle remain unverified.

## Current regression coverage

- The Metal backend suite passes 298/298 tests, including texture-stage masks,
  combiner operations, cube reflection, mip selection, command-buffer and
  render-encoder batching, texture caching, and upload-arena reuse.
- Focused Metal cases have also passed with ASan/UBSan and Metal validation.
- The diagnostic runner passed 45 Python tests, including bounded execution,
  isolation, relaunch validation, and output handling.
- Host, presenter, virtual-input, and audio/DSP suites passed their normal and
  focused sanitizer runs; presenter and input also passed focused TSan runs.
- The MMIO host suite reached 34/34 with the synthetic retirement model.
  Counterexample tests demonstrate unsafe behavior; passing them does not mean
  production retirement is safe.
- The DMA ownership suite passed 13 tests normally and under focused TSan and
  ASan/UBSan runs.
- The guest-memory inventory passed 21 synthetic tests, and the broader Python
  suite passed 66/66 at the recorded checkpoint.
- The synthetic GPU capture suite passed 11 tests normally and under
  ASan/UBSan, with four focused TSan cases.
- Windows and x86 execution were not validated on the Apple Silicon host.

## Integrity and privacy

- The coordinated diagnostic verified all 37 generated files against the
  normalized generated-code fingerprint and all 450 prepared retail files
  against the disc fingerprint.
- Preparation, toolkit, selected source, and executable hashes were recorded
  around coordinated builds and runs. Hashes do not exclude transient changes
  or replace source control.
- Disc images, unpacked assets, generated code, binaries, saves, logs, and
  captures are private game-derived material and must not be committed or
  shared.
- The working SDK for the recorded run was
  `/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.

## Running the regression suites

With CMake, SDL2, and the pinned toolkit available:

```sh
cmake -S tests/metal_backend -B build/test-metal-backend
cmake --build build/test-metal-backend
ctest --test-dir build/test-metal-backend --output-on-failure

cmake -S tests/metal_smoke -B build/test-metal-smoke
cmake --build build/test-metal-smoke
ctest --test-dir build/test-metal-smoke --output-on-failure

cmake -S tests/apu_output -B build/test-apu-output
cmake --build build/test-apu-output
ctest --test-dir build/test-apu-output --output-on-failure

cmake -S tests/input_integration -B build/test-input-integration
cmake --build build/test-input-integration
ctest --test-dir build/test-input-integration --output-on-failure

cmake -S tests/macos_presenter -B build/test-macos-presenter
cmake --build build/test-macos-presenter
ctest --test-dir build/test-macos-presenter --output-on-failure

cmake -S tests/macos_host -B build/test-macos-host
cmake --build build/test-macos-host
ctest --test-dir build/test-macos-host --output-on-failure

cmake -S tests/mmio_host -B build/test-mmio-host
cmake --build build/test-mmio-host
ctest --test-dir build/test-mmio-host --output-on-failure

build/venv/bin/python -m unittest discover -s tests -p 'test_*.py'
```

For a toolkit checkout outside `build/xboxrecomp`, pass
`-DXBOXRECOMP_DIR=/path/to/xboxrecomp` when configuring the relevant CMake
suite. Input tests use isolated virtual controllers and skip when a physical
controller is connected.

For a bounded retail run, use the
[isolated native diagnostic](BUILDING.md#isolated-native-diagnostic). Keep its
output private.
