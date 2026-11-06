# Apple Silicon work completed

Updated 2026-11-03. This document records implemented Apple Silicon work.
Current status and remaining work are in
[APPLE_SILICON_HANDOFF.md](APPLE_SILICON_HANDOFF.md); measured results and
test coverage are in [VALIDATION.md](VALIDATION.md).

## Native runtime and presentation

- Integrated the native macOS host, main-thread SDL event handling, guest event
  bridge, Metal lifecycle, framebuffer descriptor handoff, gamma application,
  and a visible 640x480 presentation path.
- Added direct `CAMetalLayer` presentation with main-thread attachment and
  resize propagation. Letterboxing, guest-AA resolve, scaling, and gamma now
  run on the GPU; `BW_METAL_DIRECT_PRESENT=0` retains the CPU/SDL fallback.
- Added a bounded isolated diagnostic runner with private writable paths,
  read-only prepared inputs, network and process-spawn denial, output limits,
  deadlines, executable and source fingerprints, and validated same-title
  relaunch handling.
- Added default-on native APU audio with strict `BW_MACOS_AUDIO=0|1` control.
  Direct `RECOMP_AC97_READY` remains rejected because it is presence-based;
  guest USB remains disabled.
- Added guarded same-thread deferred APU MMIO dispatch. The signal handler
  validates the complete ARM64 access span, redirects the original guest
  thread to ordinary context, and uses a second kernel signal return to
  restore integer, SIMD, flags, mask, and interrupted control state.
- Bound each guarded deferred stack to its APU-touching thread for the
  installed lifetime and separated ownership from active-dispatch state. This
  prevents a concurrent thread from reusing a stack while Darwin `_sigtramp`
  is still consuming its signal frame.
- Enabled dispatch only after successful APU construction and disabled it
  before startup-failure shutdown or guest-memory teardown. Normal guest
  return still uses process lifetime because all guest/device workers cannot
  yet be retired in-process.
- Rebuilt the native Release target and completed a 20-second bounded retail
  boot with default-on CoreAudio, title-driven APU/DSP activity, and APU
  interrupts at 4x direct-Metal scale. The run ended at its intentional limit;
  one SDL underrun was observed and audible correctness remains unconfirmed.
- Added a diagnostic-only headless audio sink authorized by the bounded native
  runner. It keeps 48 kHz stereo APU/DSP timing active without CoreAudio,
  retains no samples, and reports synchronized aggregate signal, silence,
  clipping, queue, cadence, jitter, and latency metrics. Default CoreAudio
  startup and failure behavior remain unchanged.

## Metal renderer

- Implemented colour and depth/stencil surfaces, clears, drawing, blending,
  colour masks, alpha testing, culling, polygon offset, gamma, presentation,
  and ordered surface writeback.
- Added independently scaled private colour and depth/stencil targets. Guest
  surfaces seed through shared staging, guest-sized readback uses a weighted
  box filter, guest AA collapses at native scale unless explicitly retained,
  and partial-clear rectangles map conservatively into physical pixels.
- Added automatic drawable-based scaling with post-frame cache epochs.
  Depth/stencil contents are preserved through live scale changes before
  targets are recreated.
- Ported the DirectX register-combiner model for one to eight stages plus the
  final combiner, including constants, input/output mappings, dot products,
  mux, blue-to-alpha, scratch destinations, fog, and inactive stages.
- Added up to four independent projective texture stages with wrap, mirror,
  and clamp addressing; point, linear, nearest-mip, and trilinear filtering;
  and restricted mip chains up to 12 levels.
- Added BC1, BC2, BC3, swizzled R5G6B5, A8R8G8B8, X8R8G8B8, and supported
  pitch-linear texture uploads with explicit bounds and active-surface alias
  checks.
- Ported the retail ocean reflection path, including diffuse and specular
  reflection texture programs, dot-product mappings, Direct3D-compatible cube
  face selection, six-slice cube uploads, nearest-mip selection, and polygon
  offset.
- Added a narrowly scoped, explicit `BW_METAL_QUINCUNX_APPROX=1` fallback for
  the title's quincunx resolve. It uses the legacy Gaussian approximation and
  is not verified NV2A filtering.
- Added immutable Metal anisotropic sampler variants from 1x through 16x.
  Only linear, mipmapped stages select anisotropy; point and unmipped stages
  stay 1x, and live changes take effect at a flip boundary.
- Converted Remastered LOD from a boot-only cache to validated atomic 1x-8x
  runtime state. Original remains fixed at 1x.
- Added a versioned in-stream ocean draw marker at the validated native ocean
  boundary. The executor scopes it to one BEGIN/END draw, copies it into
  extended batches, coalesces repeated valid preparation markers, rejects
  invalid/carry-over state, and preserves the exact retail Metal path for both
  marked and unmarked draws. A bounded four-boot Remastered run recorded
  41,348 markers, 512 scopes, and zero rejections.
