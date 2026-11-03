# Apple Silicon validation

Updated 2026-11-02. This document records measured behavior and regression
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
- On 2026-11-01, a direct-Metal real-window run selected a 4x internal target
  (2560x1920). The user reported no visual issues during gameplay; no sticky
  Metal or command-buffer error was logged, and the process exited with status
  zero after the window was closed.
- That 4x run had no sound because it predated the native APU dispatch
  implementation. It does not indicate a presentation/scaling audio
  regression, but it is also not evidence that the new audio path is audible.
- Physical gamepads, feedback, reconnect behavior, focus-loss recovery, and
  the complete input lifecycle remain unverified.

## Native audio validation

- The native host now initializes the real carried APU and 48 kHz stereo SDL
  output by default, then enables APU-only deferred fault dispatch. Startup
  fails explicitly if audio-device initialization fails.
- A protected-page regression executes real ARM64 APU stores and loads through
  the production bridge. It verifies recursive lock acquisition on the
  original guest thread, integer/SIMD/flags/signal-mask preservation, disabled
  forwarding, and 1,600 concurrent callback dispatches.
- Deferred stacks remain permanently assigned to their APU-touching thread
  while installed, so a second thread cannot overwrite a signal frame that
  `_sigtramp` is still returning through. The concurrent callback case passed
  100/100 separate stress iterations after this ownership rule was added.
- Native lifecycle fixtures cover default-on audio, explicit disable, invalid
  values, rejection of direct presence-based toolkit control, APU startup
  failure, and disabling dispatch before APU shutdown and memory teardown.
- Focused production fault and lifecycle cases pass normally and under
  ASan/UBSan and TSan. The nested fail-closed case retains its existing TSan
  skip where the sanitizer blocks nested fault delivery; unrelated deliberate
  detached-monitor and run-limit cases are excluded from the TSan lifecycle
  fixture.
- The real native Release target links successfully with the APU bridge. A
  20-second bounded retail boot selected direct Metal at 2560x1920 and 4x
  internal scale, initialized CoreAudio at 48 kHz stereo, started the APU and
  both DSP programs, delivered APU interrupts, and reached 22,500 reported DSP
  frames before the intentional run limit. It logged no deferred-dispatch or
  Metal error and one SDL underrun. Exit status 124 was the expected
  `BW_RUN_SECONDS` termination.
- Audible retail output, sustained gameplay, underruns, latency, and channel
  quality have not yet been confirmed; the bounded boot establishes device and
  guest activity, not audible correctness.
- A diagnostic-only headless sink now keeps the title's APU/DSP path and
  48 kHz stereo cadence active without opening CoreAudio. It retains no sample
  data and is accepted only through the bounded native diagnostic runner; it
  cannot be combined with disabled audio or sample-data writers.
- In the scripted three-boot Remastered run, periodic audit reports observed
  nonzero RMS and peaks in both channels with no clipping, queue overflow, or
  internal audit failure. The analytical queue did record
  scheduling underflows. These aggregates validate generated signal and
  accounting only; they do not validate audible output, device latency, or
  sound quality.

## Current regression coverage

- The Metal backend suite passes 320/320 tests, including texture-stage masks,
  combiner operations, cube reflection, mip selection, command-buffer and
  render-encoder batching, texture caching, upload-arena reuse, anisotropy
  selection/lifecycle, optional-water frame-boundary selection, exact default
  Retail output, marked-only enhancement, per-pass fallback, edition gating,
  and invalid startup rejection.
- Eight dedicated scaling cases cover forced 2x private targets, guest-AA
  collapse, exact guest seeding, weighted box-filter readback, scaled colour
  and depth scissors, invalid scale rejection, SSAA minimum handling, and a
  live 1x-to-2x resize with depth/stencil preservation.
- The macOS presenter suite has 62 CTest entries, all of which pass in the
  current WindowServer session. The real-window AppKit integration case skips
  explicitly where WindowServer is unavailable. Coverage includes direct
  Metal attachment, resize
  propagation, confirmation that direct mode skips CPU gamma and SDL texture
  upload, F1 down/repeat/up consumption, guest-input and relative-mouse
  release, main-thread panel/controller lifecycle, live LOD, frame-boundary
  filtering and water selection, restart-pending state, atomic save, and
  visible apply/save errors.
- The input integration suite passes 11/11 tests, including gameplay mouse
  capture, one-shot delta consumption, sensitivity/inversion/stick mapping,
  focus-loss clearing, keyboard/controller coexistence, hotplug, slots,
  concurrent polling, and deterministic synthetic-script packet edges,
  completion, cancellation, and reselection.
- The hook-portability suite passes 9/9 tests, including the validated ocean
  marker boundary and retail fallback. Metal backend coverage verifies
  marked/unmarked batches, 4,096-marker coalescing into one scope,
  conflicting/unknown rejection, no carry-over, and byte-identical retail
  output.
- The MSAA evidence harness passes 9/9 Python tests covering 2x/4x/8x memory
  costs, strict schemas, non-finite and overflow rejection, missing evidence,
  and exact decision-threshold boundaries.
- The Metal MSAA capability target passes 2/2 tests. The recorded Apple M5
  reports 2x, 4x, and 8x support; the probe explicitly retains the unsupported
  project policy.
- The standalone Metal MSAA smoke target passes 2/2 tests. The recorded Apple
  M5 passes synthetic 2x/4x/8x color, depth/stencil, resolve, readback, and
  explicit error checks; this is not game-renderer or visual evidence.
