# Apple Silicon performance work tracker

Status: tracked performance tracker
Last updated: 2026-11-07

This tracker covers base-game frame rate, efficiency, and thermal work. The
bounded ownership, 30 FPS closure, and representative sustained Native 4x/60
target are complete without a quality reduction. The active continuation
turns that post-warm-up Apple M5 result into a cold-smooth, frame-paced,
broader-workload and broader-device claim before undertaking conditional
vertex or GPU-pass work.

## Status legend

- `[x]` complete and validated
- `[~]` active
- `[ ]` pending
- `[!]` blocked or requires a product decision

## Current objective

Preserve the accepted Native/auto 4x quality contract while removing
pipeline-related cold hitches, attributing and tightening presentation tails,
and qualifying representative missions, split-screen, long-duration runs, and
available Apple Silicon classes. The accepted Battle baseline remains
59.98-60.01 FPS with 14.87-15.26 ms combined-GPU p95 after warm-up; it is a
regression gate, not evidence for untested workloads.

PERF-028's exact historical-band reproduction remains open; its retained
attribution was sufficient for the requested continuation through the
completed PERF-029 artifact contract. PERF-030 is complete: Xcode's Metal
toolchain compiles both stable-library variants, real default/safe selection
loads them without source fallback, and the explicit fallback remains tested.
PERF-031 is complete: bounded private canonical-key traces are written
atomically, compatibility-validated, and preloaded before first use while
every rejection retains synchronous runtime creation.
PERF-032 is complete: compatible preloads enable bounded asynchronous
compilation for untraced specializations while the exact generic pipeline
renders; failures remain explicit and callback ownership is drained at stop.
PERF-033 is active: a controller-free private Battle trace captured all 25
observed states, and two clean exact-default seeded boots preloaded all 25
with zero cache misses, asynchronous work, or post-frame-zero pipeline
creation. Historical-band reproduction, input-latency/output-hash evidence,
and a small submission-tail regression remain open. PERF-034 instrumentation
now has representative actual-presentation evidence; the explicit presenter
still exposes no display target timestamp, and the measured wall tails do not
justify changing the default pacing path yet.

Render scale, filtering, LOD, Retail draw coverage, quincunx behavior, and
Retail-water defaults remain fixed. Continue collecting direct power and
temperature evidence when privileged telemetry is available.

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
| PERF-017 | Resume modern-water visual phases | `[ ]` | PERF-040 |
| PERF-018 | Freeze the Native 4x/60 contract and baseline | `[x]` | PERF-016 |
| PERF-019 | Instrument render-target feedback and quincunx cost | `[x]` | PERF-018 |
| PERF-020 | Keep render-target feedback GPU-private | `[x]` | PERF-019 |
| PERF-021 | Evaluate exact lower-read quincunx resolves | `[x]` | PERF-020 |
| PERF-022 | Consolidate multi-target submissions | `[x]` | PERF-020 |
| PERF-023 | Re-evaluate the bounded in-flight depth | `[x]` | PERF-022 |
| PERF-024 | Attribute remaining CPU vertex/draw cost if broader workloads miss | `[!]` | PERF-023, PERF-038 |
| PERF-025 | Port justified NV2A vertex programs to Metal | `[!]` | PERF-024 |
| PERF-026 | Repeat Native 4x/60 acceptance matrix | `[x]` | PERF-023 |
| PERF-027 | Reconcile product documentation and defaults | `[x]` | PERF-026 |
| PERF-028 | Instrument Metal pipeline and cache lifecycle | `[~]` | PERF-027 |
| PERF-029 | Define the native pipeline-artifact contract | `[x]` | PERF-028 |
| PERF-030 | Compile stable Metal shaders offline | `[x]` | PERF-029 |
| PERF-031 | Add device-local trace-derived pipeline warm-up | `[x]` | PERF-028, PERF-029, PERF-030 |
| PERF-032 | Add asynchronous rare-miss compilation and bounded retention | `[x]` | PERF-028, PERF-029, PERF-030 |
| PERF-033 | Qualify cold-start pipeline behavior | `[~]` | PERF-031, PERF-032 |
| PERF-034 | Instrument presentation and display timing | `[~]` | PERF-027 |
| PERF-035 | Evaluate a gated `CAMetalDisplayLink` path | `[ ]` | PERF-034 |
| PERF-036 | Qualify match-display pacing and latency | `[ ]` | PERF-035 |
| PERF-037 | Define the broader workload and device matrix | `[ ]` | PERF-027 |
| PERF-038 | Run broader Native 4x/60 qualification | `[ ]` | PERF-033, PERF-036, PERF-037 |
| PERF-039 | Optimize a measured GPU-bound miss | `[!]` | PERF-038 |
| PERF-040 | Requalify, reconcile defaults, and publish evidence | `[ ]` | PERF-033, PERF-036, PERF-038 |

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
30 FPS target. At that checkpoint, Native 4x/60 remained outside the
demonstrated envelope; PERF-018 through PERF-027 record its later closure.

