# Apple Silicon performance work tracker

Status: tracked performance tracker
Last updated: 2026-11-06

This tracker covers base-game frame rate, efficiency, and thermal work. The
bounded ownership and 30 FPS default-quality target are complete; direct
power/temperature telemetry remains open.

## Status legend

- `[x]` complete and validated
- `[~]` active
- `[ ]` pending
- `[!]` blocked or requires a product decision

## Current objective

Retain Native/auto 4x quality while keeping every representative 300-frame
Battle window at or above 30 FPS, below a 33.3 ms wall median, and free of
material p95 or submission-latency regression. Continue collecting direct
power and temperature evidence when privileged telemetry is available.

## Dependency map

| ID | Task | Status | Depends on |
| --- | --- | --- | --- |
| PERF-001 | Verify completed-game FPS counter | `[x]` | None |
| PERF-002 | Build clean seeded Battle harness | `[x]` | PERF-001 |
| PERF-003 | Measure 1x/2.25x/4x baseline | `[x]` | PERF-002 |
| PERF-004 | Attribute CPU and Metal worker waits | `[x]` | PERF-002 |
| PERF-005 | Measure GPU command and host wait duration | `[x]` | PERF-003 |
| PERF-006 | Reuse byte-identical cached private surfaces | `[x]` | PERF-005 |
| PERF-007 | Validate cache reuse and CPU-write fallback | `[x]` | PERF-006 |
| PERF-008 | Specify bounded asynchronous ownership | `[x]` | PERF-004, PERF-005, PERF-007 |
| PERF-009 | Add frame-owned upload resources | `[x]` | PERF-008 |
| PERF-010 | Add frame-owned readback/publication state | `[x]` | PERF-008 |
| PERF-011 | Bound commands/frames in flight | `[x]` | PERF-009, PERF-010 |
| PERF-012 | Preserve explicit synchronization boundaries | `[x]` | PERF-010, PERF-011 |
| PERF-013 | Add asynchronous ownership tests | `[x]` | PERF-009, PERF-010, PERF-012 |
| PERF-014 | Repeat clean scale matrix | `[x]` | PERF-011, PERF-013 |
| PERF-015 | Collect controlled power and thermal evidence | `[~]` | PERF-014 |
| PERF-016 | Choose product defaults | `[x]` | PERF-014, PERF-015 |
| PERF-017 | Resume modern-water visual phases | `[ ]` | PERF-014 |

## Completed investigation

### PERF-001: Verify the frame-rate counter

- [x] Confirm the title bar uses completed guest flips and host wall time.
- [x] Compare it with independent Metal completed-flip intervals.
- [x] Rename the metric to `Game FPS`.
- [x] Correct the unrelated executor reporter that used process CPU time.
- [x] Document that vblank frequency is not completed-game FPS.

### PERF-002: Build a repeatable clean workload

- [x] Use the validated one-player Battle save and same-title relaunch record.
- [x] Add bounded save and launch seeding to the diagnostic runner.
- [x] Reject symlinks and enforce file-count and byte limits.
- [x] Keep the experimental keyboard pad connected.
- [x] Disable audit, capture, and menu automation for timing runs.
- [x] Fix run-limit diagnostic/profile dumping before status 124.

### PERF-003 through PERF-005: Establish the baseline

- [x] Measure fixed 1x, 2.25x, and 4x internal scales.
- [x] Measure completed-game FPS and wall-frame median/p95.
- [x] Measure completed Metal command-buffer duration.
- [x] Measure host command-completion wait duration.
- [x] Profile with caller attribution.
- [x] Prove that the guest waits in `bw_flush_wait`.
- [x] Prove that the Metal worker waits in `finish_command`.
- [x] Establish that 4x GPU work alone cannot meet 60 FPS.
- [x] Keep thermal-throttling claims open pending direct telemetry.

### PERF-006 and PERF-007: Exact private-surface reuse

