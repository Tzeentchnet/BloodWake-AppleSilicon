# Apple Silicon boot handoff

Status updated 2026-10-30 after a new request to continue. This is a
work-in-progress port, not a playable release.

## Verified status

- An experimental arm64 executable builds, opens a window and enters guest
  startup. A real main menu, nonblank game frame, fresh relaunch, and in-game
  keyboard or physical-controller navigation have **not** been verified.
- A freshly rebuilt executable reproduced both isolated boot boundaries:
  strict mode stopped at the unsupported quincunx filter; with the optional
  approximation it advanced and stopped because changing colour surfaces
  within a frame is unsupported. Neither established a nonblank or correct
  frame. An earlier diagnostic observed an SDL
  upload/copy/present, but asynchronous execution does not establish its timing
  relative to the Metal error or the contents of the submitted framebuffer.
- Exact NV2A quincunx filter weights were not established by the available
  references. The current source contains a legacy-reference approximation
  opt-in, `BW_METAL_QUINCUNX_APPROX=1`; other values retain strict rejection.
  It emits startup and first-use warnings and is **not** hardware-verified.
  The opt-in was exercised during the isolated boot; visual correctness
  remains unverified. A separate isolated boot with the option unset stopped
  at the explicit quincunx-filter rejection, as intended.
- Guest USB and APU MMIO remain disabled. The host rejects the presence of
  `RECOMP_USB` or `RECOMP_AC97_READY`, including empty values and `0`.
  Synthetic input tests do not establish working guest input or game audio.

## Work completed

- Integrated the native host, main-thread SDL handling, guest event bridge,
  Metal lifecycle and framebuffer descriptor handoff. The renderer supports
  selected clear, surface, draw, one-stage combiner and BC1 texture cases;
  unsupported states remain explicit errors. The basic texture path now
  rejects feedback from an overlapping active colour surface even before its
  first GPU write; previously it accepted that unproven case. This does not
  resolve physical aliases, CPU/GPU ownership or render-to-texture. A
  separately labeled synthetic counterexample shows an unsynchronized CPU
  write after a completed GPU clear being overwritten on readback; passing it
  reproduces that limitation and is not a successful ownership test. Replace
  the counterexample with a positive handoff regression when a coordinated
  ownership protocol is implemented.