## Native 4x/60 continuation: PERF-018 through PERF-027

### Frozen starting point and acceptance contract: PERF-018

- [x] Retain the representative `relaunch-001.bin` one-player Battle seed.
- [x] Retain Native/auto 4x, filtering, LOD, Retail draw coverage, quincunx
  approximation, and Retail water.
- [x] Use fixed 4x with a 120 FPS cap for uncapped qualification.
- [x] Report cold-start and relaunch-transition windows separately; qualify
  two consecutive rolling reports emitted at or after presented frame 900.
- [x] Require fixed-4x qualification windows to average at least 60 FPS and
  exact Native/auto windows to remain within 59.0-60.5 FPS.
- [x] Require exact-default wall median at or below 16.9 ms and combined
  render/present GPU median and p95 at or below 16.67 ms.
- [x] Require exact Native/auto with match-display pacing to remain near the
  59.94 Hz display limit rather than becoming renderer limited.
- [x] Reject material p95, submission-latency, input-latency, marker, or
  visual-regression increases.
- [x] Do not count lower scale, filtering, LOD, draw coverage, or changed
  Retail-water behavior as optimization.

The exact-default continuation baseline reports 31.89-34.65 FPS,
28.75-29.92 ms wall medians, 20.44-21.06 ms GPU medians, and
12.21-12.56 ms command-wait medians. A representative 300-frame role window
contains 100 presented and 200 offscreen commands. Presented work averages
12.31 ms; the known 640x480 offscreen target averages 5.56 ms; the remaining
unclassified offscreen command contributes roughly 4.1 ms per game frame.

### GPU-private feedback and quincunx resolve: PERF-019 through PERF-021

- [x] Add command reasons and CPU timing for feedback publication, quincunx
  texture preparation, allocation/upload, and filter encoding.
- [x] Count quincunx resolves and source generations per game frame.
- [x] Prove whether the unclassified third command is feedback publication or
  resolve preparation.
- [x] Borrow the authoritative cached private color texture when a sampled
  texture exactly aliases a tracked render-target generation.
- [x] Preserve CPU-write detection, overlap handling, explicit snapshots,
  dumps, captures, scale transitions, and error propagation.
- [x] Keep feedback dependencies ordered on the GPU without routine guest
  readback or CPU re-upload.
- [x] Replace steady-state allocation and texture creation with a
  submission-owned feedback-resolve texture.
- [x] Evaluate exact lower-read variants before accepting one. Exact 16-read
  and 24-read inline variants regressed the representative runtime; a grouped
  bilinear variant mismatched 18/4096 samples; a source-resolution prefilter
  mismatched 16/4096 samples and regressed GPU time. All were removed.
- [x] Retain the measured-faster exact 36-read legacy loop rather than trading
  correctness or representative performance for a lower theoretical read
  count.
- [x] Add edge, clamp, rounding, alias, CPU-write, generation, and failure
  tests before accepting the optimized path.

Instrumentation showed one synchronous feedback publication per game frame:
about 3.8-4.5 ms of GPU readback and 13.46-14.26 ms of blocked CPU time,
versus only about 0.33-0.37 ms for quincunx preparation. The retained private
path records zero steady-state publications and about 0.09-0.10 ms per
feedback check.

### Submission and CPU continuation: PERF-022 through PERF-025

- [x] Let one submission own an ordered set of color-surface generations so a
  target switch can end one render pass and begin another without committing
  solely to change ownership.
- [x] Preserve ordered retirement, sticky failure propagation, publication,
  and per-surface generation advancement.
- [x] Bound a command at eight target switches, then force a fallback commit.
- [x] Raise the in-flight bound from two to three only after feedback became
  GPU-private.
- [x] Reject a material submission/input-latency regression. Exact-default
  steady-state submission-latency p95 remained 15.23-15.50 ms.
