# Apple Silicon work completed

Updated 2026-10-31. This document records implemented Apple Silicon work.
Current status and remaining work are in
[APPLE_SILICON_HANDOFF.md](APPLE_SILICON_HANDOFF.md); measured results and
test coverage are in [VALIDATION.md](VALIDATION.md).

## Native runtime and presentation

- Integrated the native macOS host, main-thread SDL event handling, guest event
  bridge, Metal lifecycle, framebuffer descriptor handoff, gamma application,
  and a visible 640x480 presentation path.
- Added a bounded isolated diagnostic runner with private writable paths,
  read-only prepared inputs, network and process-spawn denial, output limits,
  deadlines, executable and source fingerprints, and validated same-title
  relaunch handling.
- Kept guest USB and APU MMIO disabled until safe dispatch, DMA ownership, and
  shutdown lifetime rules are implemented.

## Metal renderer

- Implemented colour and depth/stencil surfaces, clears, drawing, blending,
  colour masks, alpha testing, culling, polygon offset, gamma, presentation,
  and ordered surface writeback.
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
- Added a test-only same-thread ARM64 MMIO trampoline with conservative
  instruction-span validation. It is not connected to the game.
- Reproduced APU/OHCI IRQ and shutdown lifetime failures, including queued and
  executing DPCs outliving device shutdown, and added a synthetic retirement
  model for admission, closure, cancellation, continuations, and quiescence.

## Known implementation limits

- Exact NV2A quincunx weights, sample positions, edge behavior, and rounding
  remain unknown.
- CPU/GPU ownership is coordinated only at the renderer's explicit surface
  boundaries; general guest aliases and concurrent CPU access are not solved.
- Guest USB, physical gamepads, APU audio, FMV, saves, other editions, and
  broad mission coverage are not complete Apple Silicon features.
- Unsupported states continue to fail explicitly instead of being guessed or
  skipped.
