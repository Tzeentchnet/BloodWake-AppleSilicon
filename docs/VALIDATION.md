# Apple Silicon validation

Updated 2026-11-05. This document records measured behavior and regression
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

Runs made before 2026-11-05 used C `clock()` for the older `[GPU] ... fps`
line, so those values are not accepted as wall-clock evidence. The reporter
now uses `xbox_HostNowNs()`, and the window title labels its independently
wall-clocked completed-flip value as `Game FPS`.

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

- The Metal backend suite passes 337/337 tests, including texture-stage masks,
  combiner operations, cube reflection, mip selection, command-buffer and
  render-encoder batching, three-submission ownership, bounded multi-surface
  consolidation, private feedback with CPU-write fallback, exact quincunx
  reference sweeps, texture caching, upload-arena reuse, anisotropy
  selection/lifecycle, optional-water frame-boundary selection, exact default
  Retail output, marked-only enhancement, typed lit/reflection/foam pass and
  view propagation, persistent multi-draw and empty pass runs, legacy marker
  compatibility, malformed/mismatched event and run rejection, per-pass
  fallback, edition gating, invalid startup rejection, strict benchmark
  authorization, and the Retail/Realistic/Realistic/Retail benchmark state
  machine.
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
- The hook-portability suite passes 10/10 tests, including the validated lit,
  per-view, reflection-run, and foam-run boundaries plus Original fallback.
  It verifies event order, disabled and empty passes, invalid views, and guest
  state preservation. Metal backend coverage verifies marked/unmarked
  batches, 4,096-marker coalescing into one scope, persistent multi-draw and
  empty runs, conflicting/unknown rejection, no carry-over, and byte-identical
  Retail output.
- The MSAA evidence harness passes 9/9 Python tests covering 2x/4x/8x memory
  costs, strict schemas, non-finite and overflow rejection, missing evidence,
  and exact decision-threshold boundaries.
- The Metal MSAA capability target passes 2/2 tests. The recorded Apple M5
  reports 2x, 4x, and 8x support; the probe explicitly retains the unsupported
  project policy.
- The standalone Metal MSAA smoke target passes 2/2 tests. The recorded Apple
  M5 passes synthetic 2x/4x/8x color, depth/stencil, resolve, readback, and
  explicit error checks; this is not game-renderer or visual evidence.
- The focused macOS diagnostic suite passes 59/59 tests, including strict
  edition and built-in script selection, bounded marker/fingerprint summary
  parsing, typed relaunch outcomes, integrity-failure precedence, boot-cap
  reporting, explicit audio isolation, and strict headless-audit authorization
  with default audio preserved. It also covers strict water-preset propagation,
  inherited-value removal, edition gating, activation/fallback reporting,
  benchmark CLI authorization and propagation, clean-environment isolation,
  and malformed, duplicate, inconsistent, missing, or unexpected benchmark
  summary rejection.
- Nine LOD cases cover Original passthrough, defaults, disable/fix override,
  custom/clamped values, live changes, invalid rejection, and concurrent
  readers/writers.
- The standalone Metal smoke test passes, and the Release native `bloodwake`
  target builds successfully with the direct presenter and scaled-target path.
- Seven focused Metal ownership, feedback, failure, and target-switch cases
  pass with ASan/UBSan; focused cases have also passed Metal validation.
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
  Python suite now passes 117/117.
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

The next private four-boot Remastered run selected Retail initially and
authorized the diagnostic-only water benchmark. The backend waited for
supported marked-ocean rendering, then ran four same-scene blocks in
Retail/Realistic/Realistic/Retail order. Each block used 10 supported-water
warm-up frames and 50 measured frames. Two boots independently completed the
full 100-Retail/100-Realistic sample:

| Completed sample | Retail mean / median / p95 | Realistic mean / median / p95 | Mean / median / p95 delta |
| --- | --- | --- | --- |
| 1 | 51.126104 / 51.121375 / 54.506500 ms | 53.591635 / 52.075000 / 60.007250 ms | +4.822451% / +1.865413% / +10.091916% |
| 2 | 52.924863 / 53.103750 / 56.421125 ms | 55.331963 / 54.999625 / 60.814792 ms | +4.548148% / +3.570134% / +7.787273% |