- [!] Defer CPU vertex/draw attribution and a `draw_vsh` Metal port while the
  representative target clears its budget. Resume only if broader mission
  evidence identifies a CPU-bound miss.

### Final acceptance: PERF-026 and PERF-027

- [x] Pass focused ownership, feedback, quincunx, and failure tests.
- [x] Pass the complete 337-case Metal suite and seven focused ASan/UBSan
  cases.
- [x] Pass the complete 62-case macOS presenter suite.
- [x] Run two fixed-4x qualification boots and two exact-default Native/auto
  boots.
- [x] Record FPS, wall/GPU/wait percentiles, command reasons, submission
  latency, marker coverage, executable hash, and direct telemetry caveats.
- [x] Update tracked performance, validation, quality, roadmap, handoff, and
  completion records only from retained evidence.

The final exact-default boots selected 4x at 2560x1920. Their qualifying
windows measured 59.98-60.01 FPS, 16.66-16.68 ms wall medians,
14.18-14.84 ms GPU medians, and 14.87-15.26 ms GPU p95. Fixed-4x/cap-120
boots reached 59.36-100.78 FPS and 60.33-100.10 FPS across all reported
windows; both produced consecutive post-warm-up windows above 60 FPS. The
steady-state command shape is one
`render+target-switch+feedback+present+depth-copy` command per game frame.
Cold windows still contain 296-330 ms shader/workload warm-up stalls and are
retained as an explicit follow-up rather than silently discarded.

## Highest-value follow-up: PERF-028 through PERF-040

### Pipeline lifecycle and cold-start closure: PERF-028 through PERF-033

PERF-028's exact historical-band reproduction remains open. PERF-029 is
complete.

- [x] Time stable-library compilation, function specialization, and render
  pipeline creation separately.
- [x] Count canonical-key cache hits, misses, high-water entries, and
  whole-cache resets.
- [x] Correlate each event with frame number, command role, and wall-frame
  maximum on one time base.
- [x] Emit hashes and aggregate timing only; do not serialize pipeline
  descriptors, combiner words, retail traces, or generated archives.
- [ ] Repeat two clean cold seeded boots and attribute every pipeline event
  associated with the existing 296-330 ms maxima.

PERF-029 defines the artifact before implementation:

- [x] Version the backend ABI, stable shader hash, pipeline-key schema,
  executable, OS, SDK, Metal feature level, and GPU family.
- [x] Reject truncated, stale, incompatible, symlinked, or over-limit
  artifacts explicitly.
- [x] Keep trace-derived keys and compiled archives device-local and
  uncommitted; project-owned offline shader output remains a normal build
  artifact.
- [x] Preserve runtime compilation as an explicit fallback when the selected
  API, archive, or artifact is unavailable.

PERF-030 through PERF-032 implement the measured path:

- [x] Compile the stable project-owned Metal source into an offline library
  without changing shader entry points or math policy.
- [x] Preload validated local pipeline states before their first use in the
  traced workload.
- [x] Compile an untraced specialization asynchronously and retain the exact
  generic Retail pipeline until it is ready.
- [x] Propagate compilation failure explicitly and keep rendering exact;
  never return success with a missing pipeline or stale state.
- [x] Keep the measured 128-entry whole-cache policy unchanged; PERF-028
  reached only 25 entries with zero resets, while rare-miss state is separately
  bounded to 32 coalesced entries.

PERF-030 implementation preserves one 20,571-byte project-owned `.metal`
source and generates default-math and safe-math embedded libraries when both
Apple compiler tools are present. Missing or rejected libraries are reported
and compile the identical source bytes synchronously. Generator, fallback,
rejection, failure, sanitizer, full-suite, and production-build coverage pass.
Xcode 27.2 with Metal Toolchain 27B47 compiles both real libraries. Default
and safe-math selection tests load the selected library with one timed load,
zero runtime source compilations, and zero fallback. The production executable
also loaded the offline library in two private bounded Battle-seed attempts.
Those attempts displayed the controller-reconnect overlay, so their frame
maxima are not retained as exact-default performance evidence and do not
advance PERF-033.

