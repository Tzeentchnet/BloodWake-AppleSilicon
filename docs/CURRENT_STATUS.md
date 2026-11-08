# Current project status

Updated 2026-11-07. This is the canonical high-level status for the Apple
Silicon fork. Detailed implementation history, measurements, and task
dependencies remain in the linked records.

## Summary

The arm64 development build boots a user-provided North American retail copy
of Blood Wake and has been user-confirmed for menu navigation and interactive
play. The native path uses the project Metal renderer, direct
`CAMetalLayer` presentation, native audio integration, and the game-specific
macOS keyboard bridge.

The verified Native/auto configuration keeps the 640x480 guest framebuffer
contract while rendering into 2560x1920 private color and depth targets and
presenting at 2560x1920. It is a development build, not a complete
compatibility or quality release.

## Available features

- Direct Metal presentation with private output-scaled color and depth
  targets, GPU scaling, gamma, and bounded submission ownership.
- Native F1 settings for proven-live LOD, Metal filtering, optional
  realistic water, and mouse controls. Restart-required and unsupported
  settings are labeled explicitly.
- Optional gameplay mouse input with sensitivity, inversion, steering/aim
  selection, and hold/toggle relative capture.
- Native APU dispatch through SDL audio. A headless diagnostic sink can retain
  timing and signal aggregates without retaining samples.
- Deterministic diagnostic-only menu navigation and a private guided-pad
  calibration path. Normal physical input behavior remains unchanged.
- Versioned ocean markers and bounded diagnostics for water passes, Metal
  timing, presentation timing, pipeline artifacts, and indexed framebuffer
  capture.
- An experimental, default-off realistic-water pipeline for supported marked
  Remastered ocean batches. Retail remains the exact default and fallback.

The quincunx approximation, keyboard bridge, mouse input, realistic water,
and diagnostic paths remain explicit opt-ins where documented.

## Validation snapshot

Current regression coverage includes:

- 376/376 Metal backend tests;
- 62/62 macOS presenter CTest entries, including the real-window AppKit case;
- 35/35 APU MMIO host tests;
- 8/8 normal and ASan/UBSan macOS host tests;
- 11/11 input integration tests;
- 6/6 APU/audio tests;
- 13/13 support tests;
- 100/100 concurrent APU restore stress runs;
- 66/66 isolated diagnostic-runner tests; and
- 136/136 complete Python tests.

A bounded retail integration boot reached its configured limit with direct
Metal at 2560x1920, native audio/DSP activity, 1,490 audited flips, 4,503
draws, 1,252,771 vertices, unchanged prepared inputs and executable, and no
sticky Metal error. This is integration evidence, not a complete hands-on
quality or audible-output validation.

## Performance status

Representative sustained Native 4x/60 is qualified only for the measured
Apple M5 Battle workload. Two exact Native/auto, match-display boots produced
post-warm-up windows at 59.98-60.01 FPS, 16.66-16.68 ms wall medians,
14.18-14.84 ms GPU medians, and 14.87-15.26 ms GPU p95 without reducing
render scale, filtering, LOD, draw coverage, quincunx behavior, or Retail
water.

### Pipeline and cold-start result

The current controller-free Battle trace contains 25 canonical pipeline
states. Two clean exact-default boots preloaded all 25 with zero runtime cache
miss, asynchronous fallback, cache reset, or post-frame-zero specialization
or pipeline creation. Their cold maxima were 232.569-239.077 ms.

No retained current-path run reproduced the historical 296-330 ms band, so
that band is retired without retroactive attribution. A warm repeat recovered
to 59.95-60.03 FPS and 15.39-15.40 ms GPU p95, but wall p95 reached 17.07 ms
and submission p95 remained 15.60-15.62 ms. PERF-033 is therefore a bounded
no-go rather than a pass.

Static exact matches and complete Battle draw-fingerprint overlap support
rendering continuity. A bounded guest-flip-indexed diagnostic also showed that
identical-binary Battle repeats enter different camera, boat, and simulation
states. The exact dynamic output-hash gate is
`unavailable_nondeterministic_seed`, not a renderer mismatch or pass.

### Presentation and broader qualification

Presentation telemetry excluded drawable acquisition and the manual host hold
as the general 17 ms tail source. The explicit presenter exposes no display
target timestamp, and current evidence does not justify a
`CAMetalDisplayLink` implementation. PERF-035 and PERF-036 retain the
explicit presenter as measured no-go decisions.

The broader qualification contract defines six private workload slots and
cold, warm, 30-minute, and 60-minute run classes. Only the Battle control slot
is populated. PERF-038 remains blocked by the PERF-033/PERF-036 gates, five
missing workload seeds, unavailable physical input and additional device
classes, and privileged telemetry gaps. Conditional CPU or GPU optimization
must wait for a valid broader-matrix miss that identifies the limiter.

## Known limitations

- Formal visual comparison against trustworthy reference frames remains
  incomplete.
- The quincunx approximation has not been verified against NV2A hardware.
- Physical-controller lifecycle, physical input-to-present latency, guest USB,
  broad mission coverage, split-screen, saves, FMV, and other editions remain
  incomplete.
- Native point-and-click menu hitboxes are not implemented.
- Native audio integration is exercised, but audible correctness, sustained
  output quality, latency, and underrun behavior still need hands-on
  validation.
- The realistic-water path covers only supported marked base/lit batches.
  Reflection and foam remain Retail, and aligned visual evidence is missing.
- Metal MSAA remains unsupported. Current device capability and synthetic
  smoke evidence are not enough to satisfy its correctness, performance, and
  memory gates.
- Direct package-power and temperature sampling is
  `unavailable_privilege`; controlled ambient evidence is unavailable, so no
  energy-per-frame claim is made.
- Windows behavior after the Apple Silicon changes has not been revalidated.

## Current boundaries

1. Keep the explicit presenter and current quality defaults.
2. Do not activate conditional CPU vertex or GPU-pass work without a
   classified PERF-038 miss.
3. Populate and validate the missing private workload seeds only when the
   required controller, device, and telemetry resources are available.
4. Continue hands-on input, audio, visual, and broad product coverage without
   treating diagnostic substitutes as passes.
5. Keep retail-derived saves, traces, captures, hashes, archives, logs, and
   prepared game data private and uncommitted.

## Detailed records

- [Building and diagnostics](BUILDING.md)
- [Detailed Apple Silicon handoff](APPLE_SILICON_HANDOFF.md)
- [Completed implementation](WORK_COMPLETED.md)
- [Validation evidence](VALIDATION.md)
- [Performance tracker](performance/ROADMAP.md)
- [Performance measurements](performance/EVIDENCE.md)
- [Native feature roadmap](NATIVE_FEATURE_ROADMAP.md)
- [Metal quality and policy](METAL_QUALITY.md)
- [Water rendering](WATER_RENDERING.md)
- [Modern water architecture](water/MODERNIZATION.md)
- [Modern water tracker](water/ROADMAP.md)