The four-boot aggregate contained 38,330 markers, 467 clean scopes, and 604
fingerprinted batches across 17 aggregate hashes, with zero marker rejection,
fingerprint overflow, or within-scope mismatch. The shader activated in the
two benchmark boots, with no unsupported-pass fallback, malformed completed
summary, or Metal runtime error. Both completed summaries contained the exact
100/100 sample counts. The configured boot cap was reached while another
validated relaunch was requested, so the runner returned its documented
non-success cap status after preserving and reporting the two complete
samples.

These timings are completed Metal command-buffer durations for command buffers
that contained supported marked-ocean draws. They include other commands in
the same buffers and do not claim isolated shader cost, a performance
acceptance threshold, representative scene coverage, or improved visual
quality. Private logs, hashes, saves, and retail data remain uncommitted.

### Representative one-player water boundary

A fresh private guided run used physical keyboard input to enter one-player
Battle Mode, then remained in visible gameplay on open water for the bounded
capture. The bridge recorded physical Start, D-pad, A, and B transitions with
monotonic timing, and private framebuffer captures confirmed gameplay rather
than a menu or attract sequence.

- Two boots completed with one validated same-title relaunch.
- The aggregate contained 231,292 water markers and 431,057 marked draw
  scopes.
- Pass totals were 2,645 lit, 214,206 reflection, and 214,206 foam scopes.
- View 0 produced 3,253 begins and 3,253 ends; views 1-3 remained unused.
- The run fingerprinted 427,046 batches across 10 aggregate hashes, with zero
  table-overflow batches and zero within-scope mismatches.
- The first implementation reported 604 rejections. Every sampled diagnostic
  reason was a marked scope without an extended batch, originating from a
  persistent reflection or foam run rather than a malformed marker, unmatched
  view, or frame leak.

Persistent pass runs describe all title `BEGIN/END` scopes, including scopes
that submit no extended batch. The executor now records that provenance,
accepts those empty persistent-run scopes, and continues to reject an
unconsumed one-shot draw marker. A focused regression covers the distinction,
the carried patch applies cleanly, and the production runtime rebuilds.

A fresh post-fix one-player Battle run then completed two boots with one
validated relaunch and reached its configured time limit:

- 186,893 lit, 186,893 reflection, and 186,893 foam scopes;
- 2,915 view-0 begins and 2,915 view-0 ends;
- 560,679 marked batch scopes with zero rejection;
- 552,477 fingerprinted batches across 10 aggregate hashes, with zero table
  overflow and zero within-scope mismatch;
- Realistic water active with no unsupported-pass fallback; and
- Metal private reflection depth active with no logged fallback.

At this checkpoint, the then-current Metal suite passed completely, all eight
focused private-depth sanitizer cases passed, and the hook suite passed
10/10. This closed NW-007 and NW-100 for representative single-player
gameplay. Split-screen/view-1 evidence remains deferred. Private frames,
input timings, saves, logs, fingerprints, and retail data remain uncommitted.

### Guided 4x performance observation

The representative Battle captures are correctness diagnostics, not clean
performance benchmarks. They render internally at 2560x1920 while counting
every GPU method/draw and periodically synchronizing and writing framebuffer
BMPs. Runs before and after the Metal reflection-depth port both sustained
roughly 14-15 completed game flips per second in the exercised Battle scene;
the accepted post-fix run measured about 13-14 FPS in later windows. Stable
59.94 Hz vblank does not imply new game frames at that rate. These numbers
show that the displayed counter was measuring real flips, but they must not be
used as the normal-play baseline.

### Clean Battle performance baseline

A later one-player Battle matrix used the same validated private save and
same-title relaunch seed, but disabled GPU audit and BMP capture. Fixed render
scale, the lightweight Metal frame timer, and completed-command GPU timing
were the only performance diagnostics. The title still required the
experimental keyboard pad to remain connected; no synthetic menu input ran
during these samples.

| Fixed scale | Internal target | Completed-game FPS | GPU command median | CPU command-wait median | Wall-frame median |
| --- | --- | --- | --- | --- | --- |
| 1x | 640x480 | 30.53-31.89 | 17.16-17.23 ms | 17.77-17.90 ms | 30.48-30.72 ms |
| 2.25x | 1440x1080 | 28.37-29.60 | 20.13-20.35 ms | 20.76-20.95 ms | 33.38-33.65 ms |
| 4x | 2560x1920 | 15.76 | 48.25 ms | 48.84 ms | 62.03 ms |