PERF-031 records at most 128 successful canonical draw keys in a deterministic
little-endian kind-1 `.bwmpa` payload, matching the unchanged 128-state cache
limit. Capture is opt-in, writes only to an owner-controlled directory through
an atomic owner-only file replacement, and emits bounded aggregate status
without paths or key contents. Warm-up validates the complete PERF-029
identity, payload digest, fixed key schema, ordering, uniqueness, and semantic
fields before creating any state. A missing, stale, corrupt, malformed,
unsupported, or failed artifact clears any partial preload and uses the exact
synchronous runtime path.

A private production smoke pair without a physical controller captured and
preloaded 15 paused-renderer states. The preload run reported all 15 compatible
states loaded before use, then more than one million cache hits and zero
misses. Because the title remained paused behind its reconnect overlay, those
artifacts are not representative gameplay traces, their frame timings are not
retained, and PERF-033 remains open.

PERF-032 activates only after a fully compatible PERF-031 preload. An untraced
specialized miss immediately uses the existing exact generic extended shader
while Metal specializes the function and creates the render pipeline
asynchronously. Duplicate canonical keys coalesce in a fixed 32-entry queue.
Completion callbacks transfer only bounded state under a lock; the serial
renderer installs ready states at draw or flip boundaries, and stop waits for
all callbacks before releasing Metal objects. Allocation, function, pipeline,
timing, and capacity outcomes remain explicit in bounded aggregate JSONL.
Missing or rejected artifacts retain the unchanged synchronous path.

Ten focused async scenarios cover default-disabled behavior, exact generic
output, successful specialization/pipeline timing, duplicate coalescing,
allocation/function/pipeline failures, the 32-entry bound, stable private
reporting, pending-stop ownership, and restart isolation. They pass normally,
under ASan/UBSan, and under ThreadSanitizer. The complete Metal backend suite
passes 363/363, strict Objective-C warnings pass, and the production
offline-library target rebuilds. The changed strict-C test translation unit
compiles cleanly; the aggregate warning target is currently blocked later by
a pre-existing `-Wcomment` error in the pinned toolkit's `kernel.h`. No
no-controller result is treated as gameplay or cold-performance evidence;
PERF-033 remains open.

The diagnostic keyboard bridge now provides a neutral connected controller
without requiring a physical pad. Live guided input was used only to calibrate
additional spaced A confirmations in the deterministic menu script. A fresh
direct seed then ran with no scripted or guided input, captured 25 canonical
states across 273,229 validated Battle scopes, and produced a complete private
artifact with zero drops.

Two subsequent exact-default Native/auto boots used that same private
save/relaunch seed and compatible artifact. Both preloaded all 25 states,
reported zero cache misses, zero asynchronous scheduling/fallback, zero
whole-cache resets, and no function-specialization or pipeline-creation event
after frame 0. Their retained wall maxima were 227.223 ms and 225.080 ms at
frame 1, with no Metal, allocation, non-monotonic timing, marker-rejection, or
malformed-report failure. The one pre-first-flip wall correlation per boot
remained explicitly unavailable. They retained 281,832 and 258,621 validated
Battle scopes.

Selected consecutive post-frame-900 windows remained within the 59.0-60.5 FPS
and 16.67 ms GPU budgets: 59.66-60.17 FPS, 16.60-16.74 ms wall medians,
14.56-14.79 ms GPU medians, and 15.48-15.79 ms GPU p95. Wall p95 was
17.32-17.95 ms and submission p95 was 15.67-15.80 ms, so the prior wall and
submission tails are not declared non-regressed. No physical input-latency or
output-hash comparison was collected. The historical 296-330 ms band also
did not reproduce, so its attribution item remains open.

PERF-033 exit gates:

- [x] Two clean exact-default cold boots complete with zero renderer-thread
  specialized-pipeline creation after the first seeded gameplay flip.
- [ ] No retained 296-330 ms maximum is attributed to Metal library,
  specialization, pipeline creation, archive load, or cache churn.
- [ ] Steady-state FPS/GPU/submission percentiles, input latency, markers,
  output hashes, and Metal error behavior do not regress.
- [x] Missing, stale, corrupt, and unsupported artifacts take the tested
  explicit fallback.

### Presentation pacing: PERF-034 through PERF-036

PERF-034 reuses the disabled-by-default exact-value
`BW_METAL_FRAME_TIMING=1` switch. It retains at most 512 flip records and
streams each completed record once as
`bloodwake.metal-presentation-timing/v1`, with a final aggregate summary when
backend stop runs. Flip stall/release, drawable acquisition, present request,
command commit/completion, and the actual-presentation callback use the
existing monotonic host clock. Valid `MTLDrawable.presentedTime` values are
converted onto that clock at callback time; invalid or unavailable values
remain explicit. Exact nanosecond intervals are never rounded to an assumed
integer refresh rate.