- Added a standalone, fail-closed Metal MSAA evidence tool for checked
  attachment/resolve memory costs and controlled timing, correctness, and
  visual-review records. It does not enable Metal MSAA.
- Added a read-only Metal sample-count probe with fixed-shape JSON and explicit
  error reporting. The recorded Apple M5 supports 2x, 4x, and 8x; project
  policy remains unsupported.
- Added a standalone real-Metal MSAA smoke path. The Apple M5 passes synthetic
  2x/4x/8x color, depth/stencil, resolve, readback, error, memory-lower-bound,
  and timing checks without changing the game renderer or support policy.
- Added bounded marker-only ocean fingerprints over stable copied metadata.
  Private GPU audits report at most 32 aggregate hashes plus overflow and
  within-scope mismatch counts; retail data and rendering are unchanged.
- Combined deterministic menu navigation with the headless audio sink in a
  private three-boot/two-relaunch Remastered flow. It produced 12,670 markers,
  154 clean scopes, and 297 fingerprinted batches across 10 aggregate hashes,
  with zero marker rejection, table overflow, or within-scope mismatch.
- Added a default-off Remastered `Retail`/`Realistic` water setting for macOS
  Metal. It owns a separate fragment pipeline, applies only at a flip boundary
  to explicitly marked batches with the validated water stage shape, preserves
  retail alpha, and falls back per pass. Unmarked draws and Retail continue to
  use the existing fragment entry.
- Added restrained Fresnel shaping from the game's existing interpolated water
  normal/view basis, stage-0 water sample, fog atmosphere, and visibility.
  The later Retail cube-reflection and wake-foam passes remain unchanged. It
  adds no replacement textures, refraction, absorption, foam, shoreline
  logic, or depth dependency.
- Added synthetic coverage for default Retail equivalence, marked-only
  enhancement, unmarked recovery, unsupported-state fallback, frame-boundary
  activation, strict environment parsing, and Original rejection. A private
  four-boot run activated the shader on real marked ocean batches with no
  observed fallback or Metal error.
- Added the source-grounded
  [water-rendering dossier](WATER_RENDERING.md), separating simulation,
  render geometry, the three Retail ocean passes, the lit-pass marker, Metal
  shader inputs, optional-preset limits, mechanics-safe opportunities, and
  open evidence questions. It records that the current treatment is
  intentionally subtle and that the distant-water-line fix remains DX11-only.
- Added the source-grounded
  [lighting/rendering dossier](LIGHTING_RENDERING.md), tracing title scene
  ownership through CPU fixed-function/vertex-program evaluation and Metal
  combiner rendering, inventorying missing PBR semantics, and defining a
  staged semantic-hybrid path from Retail fidelity through per-pixel lighting
  and selective PBR before any full renderer replacement.

## Renderer performance

- Replaced per-draw command-buffer submission and waits with an ordered pending
  command buffer submitted at presentation, surface ownership, readback, and
  shutdown boundaries.
- Reused one Metal render encoder across draws with matching colour and depth
  attachments.
- Replaced per-draw Metal vertex-buffer allocation with a shared upload arena
  reused only after command-buffer completion.
- Added a 1024-entry decoded-texture cache keyed by guest span and layout.
  Entries are checked at most once per frame; stable entries use distributed
  samples and staggered full verification. LRU eviction is bounded, and
  render-target writeback invalidates overlaps.
- Added monotonic rolling frame-time reporting under `BW_GPU_AUDIT=1`,
  including average FPS, median, p95, and maximum frame interval.
- Removed the direct-present path's CPU gamma pass, second guest-memory read,
  and SDL streaming-texture upload. The executor's synchronous guest-sized
  writeback remains for snapshots and framebuffer dumps.

## Input

- Added a default-off, game-specific macOS keyboard bridge with
  fingerprint-checked wrappers, synchronized polling, handle validation, and
  a single controller on port 0.
- Added presenter coverage for key presses, releases, focus loss, and virtual
  controller connection changes.
- Added a separately compiled synthetic-input diagnostic that verifies a guest
  state query can observe Start, Down, and A.
- Added a strict built-in `remastered-boot-menu-to-gameplay-v1` script. It
  generates direct Xbox-equivalent Start, D-pad Down, and A press/release
  packets on guest polling epochs, requires the diagnostic synthetic-input
  build and Remastered selection, and releases state on completion, focus
  loss, and shutdown.
- Added a floating AppKit F1 panel built from the shared settings schema.
  It consumes F1 before guest input, releases held input, reports
  live/frame/restart/unsupported state, applies subsystem-owned live values,
  and saves through the strict atomic INI writer. It does not synthesize an
  unsafe pause.