A separate two-minute clean 4x run began near 14.8 FPS and later measured
about 12.7 FPS. That sustained decline is consistent with increasing load,
but scene progression was not held constant and no direct package-power or
thermal telemetry was collected, so it is not proof of thermal throttling.

Caller-attributed native sampling identifies the serialization boundary. The
guest thread spent 87.3% of its samples in the condition wait called by
`bw_flush_wait`; the NV2A/Metal worker spent 79.3% in the condition wait
called by `finish_command`. The latter commits the current command buffer and
calls `waitUntilCompleted` while synchronizing private render surfaces with
guest memory. The close GPU-duration and host-wait medians independently
confirm that this is real GPU completion time rather than a faulty FPS
counter.

Resolution is a major cost, but serialization also imposes a substantial
fixed floor. At 4x, the measured 48.25 ms median GPU command alone limits the
current workload to about 20.7 FPS before CPU work, so removing the wait
cannot make native 4x reach 60 FPS. At 2.25x the corresponding ideal ceiling
is about 49 FPS, while 1x is close to a 60 FPS GPU budget but reaches only
about 32 FPS because CPU and GPU work do not overlap. The next implementation
boundary is bounded asynchronous resource ownership, not blindly deleting
the wait: vertex-upload memory, readback resources, render-surface reuse,
CPU-written guest surfaces, snapshots, and shutdown all require explicit
completion rules. No default render scale or frame cap changed as a result of
this diagnostic matrix.

#### Synchronized private-surface reuse

The first base-renderer optimization removes redundant guest-to-private color
surface seeds without relaxing completion. Each cached Metal color target now
retains an exact guest-resolution synchronization snapshot. Reactivation
reuses the private texture only when the surface identity, format, dimensions,
pitch, and every active guest byte still match; a CPU or copy-engine change
forces the existing seed path. GPU writeback refreshes both guest memory and
the snapshot. This keeps target switches, framebuffer descriptors, the
CPU-write counterexample, and shutdown synchronous while avoiding an
unnecessary upload/scale operation.

The then-current Metal suite passed completely with a new ownership case
covering unchanged reactivation, CPU mutation, and subsequent reuse. Clean
seeded Battle runs reached the time limit with zero marker rejection or Metal
error. At each scale the first 300-frame report recorded 599 target reuses and
only one present-time guest reseed:

| Fixed scale | Earlier baseline median | First optimized median | Later optimized range |
| --- | --- | --- | --- |
| 1x | 17.16-17.23 ms GPU / 30.48-30.72 ms wall | 16.76 ms GPU / 30.02 ms wall | 16.77-17.40 ms GPU / 29.72-30.73 ms wall |
| 2.25x | 20.13-20.35 ms GPU / 33.38-33.65 ms wall | 19.75 ms GPU / 32.99 ms wall | 20.48-22.40 ms GPU / 33.76-36.77 ms wall |
| 4x | 48.25 ms GPU / 62.03 ms wall | 43.38 ms GPU / 56.75 ms wall | 44.56-50.27 ms GPU / 58.46-65.24 ms wall |

The early 4x window suggests a useful reduction in redundant scaled-surface
work, and 1x wall time improves modestly. The later 2.25x and 4x windows also
show that scene progression and sustained load can outweigh that gain. These
were repeat runs, not interleaved process-level A/B samples with controlled
power and temperature, so no precise percentage is claimed. Most importantly, GPU duration and host wait remained nearly identical:
exact cache reuse did not remove the per-frame `waitUntilCompleted`
serialization. This identified bounded asynchronous ownership as the next
major optimization; the completed follow-up is documented below.

#### Bounded ownership and function specialization

The 30 FPS closure separated direct display preparation from explicit guest
publication, bounded the queue at two submissions, and assigned
vertex, surface-upload, and surface-readback resources to their owning
submission. Ordinary target switches commit private generations
asynchronously. Snapshots, dumps, feedback, captures, scale transitions, and
other CPU-visible consumers still wait and publish in order. Detected guest
CPU changes are merged over the newest private generation before reseeding.

The first representative ownership-only 4x run remained GPU-bound at
16.50 FPS with a 59.42 ms wall median. Command-role timing attributed the
remaining cost across the selected AA target, an alternate target, and
presentation. Metal function constants now specialize immutable texture-stage
programs, register-combiner routing, final-combiner routing, and alpha-test
mode while preserving dynamic material constants and references.

Two independent fixed-4x Battle runs produced the following range across all
reported rolling 300-frame windows:

- 32.08-36.14 FPS average;
- 27.90-29.34 ms wall median and 33.26-35.63 ms wall p95;
- 15.66-20.75 ms combined render/present GPU median;
- 9.36-12.45 ms command-wait median; and
- 12.33-15.89 ms submission-latency p95.

Both runs retained the expected lit, reflection, and foam scopes with zero
marker rejection. The exact Native/auto, match-display run selected 4x
(2560x1920); every window averaged 31.89-34.65 FPS with a
28.75-29.92 ms wall median and 32.73-35.89 ms p95. This clears the 30 FPS and
33.3 ms median gate without a quality-setting change.

The final two-run matrix measured 38.50-45.30 FPS at 1x,
37.95-47.62 FPS at 2.25x, and 32.08-36.14 FPS at 4x. The complete Metal suite
passes 337/337, seven focused ownership/publication cases pass under
ASan/UBSan, and the macOS presenter suite passes 62/62. The measured
executable SHA-256 is
`fee23401f7c42a13744b5c1bd6521bbc22ea93cbffe8c17e091337f693496891`.
Post-matrix `pmset -g therm` reported no thermal or performance warning, but
direct package-power and temperature telemetry was not available, so no
energy-per-frame conclusion is drawn.

#### GPU-private feedback and sustained Native 4x/60

The 4x/60 continuation added exact command reasons and feedback CPU counters.
The original path performed one synchronous feedback readback per game frame:
about 3.8-4.5 ms on the GPU and 13.46-14.26 ms of blocked CPU time. The
retained exact-alias path resolves the newest private XRGB anti-aliased
generation on the GPU when guest bytes are unchanged. A detected CPU write
still waits, publishes, merges changed 32-bit pixels, and reseeds.
Steady-state evidence records zero publications and 0.09-0.10 ms per feedback
check.

The final path uses three bounded submission-owned resource sets and lets one
command own every color-surface generation touched in order. Safe target
switches no longer force a commit; eight switches trigger a bounded fallback.
The representative frame therefore produces one
`render+target-switch+feedback+present+depth-copy` command instead of one
presented and two offscreen commands.

Two independent exact Native/auto boots selected 4x at 2560x1920. Across
their consecutive post-warm-up 300-frame qualification windows:

| Metric | Range |
| --- | --- |
| Completed-game FPS | 59.98-60.01 FPS |
| Wall median / p95 | 16.66-16.68 / 17.06-17.46 ms |
| Combined GPU median / p95 | 14.18-14.84 / 14.87-15.26 ms |
| Submission-latency p95 | 15.23-15.50 ms |

Two fixed-4x, cap-120 boots measured 59.36-100.78 FPS and
60.33-100.10 FPS across all reported windows. Both included consecutive
post-warm-up windows above 60 FPS; the second exceeded 60 FPS in every
complete window. All four boots used Retail water and the accepted quincunx
approximation, retained 4x scale/filtering/LOD/draw coverage, reported zero
rejected ocean markers, and completed without a Metal command failure.

Cold exact-default windows are not hidden: they averaged as low as 53.22 FPS
and contained 296-330 ms shader/workload warm-up maxima. Qualification uses
consecutive rolling reports emitted at or after presented frame 900 and
before the guest relaunch transition. A 59.94 Hz cap cannot compensate for a
cold stall with faster frames, so cold pipeline warm-up remains separate
follow-up work.

The measured executable SHA-256 is
`3516f2e2906347880bcf20dd8f2966a0c32b46776e620923d694f097e4d37562`.
The final source passes 337/337 Metal backend tests, seven focused
ASan/UBSan ownership/feedback cases, and 62/62 macOS presenter tests.

### Water visual-review boundary

The user's hands-on assessment is that the current optional treatment looks
almost identical to Retail. Separate bounded Retail and Realistic runs each
completed four private captures at fixed supported-water offsets, but camera
and scene phase drift between processes prevents pixel alignment. Those images
are excluded as formal A/B evidence and remain private.

This near-Retail result is consistent with the implementation rather than
proof of full-scene equivalence: the optional shader changes only the
explicitly marked lit/base pass, with deliberately low strength, while the
later Retail cube-reflection and wake-foam passes remain unchanged. Future
visual evidence must use same-execution aligned pairs and isolate all three
passes. See [WATER_RENDERING.md](WATER_RENDERING.md).

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