- [x] Add per-surface guest-resolution synchronization snapshots.
- [x] Compare all active bytes before reusing a private texture.
- [x] Include format, dimensions, pitch, and AA layout in the decision.
- [x] Force reseed after CPU or copy-engine mutation.
- [x] Upload from the stored snapshot to avoid comparison/upload races.
- [x] Refresh the snapshot after GPU writeback.
- [x] Count target reuses and present-time guest reseeds.
- [x] Add normal and sanitized ownership tests.
- [x] Run the complete 333-case Metal suite.
- [x] Repeat seeded Battle runs at 1x, 2.25x, and 4x.

## Completed design: PERF-008 through PERF-012

### Command lifetime

- [x] Define a submission record containing the command buffer and every
  resource whose contents may still be read or written by that command.
- [x] Separate "encoded", "committed", "completed", and "guest-published"
  states.
- [x] Choose a two-submission bound for command groups in
  flight.
- [x] Make queue saturation wait for the oldest submission only.
- [x] Propagate command failure before reporting success for dependent work.

### Vertex uploads

- [x] Replace the one reset-after-wait vertex arena with a small ring.
- [x] Prevent CPU overwrite until the owning command completes.
- [x] Preserve 256-byte alignment and checked capacity growth.
- [x] Reuse completed buffers rather than allocating every command.
- [x] Cover large-draw overflow and allocation failure.

### Surface uploads

- [x] Give pending guest-to-private uploads frame-owned staging resources.
- [x] Prevent `replaceRegion` data from being changed before the GPU consumes
  it.
- [x] Retain exact guest snapshots as the CPU-write detector.
- [x] Keep first-use and forced-reseed behavior synchronous where required.

### Readback and guest publication

- [x] Give each pending readback a stable destination resource.
- [x] Track the color-surface entry and generation represented by each
  readback.
- [x] Publish completed readbacks to guest memory in submission order.
- [x] Define how a CPU write discovered while a private GPU generation is
  pending is merged or forces completion.
- [x] Prevent an older completed readback from overwriting a newer guest
  generation.
- [x] Invalidate texture-cache aliases after publication.

### Private surface ownership

- [x] Track private-authoritative, guest-authoritative, and synchronized
  states separately.
- [x] Track the newest submitted and completed generation per color surface.
- [x] Wait before reusing resources, not merely because a flip occurred.
- [x] Preserve target-switch behavior when the next target aliases depth or a
  sampled texture.
- [x] Keep the existing CPU-write counterexample behavior explicit.

### Direct presentation

- [x] Queue presentation after the producing render command on the same Metal
  queue.
- [x] Present directly from the correct private surface generation.
- [x] Avoid requiring guest publication only for display.
- [x] Preserve gamma, aspect ratio, drawable failure, and resize behavior.
- [x] Bound presentation latency independently from display refresh.

### Explicit synchronization boundaries

The following paths must wait for and publish the required generation:

- [x] framebuffer dumps;
- [x] screenshots and water captures;
- [x] render-to-texture feedback;
- [x] CPU-visible framebuffer snapshots;
- [x] target reinterpretation or aliasing;
- [x] resize and render-scale epoch changes;
- [x] backend stop drains owned work without accessing torn-down guest memory;
- [x] command failure recovery; and
- [x] any guest-visible GPU fence that promises completed rendering.

### Depth and water interactions

- [x] Preserve depth-surface ownership across asynchronous color work.
- [x] Preserve the per-view private reflection-depth copy.
- [x] Preserve lit depth-only priming before reflection filtering.
- [x] Reset water private-depth lifecycle on the same boundaries as today.
- [x] Report allocation or synchronization failure explicitly rather than
  silently presenting stale contents.

## Completed tests: PERF-013

Add focused synthetic coverage before enabling asynchronous submission:

- [x] vertex-upload buffer reuse while an older command remains pending;
- [x] staging texture reuse while an older upload remains pending;
- [x] two-buffer ownership and third-frame queue saturation;
- [x] cached surface reactivation before and after completion;
- [x] CPU write while a private generation is pending;
- [x] older readback versus newer guest generation;
- [x] render-to-texture feedback;
- [x] framebuffer descriptor and dump correctness;
- [x] immediate and normal snapshots;
- [x] resize with pending commands;
- [x] render-scale epoch change with pending commands;
- [x] drawable acquisition failure;
- [x] command-buffer failure;
- [x] bounded queue saturation;
- [x] shutdown with pending work;
- [x] water private-depth target switch;
- [x] Original-edition behavior; and
- [x] explicit failure instead of a stale-content allocation fallback.

Validation gates:

- [x] complete Metal suite passes 337/337;
- [x] seven focused ASan/UBSan ownership cases pass;
- [x] macOS presenter suite passes 62/62;
- [x] no broad catch or silent fallback is introduced;
- [x] runtime logs contain no sticky Metal error;
- [x] private Battle seed reaches its relaunch boundary;
- [x] marker rejection remains zero; and
- [x] guest snapshots match the synchronous reference cases.

## Completed matrix and active telemetry: PERF-014 and PERF-015

### Clean performance matrix

- [x] Run fixed 1x, 2.25x, and 4x.
- [x] Keep edition, save, launch record, frame cap, audio, and scene fixed.
- [x] Collect at least two independent runs per cell.
- [x] Retain the historical baseline and interleave final candidate scales.
- [x] Record rolling steady-state windows separately from startup outliers.
- [x] Record FPS, wall median/p95, GPU median/p95, and host-wait median/p95.
- [x] Record bounded queue, publication, command-role, and submission-latency
  metrics.
- [x] Retain executable SHA-256 and exact feature flags.

### Thermal and power evidence

- [ ] Record ambient and a controlled cold starting machine state.
- [x] Interleave final scale order and avoid attributing scene/process variance
  to throttling.
- [ ] Collect direct package-power and temperature telemetry if available.
- [x] Record that post-matrix `pmset -g therm` reports no thermal or
  performance warning.
- [x] Distinguish scene progression from time-under-load.
- [ ] Compare energy per completed frame, not FPS alone.

### Success interpretation

- [x] At 1x, wall time moves materially closer to GPU duration.
- [x] At 2.25x, improved overlap is visible without increasing p95
  instability or input latency.
- [x] At 4x, use 30 FPS rather than 60 FPS as the acceptance
  target.
- [x] Queue latency remains bounded under sustained load.
- [x] No quality reduction is counted as an implementation speedup.

## Product decisions: PERF-016

Retain Native/auto 4x, match-display frame rate, filtering, LOD, and Retail
water. Every exact-default 300-frame window averaged 31.89-34.65 FPS with a
28.75-29.92 ms wall median, so no quality-policy change is needed for the
30 FPS target. Native 4x/60 remains outside the demonstrated envelope.

## Explicitly unsafe shortcuts

Do not:

- delete `waitUntilCompleted` while reusing the current shared resources;
- reset the vertex-upload arena before its command completes;
- reuse one readback texture across pending commands;
- skip guest writeback without tracking private/guest authority;
- retain a cached private texture without exact CPU-write detection;
- publish completed readbacks out of order;
- report success after a command failure;
- hide failure by silently dropping to stale guest contents; or
- claim native 4x/60 from CPU/GPU overlap alone.

## Water-work boundary

The following remain paused, not cancelled:

- scene-color capture and refraction;
- depth-based absorption;
- screen-space reflection;
- modern foam and wake composition;
- shoreline treatment;
- geometry refinement; and
- final Black Flag-style composition.

PERF-014 now establishes the base-renderer budget. Resume these only through
their separate visual plan, with each phase charged against the measured
Native/auto envelope.

Related local record:

- [Performance investigation](PERFTESTS.md)

Related tracked documentation:

- [Modern water tracker](NEWWATER_TODO.md)
- [Validation evidence](docs/VALIDATION.md)
- [Metal quality and policy](docs/METAL_QUALITY.md)
- [Build and diagnostic workflow](docs/BUILDING.md)
