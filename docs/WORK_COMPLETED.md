# Apple Silicon work completed

Updated 2026-11-01. This document records implemented Apple Silicon work.
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
  sustained gameplay remain unconfirmed.
- Unsupported states continue to fail explicitly instead of being guessed or
  skipped.