- Added a reusable single-boot [isolated diagnostic runner](BUILDING.md#isolated-native-diagnostic)
  with verified sandbox preflight, private writable paths, bounded output and
  deadlines, and before/after preparation, toolkit and executable checks.
  It does not execute relaunches, provide a replay capture, impose a total
  disk quota or establish visual correctness.
- Added a bounded, private [synthetic GPU capture/replay fixture](GPU_CAPTURE.md)
  for ordered methods, explicitly supplied memory changes, barriers and
  comparisons. It rejects unsupported or incomplete fixture streams. The
  observer is compiled only for the fixture; it is not live guest capture or
  Metal replay, and does not provide missing ownership or synchronization.
- Fixed POSIX OHCI invalid-DMA success reporting and reproduced and fixed
  physical-page-map and heap-reclaim initialization races. Atomic page-map
  accesses do **not** pin the mapping across a DMA transfer. Separate
  [synthetic DMA tests](../tests/dma_ownership/README.md) reproduce heap and
  contiguous reuse and ambiguous aliases. A new test-only physical-authority
  model exercises generation-qualified leases, shared aliases, sub-page
  ownership and synchronized handoff; neither reference model is connected to
  the runtime.
- Added a version-checked [guest-memory candidate inventory](GUEST_MEMORY_INVENTORY.md)
  covering the verified generated source, project and pinned toolkit, with an
  optional separately hashed derived include. It is lexical and non-exhaustive:
  the private report identifies candidates, not proven guest accesses or
  complete MMIO-dispatch coverage.
- Built a **test-only** same-thread MMIO trampoline with conservative ARM64
  instruction-span validation. It is not connected to the game; tests cannot
  rule out hardware partial writes, and thread registration, signal masks,
  lock order, cancellation and safe retirement remain unresolved.
- Reproduced APU/OHCI IRQ and shutdown lifetime failures using synthetic
  probes. Producer-thread joins do not drain queued, coalesced or executing
  callbacks or guest DPCs; stopping or clearing pending work is not a safe
  substitute. An additional real-bridge DPC counterexample reproduced queued
  and executing DPCs outliving APU shutdown. Three bounded CTest cases now
  exercise those counterexamples and their oracle. A separate
  [synthetic retirement model](../tests/mmio_host/RETIREMENT_MODEL.md)
  exercises admission, closure, queued and executing work, continuations and
  quiescence; it is not wired into the runtime. The previous TSan hang was
  traced to a missing SDL3 loader path and modal alert before `main`; the test
  build now supplies the discoverable SDL3 rpath. See
  [UPSTREAM_ISSUES.md](UPSTREAM_ISSUES.md#posix-device-irq-shutdown).

## Validation and integrity

- The diagnostic runner revision passed 45 Python tests, including 15 focused
  runner tests. Fresh host (6), presenter (58), virtual-input (7) and audio/DSP
  (4) tests passed both normally and under ASan/UBSan; presenter and input also
  passed under TSan. The synthetic Metal smoke test passed. These checks do
  not establish playable guest input, audio or rendering.
- The nested-fault fixture passes normally and under ASan/UBSan. Under TSan
  its callback was observed with both SIGBUS and SIGSEGV blocked despite
  `SA_NODEFER`; experimentally unblocking them before the nested fault allowed
  it to finish. The fixture now detects this incompatible mask and reports a
  separate explicit TSan skip instead of hanging. Six other host tests pass
  under TSan; the skip is not nested-fault coverage or a production fix.
  The upstream integer-MMX test uses x86-only
  compiler flags: an x86_64 cross-build succeeded but could not run because
  x86 execution was unavailable. The native MMX rounding test passed.
- The MMIO host suite passed 30/30 after the test-only span work, including
  386 synthetic span cases. Eight focused hardware and rejection tests passed
  in optimized, ASan/UBSan, UBSan and TSan builds. The AC'97 fault test
  previously passed 200 consecutive repetitions. Focused page-map and
  heap-reclaim tests passed under ThreadSanitizer and ASan/UBSan. The updated
  MMIO suite passed 33/33; all three DPC counterexample tests passed in debug,
  release, TSan and ASan/UBSan builds. Passing means unsafe behavior was
  reproduced, not that retirement is safe. With the synthetic retirement
  model registered, the full MMIO suite passed 34/34. The model and those
  three counterexamples also passed together under TSan and ASan/UBSan; the
  model does not prove runtime quiescence.
- The expanded DMA ownership suite passed 13 tests normally, under TSan and
  under ASan/UBSan, including ten TSan repetitions of five concurrency tests.
  These model checks do not prove runtime DMA safety.
- The guest-memory inventory passed 21 synthetic tests; the full Python suite
  passed 66/66. Two identical scans
  produced identical private reports across 264 file entries and 278,836
  lexical candidates; the derived build include was hashed but unverified.
  The prepared tree and pinned toolkit remained unchanged.
- The synthetic GPU capture suite passed 11 tests normally and under
  ASan/UBSan; four focused cases passed under TSan. The fixture reproduced its
  own CPU-rendered outputs and audit report, not a real guest frame or a
  hardware reference. Capture-disabled Metal/audit compatibility passed five
  focused tests.
- The latest Metal approximation revision passed 237 tests normally and
  under ASan/UBSan, 75 focused TSan tests and 40 Metal-validation tests.
  SDK 15.4 compatibility, including a forced legacy fallback, passed 11
  tests; a pre-15 SDK/OS was unavailable. The synthetic DMA investigation
  passed four tests normally and under TSan and ASan/UBSan; passing the
  counterexamples means the unsafe behavior was reproduced.
  Earlier focused runs also passed 57 presenter tests normally and under
  ASan/UBSan, seven virtual-input tests, four audio/DSP tests, and 30 Python
  tests. These do not prove in-game behavior.
- The active-feedback guard passed four new exact, partial-overlap and
  adjacent-span cases; the full Metal suite passed 242/242 normally and under
  ASan/UBSan. Eight focused neighboring cases passed under TSan and Metal
  validation. The suite includes the CPU-write counterexample, which reproduced
  its overwrite under normal, ASan/UBSan, TSan and Metal-validation runs.
- The new preflight verified all 37 generated files against the normalized
  published fingerprint and all 450 prepared disc files against the retail
  fingerprint. All 538 preparation records, including the root, and the
  pinned toolkit were unchanged across a separate arm64 build and both new
  diagnostics. A later fresh build additionally fingerprinted 131 selected
  project source entries before and after the build and saved a private record
  linking that snapshot to the executable hash. The worktree was uncommitted;
  external tools and libraries were not hashed, transient changes are not
  excluded, and hashes alone cannot reconstruct the source. Recheck integrity
  before and after future builds and diagnostics. No Windows build was verified.
- Keep disc images, unpacked assets, generated code, binaries, diagnostic
  logs and captures private. Never replace the prepared tree just to rebuild;
  use the separate build workflow in [BUILDING.md](BUILDING.md#macos-on-apple-silicon-experimental).
  The working SDK on this machine was
  `/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.

## Next steps

The next milestone is a **playable Original edition on native Metal**: boot,
navigate the menu, complete one representative mission, save and reload after
a fresh launch, with working keyboard, physical controller and game audio.
Aim for stable 30 FPS on this Mac. Remastered parity, every mission and
general Xbox compatibility are not prerequisites for this milestone.

The following is a coordinated implementation roadmap, not a claim that the
work is complete. Develop the rendering and device foundations as whole
systems, then integrate them for the milestone; do not drive the port solely
by fixing the first error from each new run.

### 1. Capture the workload and establish a reproducible baseline

- Confirm source and executable provenance, prepared-tree fingerprints and
  pinned-toolkit integrity for each new build and diagnostic. Build separately
  without replacing the prepared tree or modifying the toolkit checkout.
- Extend the synthetic capture foundation to a coherent, bounded private
  capture/replay workflow for ordered GPU methods, state, referenced memory
  and synchronization. Use representative boot, menu and mission workloads
  and synthetic replay fixtures to identify complete state families and their
  interactions. The existing audit is a usage summary, not a replay; the
  synthetic fixture must not be enabled for a live guest.
- Compare against an available, validated reference path and the existing CPU
  evaluator where applicable, while documenting their gaps. The current Metal
  backend stops at unsupported states; if no reference can reach later scenes,
  record that coverage boundary rather than silently skipping draws or
  assuming a capture-only backend preserves guest behavior.
- Run any real guest diagnostics with bounded duration, isolated home,
  temporary, configuration and save paths, verified outbound network denial,
  and a read-only prepared tree. Check integrity before and after. Keep
  captures and logs private. Until deliberately changed and validated, use the
  quincunx approximation only with its explicit opt-in.

### 2. Implement a coherent Metal renderer

- Use the extended executor interfaces, CPU vertex/combiner evaluators and
  Windows D3D11 renderer as prior art, not as proof of hardware fidelity.
  Implement the interacting state families required by the captured
  milestone: vertex transformation, multi-stage combiners and texture
  programs, formats and swizzling, sampling and render-to-texture,
  depth/stencil, blending, clipping, anti-aliasing, gamma and presentation.
- Define surface and texture ownership, aliases, GPU/CPU synchronization,
  readback, cache invalidation and error propagation across the executor and
  presenter. Do not rely on an unproven per-draw CPU fallback.
- Test state families and their combinations with synthetic pixel tests and
  differential replay; compare private game frames with a trustworthy
  reference when available. Document and test any visually acceptable
  approximation instead of claiming hardware verification. Unsupported
  states must continue to fail explicitly.

### 3. Make guest memory and devices safe together

- Prefer explicit guest-memory and MMIO dispatch outside signal context,
  with a fast RAM path and a checked device path. Inventory all generated and
  handwritten accesses, including direct pointers, bulk/string operations,
  vector and floating-point access, atomics, read-modify-write, and crossing
  spans. Use the lexical candidate inventory as a starting point, not proof
  of exhaustive coverage. The generated `MEM*` macros are writable lvalues,
  so changing one macro cannot cover every access. Adapt the lifter/runtime
  using project-owned, version-checked integration or separate generated
  build artifacts, preserving Windows behavior and the prepared tree.
- Establish authoritative physical backing and alias identity and a complete
  DMA ownership protocol. Prefer unique physical ownership with shared aliases
  if feasible; any alternative must carry valid identity from submission,
  not infer it later from a physical address. Coordinate allocation, claim,
  decommit, protection, reuse and teardown. Validate and pin or copy complete
  DMA spans, including descriptors and cached pointers, for their actual
  lifetimes. Define CPU/device data handoff as well as lifetime; atomic page
  maps or memory fences alone do not eliminate data races.
- Define admission, lock ordering, cancellation and nested-fault behavior for
guest threads, devices, IRQs and queued or executing DPCs. Use the registered
DPC counterexamples when designing the fix. Prove real callback and lease
quiescence before any in-process retirement; joining
  producers or clearing pending bits is insufficient. Process exit may remain
  the lifetime boundary when safe, but it does not make live DMA reuse safe.
  Keep USB and APU MMIO disabled until the entire relevant contract is tested;
  do not promote the test-only signal trampoline without proven coverage.

### 4. Integrate and validate the playable path

- Once the device contracts are validated, enable OHCI and APU and exercise
  real guest input and audio, including initialization, controller connection
  changes and sustained output. Verify saves in isolated writable paths
  across a fresh process. Synthetic SDL tests alone are not game validation.
- Determine whether boot or the representative mission needs FMV. If so,
  identify the actual container and codec and implement an appropriate macOS
  decode/presentation path, or report the blocker; do not assume macOS
  provides the required decoder.
- Verify visibly correct, nonblank menu and mission frames, not merely a
  submitted present; complete a mission without fatal or silently unsupported
  GPU/device states. Independently test keyboard and physical controller,
  recognizable menu and gameplay audio, save/reload and clean exit/relaunch.
  Ask the user for GUI, controller or audio assistance where needed.
- Measure a representative 60-second active-gameplay segment after loading.
  Report average FPS, median and 95th-percentile frame times, long stalls,
  resolution and settings; do not hide loading or shader-compilation stalls
  in the overall observations. Report a missed 30 FPS target separately from
  functional progress.
- Run focused Metal, presenter, host, MMIO, DMA, input, audio and Python
  regressions, plus applicable sanitizer, Metal-validation and broader build
  checks. Validate Windows if available, otherwise report it unverified.
  Update build and status documentation only to match observed results.

Test commands and native build prerequisites are in the
[README](../README.md#native-foundation-checks) and
[BUILDING.md](BUILDING.md#macos-on-apple-silicon-experimental). The latest
executable may not include source edits made after its last coordinated build.