The current explicit `presentDrawable:` API exposes no display target
timestamp, so its status is `unavailable_explicit_presenter` with a null
value. A target timestamp is available only through the gated display-link
experiment in PERF-035. Eleven focused lifecycle, streaming, paced-release,
fractional/variable interval, failure, bound, allocation, callback-stop, and
restart cases plus the machine-readable integration case pass normally,
under ASan/UBSan, and under ThreadSanitizer. The complete Metal backend suite
passes 375/375, strict C and Objective-C validation pass, and the production
offline-library target rebuilds.

A private exact-default paused-renderer smoke first validated callback and
explicit-unavailability mechanics. The later seeded Battle pair retained 512
events per run. It reported 507 and 508 actual presentations, exact
16.667 ms median intervals, variable 33.333/50.000 ms missed-present
intervals, and no malformed or command-completion failure. Every explicit
presenter record still reported its display target unavailable.

Drawable acquisition reached no 5 ms tail in either retained gameplay sample,
and the existing manual hold exceeded 8 ms on only one and two wall-tail
frames. Command completion and missed actual presents contributed to the
second run's cold tail, while most 17 ms wall tails had neither a long
drawable wait, long hold, 16 ms command completion, nor missed present. Exact
target timestamps remain necessary to separate residual scene/OS scheduling,
so causal attribution and a display-link change remain gated.

- [ ] Correlate flip release, command commit/completion, drawable acquisition,
  requested presentation, actual presentation, and display target timestamps.
- [x] Record fractional and variable display-rate changes rather than rounding
  the display to an assumed fixed integer cadence.
- [ ] Prove whether drawable wait, manual host pacing, command completion, OS
  scheduling, or scene work causes the 17.06-17.46 ms wall p95.
- [ ] If pacing is material, add an availability-gated
  `CAMetalDisplayLink` match-display path with target timestamps and the
  display-link-provided drawable.
- [ ] Keep the existing bounded explicit-drawable path for vsync-off,
  uncapped, unsupported, initialization-failure, and test configurations.
- [ ] Drain owned submissions and preserve title flip timing before switching
  pacing modes.

PERF-036 accepts the new path only if two exact-default boots retain
59.0-60.5 FPS, combined-GPU p95 at or below 16.67 ms, submission-latency p95
at or below 15.50 ms, wall p95 at or below 17.0 ms, no new dropped drawable,
and no material input-latency or frame-order regression. Otherwise retain the
current default and record the experiment as no-go.

### Broader qualification: PERF-037 and PERF-038

- [ ] Define private seeds for representative geometry, particle, weather,
  calm/storm water, shore, reflection, transition, and split-screen loads.
- [ ] Include multiple missions, 30-60 minute sustained runs, cold and warm
  windows, and every available Apple Silicon class.
- [ ] Record FPS, wall/GPU median/p95/p99/max, submission latency, pipeline
  events, drawable events, marker coverage, approximation flags, executable
  hash, and direct power, temperature, sustained-clock, and memory evidence
  where approved.
- [ ] Report unavailable hardware or privileged telemetry as an evidence gap;
  do not infer a pass from the Apple M5 result.
- [ ] Preserve exact defaults and distinguish loading, transition, and
  steady-state windows.

### Conditional headroom and closure: PERF-024, PERF-025, PERF-039, PERF-040

PERF-024/PERF-025 activate only for a CPU-bound matrix miss. Attribute vertex
fetch, fixed-function transform/lighting, NV2A vertex-program interpretation,
draw preparation, and submission before porting `draw_vsh`. Any Metal port
must retain exact CPU comparison, bounded pipeline compilation, unsupported
program fallback, and CXT_WRITE behavior.

PERF-039 activates only for a GPU-bound miss. Use Metal/Xcode pass counters to
distinguish vertex, fragment, texture-filtering, bandwidth, depth/stencil,
feedback, copy, and presentation limits. Rejected lower-read quincunx
variants, unbounded in-flight work, lower quality, and unevidenced MSAA are
not candidates.

PERF-040 repeats cold and warm acceptance after every activated conditional
task, updates product defaults only from retained evidence, and blocks
PERF-017 until the accepted base-performance envelope and remaining gaps are
documented.

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