- Added a real AppKit/presenter integration target covering F1 down/repeat/up,
  main-thread panel/controller lifecycle, input and mouse release, capability
  reasons, live LOD, frame-boundary filtering, restart-pending state, isolated
  atomic save, and visible error sheets. Its real-window case skips explicitly
  when WindowServer is unavailable.
- Added configurable gameplay mouse input through the native controller
  bridge, including synchronized relative deltas, focus/capture release,
  sensitivity, inversion, response curve, left/right stick selection, and
  larger-magnitude merging with keyboard/controller axes.
- The current input-enabled build is user-confirmed for menu navigation and
  in-game play. Feedback, reconnect, focus-loss, and complete device-lifecycle
  behavior remain experimental.

## Diagnostics and reference tooling

- Added a bounded synthetic GPU capture/replay fixture for ordered methods,
  supplied memory changes, barriers, and comparisons. It is not enabled for a
  live guest and does not replace a hardware reference.
- Added a version-checked lexical guest-memory candidate inventory covering
  verified generated source, project source, and the pinned toolkit.
- Added explicit sticky errors for unsupported renderer states, invalid spans,
  allocation failures, and Metal command failures rather than silently
  dropping work.
- Added strict diagnostic `--water-preset retail|realistic` selection,
  Remastered gating, inherited-setting removal, and fail-closed reporting when
  a requested realistic run never activates the shader.
- Added a diagnostic-only same-scene water benchmark with strict private-runner
  authorization, Retail/Realistic/Realistic/Retail ordering, supported-frame
  warm-up and sampling, completed-command-buffer GPU timing, Retail restore,
  and fail-closed aggregate parsing. Two private boots completed independent
  100-Retail/100-Realistic samples; Realistic command-buffer deltas were
  +4.548%-+4.822% mean, +1.865%-+3.570% median, and +7.787%-+10.092% p95.
- Added an offline Blood Wake upstream parity manifest/checker covering the
  42-entry fix registry, settings fix references, native source wiring, branch
  divergence, and the two classified DX11 warm-up commits.

## Memory and device research

- Reproduced unsafe shared-backing reuse, unsynchronized CPU/GPU ownership,
  physical-page-map publication, and heap-reclaim cases in isolated fixtures.
- Fixed POSIX OHCI invalid-DMA success reporting and reproduced initialization
  races in physical-page mapping and heap reclaim.
- Added synthetic DMA ownership models for generation-qualified leases, shared
  aliases, sub-page ownership, and synchronized handoff. These models are not
  connected to the runtime.
- Promoted the bounded same-thread ARM64 trampoline into the native APU fault
  bridge with conservative full-span instruction validation, 32 preallocated
  slots, guarded stacks, a protected restore page, cancellation suppression,
  and fail-closed nested/unrelated-fault behavior. OHCI remains excluded.
- Added production-bridge regressions for a real protected-page load/store,
  recursive model locking on the original guest thread, state preservation,
  disabled forwarding, concurrent dispatch, startup controls, initialization
  failure, and dispatch-before-shutdown ordering.
- Repeated the concurrent production bridge in 100 separate stress processes
  after fixing deferred-stack lifetime; all 100 completed successfully.
- Reproduced APU/OHCI IRQ and shutdown lifetime failures, including queued and
  executing DPCs outliving device shutdown, and added a synthetic retirement
  model for admission, closure, cancellation, continuations, and quiescence.

## Known implementation limits

- Exact NV2A quincunx weights, sample positions, edge behavior, and rounding
  remain unknown.
- Direct presentation and 4x internal scaling have synthetic coverage and
  hands-on retail visual confirmation. Formal reference comparison, frame
  time, and memory measurements remain outstanding.
- CPU/GPU ownership is coordinated only at the renderer's explicit surface
  boundaries; general guest aliases and concurrent CPU access are not solved.
- Guest USB, physical gamepads, FMV, saves, other editions, and broad mission
  coverage are not complete Apple Silicon features. Native APU audio is
  implemented and regression-tested, but audible retail output, quality, and
  sustained gameplay remain unconfirmed. The headless audit validates
  aggregate signal/timing only and is not an audible-output fallback.
- Gameplay mouse input is implemented, but point-and-click menu hitboxes need
  validated title UI state and geometry. Ship/weapon profiles need proven
  definition fields/ownership. The optional realistic-water shader is
  implemented and runtime-activated, but remains experimental and default-off
  until representative pass coverage, hands-on Retail/Realistic visual
  comparisons, and a project acceptance decision are complete. Same-scene GPU
  timing is recorded for the current automated scene; it is whole
  water-containing command-buffer cost, not isolated shader timing.
- Metal MSAA remains deliberately unsupported until the controlled
  visual/GPU-time/memory gate in `METAL_QUALITY.md` justifies it.
- Unsupported states continue to fail explicitly instead of being guessed or
  skipped.