- The focused macOS diagnostic suite passes 43/43 tests, including strict
  edition and built-in script selection, bounded marker/fingerprint summary
  parsing, typed relaunch outcomes, integrity-failure precedence, boot-cap
  reporting, explicit audio isolation, and strict headless-audit authorization
  with default audio preserved. It also covers strict water-preset propagation,
  inherited-value removal, edition gating, and activation/fallback reporting.
- Nine LOD cases cover Original passthrough, defaults, disable/fix override,
  custom/clamped values, live changes, invalid rejection, and concurrent
  readers/writers.
- The standalone Metal smoke test passes, and the Release native `bloodwake`
  target builds successfully with the direct presenter and scaled-target path.
- Focused Metal cases have also passed with ASan/UBSan and Metal validation.
- The macOS host suite passed 8/8 normally and under ASan/UBSan. Under TSan,
  seven cases passed and the existing nested-signal case was skipped because
  the sanitizer blocks that delivery. The expanded APU/audio suite passed 6/6
  normally, under ASan/UBSan, and under TSan.
- The support/settings suite passes 13/13. Both the keyboard-experiment and
  synthetic-input native Release targets build successfully.
- Host, presenter, virtual-input, and audio/DSP suites passed their normal and
  focused sanitizer runs; presenter and input also passed focused TSan runs.
- The new production APU fault and native audio lifecycle cases pass normal,
  focused ASan/UBSan, and focused TSan runs.
- The MMIO host suite reached 35/35 with the synthetic retirement model.
  Counterexample tests demonstrate unsafe behavior; passing them does not mean
  production retirement is safe.
- The DMA ownership suite passed 13 tests normally and under focused TSan and
  ASan/UBSan runs.
- The guest-memory inventory passed 21 synthetic tests, and the complete
  Python suite now passes 112/112.
- The synthetic GPU capture suite passed 11 tests normally and under
  ASan/UBSan, with four focused TSan cases.
- Windows and x86 execution were not validated on the Apple Silicon host.
- The offline upstream report passes with all 42 fixes, 15 settings fix
  references, and 10 native fix sources matching the recorded `e631edd`
  baseline.

The direct-present and scaled-target paths now also have hands-on 4x visual
confirmation. Formal reference comparison and controlled frame-time and
memory measurements remain outstanding, so no scaled-performance claim is
recorded yet.

After the runtime-settings, LOD/filtering, and mouse changes, a fresh
keyboard-enabled Release executable completed a 30-second isolated
Original-edition retail boot:

- Direct Metal attached at 2560x1920.
- Native audio initialized and periodic APU/DSP output remained active.
- The 25-second GPU audit recorded 1,490 flips, 4,503 draws, 1,252,771
  vertices, and 1,490 clears.
- The executable reached `BW_RUN_SECONDS` intentionally, with no sticky Metal
  or command-buffer error.
- Prepared-tree, toolkit, and executable integrity were unchanged.

That bounded boot did not interact with F1, enable Remastered LOD/anisotropy,
capture the mouse, judge rendered output, or confirm audible sound. Those
remain hands-on validation items.

A later private four-boot Remastered diagnostic followed validated relaunches
until the configured boot cap. Prepared-tree, toolkit, and executable integrity
were unchanged. It aggregated 41,348 ocean markers into 512 scoped batches
with zero rejections. This validates the coalescing and one-shot executor
lifetime in the exercised boot flow; representative calm/storm, day/night,
shore, split-screen, reflection, and wake coverage remains outstanding.

After adding marker-only metadata fingerprints, two audio-enabled private
attempts stopped before guest execution because CoreAudio could not start. An
audio-disabled boot succeeded but changed title progression and did not reach
an ocean scene. The diagnostic-only headless audio sink then preserved guest
audio timing while a built-in poll-epoch script navigated Remastered from boot
through menu input into gameplay:

- Three boots completed and two validated same-title relaunches were followed.
- The run stopped at its intentional boot limit with exit status 124.
- The script completed its distinct Start, D-pad Down, and A press/release
  sequence in the relevant boots without OS-generated input.
- The aggregate contained 12,670 ocean markers, 154 scopes, and zero rejected
  markers.
- It contained 297 fingerprinted batches across 10 distinct aggregate hashes,
  with zero table-overflow batches and zero within-scope mismatches.
- Prepared-tree, toolkit, and executable integrity remained unchanged.

This demonstrates marker ordering and fingerprint consistency in the
automated boot-to-gameplay path. It does not establish visual correctness
across calm/storm, day/night, shore, split-screen, reflection, wakes, or every
mission. Actual hash values and all private logs remain uncommitted.

After adding the optional realistic-water pipeline, a fresh private
Remastered run explicitly selected it through the bounded runner:

- Four sandboxed boots completed and three validated same-title relaunches
  were followed before the configured boot cap.
- The aggregate contained 40,790 ocean markers, 499 clean scopes, and zero
  rejected markers.
- It contained 602 fingerprinted batches across 16 aggregate hashes, with
  zero table-overflow batches and zero within-scope mismatches.
- The separate realistic fragment pipeline activated in the two boots that
  reached supported marked passes.
- No unsupported-marked-pass fallback, realistic-pipeline failure, or Metal
  runtime error was observed.
- Prepared-tree, toolkit, and executable integrity remained unchanged.

This proves that the optional pipeline is not a synthetic-only or silent
no-op in the exercised runtime flow. It does not judge whether the treatment
looks better, establish representative scene coverage, or measure GPU cost.
Private hashes, logs, saves, and retail data remain uncommitted.

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
