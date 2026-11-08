# Apple Silicon performance investigation

Status: tracked performance record
Last updated: 2026-11-07

This document records the base-game frame-rate and efficiency investigation
that temporarily takes priority over later modern-water visual work. The
Native 4x/30 closure and representative sustained Native 4x/60 continuation
are complete at unchanged quality. The next program targets cold pipeline
stalls, presentation pacing, broader workload/device qualification, and only
then evidence-gated CPU or GPU headroom. It separates measured behavior from
hypotheses and product-policy decisions. Retail-derived saves, launch records,
pipeline traces and archives, profiles, logs, frames, and prepared game data
remain private and must not be committed.

## Investigation goals

1. Verify whether the displayed low frame rate is real.
2. Separate diagnostic overhead from normal renderer cost.
3. Measure resolution scaling independently from CPU/GPU serialization.
4. identify the highest-confidence optimization that preserves rendering,
   guest-memory, and gameplay behavior.
5. Avoid silently reducing visual quality or changing frame-rate policy.
6. Continue from the validated Native 4x/30 envelope to Native 4x/60 without
   reducing scale, filtering, LOD, draw coverage, or Retail-water behavior.
7. Remove runtime pipeline-compilation hitches without weakening exact Retail
   fallback or committing retail-derived pipeline artifacts.
8. Attribute wall-frame tail latency before changing presentation pacing.
9. Qualify cold start, broader missions, split-screen, long-duration behavior,
   and available Apple Silicon device classes.
10. Port CPU vertex work or optimize additional GPU passes only when a failing
    workload and measured bottleneck justify the change.

## Workload and controls

The representative workload is one-player Battle gameplay launched from the
validated private Remastered save and same-title relaunch record captured
during water validation.

Clean performance runs use:

- no full `BW_GPU_AUDIT`;
- no periodic BMP capture;
- the lightweight `BW_METAL_FRAME_TIMING=1` path;
- a fixed Metal render scale;
- a fixed Remastered frame cap of 120 for uncapped comparison;
- the experimental keyboard pad so the title sees a connected controller;
- no synthetic menu input after launch; and
- the same private save and launch seed for every scale.

The keyboard hook is required even when no buttons are automated. Omitting it
causes the disconnected-controller pause prompt.

Status 124 is the expected result when a bounded diagnostic reaches its
configured run limit. A hard timeout may interrupt a water view between its
begin and end markers; zero marker rejection is more important than a single
final unmatched boundary in a short performance run.

## Diagnostic support added

The investigation added or exposed:

- `BW_METAL_FRAME_TIMING=1`, independent of full GPU audit;
- completed Metal command-buffer GPU durations;
- host time spent waiting for command completion;
- rolling 300-frame FPS, median, p95, p99, and maximum reports;
- fixed `--render-scale` and `--frame-cap` diagnostic controls;
- native approximately 1 kHz profiling with optional caller attribution;
- bounded private save and same-title launch seeding;
- run-limit profile dumping before the intentional status-124 exit; and
- surface-cache reuse and forced-reseed counters.

The window counter was renamed to `Game FPS` because it measures completed
guest flips, not display refresh or host event-loop frequency.

## FPS counter verification

The approximately 14 FPS result at 4x was not just a bad counter.

- The title-bar counter measures completed guest flips using host wall time.
- Metal's independently recorded completed-flip intervals agreed with the
  title-bar result.
- Stable approximately 59.94 Hz vblank activity does not mean the title is
  producing 59.94 new frames per second.
- Full GPU audit and BMP capture added overhead, but clean seeded Battle runs
  reproduced the same general 4x range.
- A separate executor reporter incorrectly used process CPU time and was
  corrected, but that reporter was not the source of the title-bar result.

## Clean baseline matrix

The following values predate the synchronized private-surface reuse
optimization.

| Fixed scale | Internal target | Completed-game FPS | GPU command median | Command-wait median | Wall-frame median |
| --- | --- | --- | --- | --- | --- |
| 1x | 640x480 | 30.53-31.89 | 17.16-17.23 ms | 17.77-17.90 ms | 30.48-30.72 ms |
| 2.25x | 1440x1080 | 28.37-29.60 | 20.13-20.35 ms | 20.76-20.95 ms | 33.38-33.65 ms |
| 4x | 2560x1920 | 15.76 | 48.25 ms | 48.84 ms | 62.03 ms |

A separate two-minute clean 4x run began near 14.8 FPS and later measured
about 12.7 FPS. This is consistent with increasing sustained load, but the
scene was progressing and direct package-power and thermal telemetry were not
recorded. It is not proof of thermal throttling.

### Resolution implications

- 4x renders 16 times the console pixel count.
- A 48.25 ms GPU median imposes a GPU-only ceiling of about 20.7 FPS.
- A 20.35 ms GPU median at 2.25x imposes a ceiling of about 49 FPS.
- A 17.23 ms GPU median at 1x is close to a 58 FPS GPU budget.
- Therefore perfect CPU/GPU overlap cannot make the measured 4x workload
  reach 60 FPS.
- The large gap between the 1x GPU budget and approximately 32 FPS wall rate
  shows a separate fixed serialization cost.

## Native sampling evidence

Caller-attributed profiling of the 4x Battle workload found:

| Thread | Dominant sample | Share |
| --- | --- | --- |
| Guest/title thread | condition wait called by `bw_flush_wait` | 87.3% |
| NV2A/Metal worker | condition wait called by `finish_command` | 79.3% |

The remaining active Metal-worker samples were led by CPU vertex-program
evaluation, draw submission, and AGX texture compression/decompression.

This means the CPU is mostly blocked rather than broadly saturated. The guest
waits for the NV2A worker, while the NV2A worker waits for Metal command
completion.

## Synchronous command path

The important path is:

1. `metal_present_sync()` requests a current guest-visible framebuffer.
2. `writeback()` encodes private-surface resolve/readback work.
3. `flush_commands()` ends encoders and submits the command buffer.
4. `finish_command()` calls `waitUntilCompleted`.
5. The completed readback is copied into guest framebuffer memory.

Target changes also call `writeback()`. The old implementation additionally
reseeded cached private color targets from guest memory even when a preceding
writeback had just made both copies identical.

Removing `waitUntilCompleted` by itself is unsafe because the current design
also shares:

- one vertex-upload arena;
- one color-surface upload texture;
- one color-surface readback texture;
- private color and depth targets with guest-memory publication rules; and
- snapshot, framebuffer-dump, feedback, resize, error, and shutdown paths.

## First ownership-safe optimization

Cached Metal color surfaces now keep an exact guest-resolution synchronization
snapshot.

On target activation, the private texture is reused only when:

- the cached surface identity matches;
- format, dimensions, pitch, and AA layout match; and
- every active guest byte matches the stored snapshot.

A CPU or copy-engine change forces the established guest-to-private seed path.
GPU writeback refreshes guest memory and the snapshot. Uploads use the stored
snapshot bytes so the comparison and uploaded contents cannot diverge.

This optimization does not remove command completion or guest writeback. It
only removes redundant target reseeds while preserving the existing ownership
contract.

### Cache-reuse measurements

Every scale recorded 599 target reuses and one present-time guest reseed in
its first 300-frame report. Longer runs accumulated thousands of target
reuses without Metal errors or rejected water markers.

| Fixed scale | Baseline median | First optimized median | Later optimized range |
| --- | --- | --- | --- |
| 1x | 17.16-17.23 ms GPU / 30.48-30.72 ms wall | 16.76 ms GPU / 30.02 ms wall | 16.77-17.40 ms GPU / 29.72-30.73 ms wall |
| 2.25x | 20.13-20.35 ms GPU / 33.38-33.65 ms wall | 19.75 ms GPU / 32.99 ms wall | 20.48-22.40 ms GPU / 33.76-36.77 ms wall |
| 4x | 48.25 ms GPU / 62.03 ms wall | 43.38 ms GPU / 56.75 ms wall | 44.56-50.27 ms GPU / 58.46-65.24 ms wall |

The final 1x smoke run produced:

- 16.70 ms first-window GPU median;
- 17.23 ms command-wait median;
- 29.28 ms wall-frame median;
- 32.13 FPS average including the long startup outlier;
- 599 target reuses and one present guest reseed; and
- no Metal runtime error.

### Interpretation

- The exact cache path is active and correct.
- It modestly improves 1x wall time.
- The first 4x window suggests a useful reduction in redundant scaled-surface
  work.
- Later 2.25x and 4x windows show that scene progression and sustained load
  can outweigh the gain.
- These were repeat runs, not interleaved process-level A/B samples with
  controlled temperature, so no precise percentage is claimed.
- GPU duration and host wait remain nearly identical. Per-frame synchronous
  completion is still the dominant architecture boundary.

## Bounded ownership and 30 FPS closure

The completed ownership path separates display preparation from explicit
guest publication. Normal direct presentation keeps the newest color
generation private, while snapshots, dumps, feedback, captures, scale
changes, and other CPU-visible paths wait and publish explicitly.

At the 30 FPS closure, mutable vertex, surface-upload, and surface-readback
resources belonged to one of two bounded submissions. Queue saturation
retired only the oldest
submission. Completion, failure, color generation, and guest publication were
processed in order on the executor thread. A CPU change detected while a
private generation was newer was merged at 32-bit pixel granularity before
the surface was reseeded.

Ordinary render-target switches no longer resolve, wait, and publish the old
target. They commit its private generation asynchronously. Full attachment
clears remain in the first matching draw pass. Finally, immutable texture
stage, register-combiner, final-combiner, and alpha-test routing is supplied
to Metal as function constants. Material colors, fog, factors, alpha
reference, textures, and samplers remain dynamic. This lets Metal compile the
same Retail equations without executing the generic routing machinery for
every fragment.

### Representative 4x progression

The first ownership-only representative run still measured 16.50 FPS,
59.42 ms wall median, 52.92 ms combined render/present GPU median, and
43.39 ms queue-wait median. Fusing the clear changed this only to 16.78 FPS
and 58.83 ms wall median. Asynchronous target switching reduced wall median
to 53.66-55.98 ms but left about 53-54 ms of GPU work.

Command-role timing then attributed about 37.2 ms per frame to the selected
1280x480 AA target plus presentation, about 6.6 ms to the 640x480 alternate
target, and the balance to other queued work. Texture-stage-only function
specialization reduced GPU median to 34-38 ms. Full combiner-routing
specialization closed the remaining budget without changing scale, draw
coverage, filtering, LOD, or Retail water behavior.

Two independent explicit 4x runs produced these ranges across every reported
rolling 300-frame window:

| Metric | Final range |
| --- | --- |
| Completed-game FPS average | 32.08-36.14 FPS |
| Wall-frame median | 27.90-29.34 ms |
| Wall-frame p95 | 33.26-35.63 ms |
| Combined render/present GPU median | 15.66-20.75 ms |
| Queue-wait median | 9.36-12.45 ms |
| Submission-latency p95 | 12.33-15.89 ms |

Both runs retained the expected Battle water workload and zero rejected
markers. The exact Native/auto, match-display run selected 4x
(2560x1920) and reported 31.89-34.65 FPS, 28.75-29.92 ms wall medians,
32.73-35.89 ms wall p95, and 20.44-21.06 ms GPU medians. Thus every measured
300-frame default window exceeded 30 FPS and stayed below the 33.3 ms median
gate. The p95 and submission-latency results improved materially rather than
regressing.

The final two-run matrix was:

| Fixed scale | Internal target | FPS range | GPU median | Wait median | Wall median | Wall p95 |
| --- | --- | --- | --- | --- | --- | --- |
| 1x | 640x480 | 38.50-45.30 | 7.52-8.21 ms | 7.12-7.78 ms | 21.84-26.07 ms | 24.76-28.03 ms |
| 2.25x | 1440x1080 | 37.95-47.62 | 10.61-11.57 ms | 7.40-8.00 ms | 21.17-25.04 ms | 23.94-28.45 ms |
| 4x | 2560x1920 | 32.08-36.14 | 15.66-20.75 ms | 9.36-12.45 ms | 27.90-29.34 ms | 33.26-35.63 ms |

The new GPU metric includes direct presentation because rendering and
presentation now share command buffers; it is not directly comparable to the
old render-only baseline. Wall-frame throughput is the stable comparison.

## Validation completed

- Metal backend suite: 337/337 passed.
- Seven focused ownership/publication cases passed under ASan/UBSan.
- The macOS presenter suite passed 62/62.
- The production synthetic-input runtime rebuilt successfully.
- Two seeded 1x, 2.25x, and 4x Battle runs and an exact-default Native/auto
  run completed with zero marker rejection.
- No tested run reported a Metal command-buffer failure.
- Post-matrix `pmset -g therm` reported no thermal or performance warning.
  Direct package-power and temperature telemetry remains unavailable without
  privileged instrumentation, so no energy-per-frame claim is made.
- The final measured executable had SHA-256:
  `fee23401f7c42a13744b5c1bd6521bbc22ea93cbffe8c17e091337f693496891`.

## Native 4x/60 continuation

The frozen exact-default starting point was 34.65 FPS, 28.75/32.73 ms
wall median/p95, 21.00/25.71 ms combined GPU median/p95, and
12.50/15.09 ms command-wait median/p95. Each game frame produced one
presented and two offscreen commands.

### Feedback attribution and GPU-private resolve

Exact command-reason and CPU timing proved that the dominant feedback cost
was not allocation alone:

- one `surface-readback+feedback` command per game frame consumed about
  3.8-4.5 ms of GPU time;
- each feedback check blocked the CPU for 13.46-14.26 ms;
- guest-derived quincunx allocation/upload cost only 0.33-0.37 ms; and
- each 100 game frames produced 100 presented and 200 offscreen commands.

The retained fast path recognizes an exact tracked XRGB anti-aliased
render-target alias. If guest bytes still match the synchronization snapshot,
it resolves the authoritative private generation directly into one
submission-owned logical-size texture and samples it in queue order. If any
guest pixel changed, the old wait/readback/32-bit merge/reseed path remains.
Steady-state publications then fell to zero and feedback CPU time to
0.09-0.10 ms per check.

With the original two-submission bound, the private path reached 53.80 FPS,
17.42 ms wall median, and 17.07 ms GPU median in its best 300-frame window.
Raising the bounded depth to three removed measured command waits and produced
later windows at 60.00-60.06 FPS with 16.63-16.64 ms wall medians and
15.91-16.62 ms GPU medians.

### Rejected lower-read quincunx experiments

The retained shader still evaluates the exact legacy 3x3 Gaussian through
nine explicit bilinear operations, up to 36 reads per output fragment. Four
lower-read approaches were measured and removed:

1. an exact 16-read cached-neighborhood shader passed the reference tests but
   regressed representative runtime timing;
2. four grouped bilinear operations mismatched 18/4096 reference samples by
   one channel value;
3. an exact 24-read row-blocked shader passed the reference tests but also
   regressed runtime timing; and
4. a padded source-resolution prefilter reduced the final shader to four
   reads, but mismatched 16/4096 samples and raised combined GPU timing to
   about 17.2-17.5 ms.

The lower theoretical read count did not survive exact-output and runtime
measurement, likely because of register pressure, scheduling, and the added
pass. The original exact loop is therefore the accepted implementation.

### Multi-target consolidation and final qualification

One submission now owns every color-surface generation touched in order.
Target switches end and begin render passes without a command-buffer commit;
eight switches force a bounded fallback commit. Three submission-owned
resource sets remain the global in-flight limit. The representative command
shape fell from two commands per frame to one
`render+target-switch+feedback+present+depth-copy` command.

Two exact Native/auto boots selected 4x, 2560x1920, and a 59.94 Hz
match-display cap. Their consecutive post-warm-up qualification windows were:

| Metric | Two-boot range |
| --- | --- |
| Completed-game FPS | 59.98-60.01 FPS |
| Wall-frame median | 16.66-16.68 ms |
| Wall-frame p95 | 17.06-17.46 ms |
| Combined GPU median | 14.18-14.84 ms |
| Combined GPU p95 | 14.87-15.26 ms |
| Submission-latency p95 | 15.23-15.50 ms |
| Steady-state feedback | 100 GPU-private, 0 fallback/publication per 100 checks |

Two fixed-4x, cap-120 boots measured 59.36-100.78 FPS and
60.33-100.10 FPS across all reported windows. Both produced consecutive
post-warm-up windows above 60 FPS; the second exceeded 60 FPS in every
complete window despite a 306.86 ms cold-start maximum. No retained run
reported a rejected ocean marker or Metal command failure.

Cold exact-default windows are retained separately. They averaged as low as
53.22 FPS and contained 296-330 ms startup maxima while specialized pipelines
and the seeded workload warmed. The 59.94 Hz cap cannot compensate for such a
stall with faster frames, so qualification uses consecutive rolling reports
emitted at or after presented frame 900 and before the guest relaunch
transition. Cold
pipeline warm-up remains an honest follow-up; it is not silently counted as
steady-state performance.

The final executable SHA-256 is
`3516f2e2906347880bcf20dd8f2966a0c32b46776e620923d694f097e4d37562`.
The complete 337-case Metal suite, seven focused ASan/UBSan ownership and
feedback cases, and all 62 macOS presenter cases pass.

## Highest-value continuation program

The accepted result has only 1.41-1.80 ms of combined-GPU p95 headroom under
16.67 ms, and exact-default wall p95 remains 17.06-17.46 ms. The next work
therefore targets user-visible stalls and tail latency before speculative
steady-state rewrites.

### 1. Pipeline lifecycle, warm-up, and archives

The backend loads its stable Metal library offline when available and retains
identical-source compilation as an explicit fallback. Validated trace-derived
states preload synchronously before the first draw; compatible preloads enable
bounded asynchronous handling for untraced specializations. The in-memory
draw-pipeline cache still clears all entries when it reaches 128 states.

The first active task records:

- stable-library compile time;
- function-specialization and render-pipeline creation time;
- canonical pipeline-key hashes, cache hits, misses, high-water count, and
  whole-cache resets;
- the frame and command role associated with each creation; and
- a machine-readable cold-start summary that attributes every pipeline event
  without serializing retail-derived state.

#### PERF-028 implementation evidence

`BW_METAL_PIPELINE_TIMING=1` is an exact-value, disabled-by-default opt-in.
It uses the existing monotonic host clock and emits
`bloodwake.metal-pipeline-lifecycle/v2` JSONL. Expensive events retain only a
canonical-key hash, phase, duration or explicit timing state, frame, command
role, wall-frame interval, and explicit operation status. Cache hits are
aggregate-only so hot draws cannot displace cold misses. Storage is fixed at
512 events and output at 64 summaries; overflow is counted. No descriptor,
combiner word, function-constant value, texture identity, guest address, or
other retail-derived field is formatted.

Six focused cases cover the disabled and invalid switch, real library /
specialization / pipeline classification, hit/miss/high-water/reset
accounting, the 128-entry whole-cache reset, fixed storage, stable output,
clock/allocation failure, and prohibited-field absence. All six pass normally
and under ASan/UBSan with leak detection disabled. The complete Metal backend
suite passes 343/343, and the isolated diagnostic runner passes 61/61.

#### PERF-028 runtime evidence

Two clean seeded exact-default Native/auto Battle boots used the same private
save and validated same-title launch seed, Retail water, match-display pacing,
and the accepted quincunx approximation. No GPU audit, capture, profile,
render-scale override, or frame-cap override was present. Both selected 4x
internally, reported zero rejected ocean markers, retained all 79 pipeline
events with zero drops, and reported no timing, allocation, or Metal failure.
The individual hash sequence and all logs remain private.

| Metric | Cold boot 1 | Cold boot 2 |
| --- | ---: | ---: |
| Stable-library compile | 233.197 ms | 209.109 ms |
| Function specialization | 24 attempts, 223.487 ms total | 24 attempts, 215.459 ms total |
| Render-pipeline creation | 29 attempts, 333.387 ms total | 29 attempts, 323.634 ms total |
| Draw-cache lifecycle | 738,606 hits, 25 misses | 745,516 hits, 25 misses |
| Cache high-water / resets | 25 / 0 | 25 / 0 |
| Maximum correlated wall frame | 272.507 ms at frame 1 | 267.984 ms at frame 10 |

Boot 1's maximum contained two misses, two specializations, and two pipeline
creations totaling 48.439 ms; a second 263.333 ms cold frame contained ten
misses, nine specializations, and ten pipeline creations totaling 243.255 ms.
Boot 2's maximum contained that same 10/9/10 event shape totaling 235.055 ms.
Its frame-1 267.002 ms interval contained the two-miss event shape totaling
47.378 ms. Thus one cold maximum in each run is predominantly synchronous
specialization/pipeline work, while the other includes roughly 219-224 ms not
spent in those measured Metal phases. Stable-library compilation occurs
before the first correlated flip interval and contributes to boot latency,
not these wall-frame maxima.

The valid pair did not reproduce the historical 296-330 ms band, so the exact
PERF-028 runtime exit item remains open rather than being inferred from the
267.984-272.507 ms maxima. It does prove that the 128-entry reset policy is
not involved: the repeatable working set reached only 25 entries and never
reset. PERF-029 should therefore define strict compatibility and rejection
metadata for the stable library and the repeatable device-local canonical-key
set before any archive or warm-up implementation; it should not change cache
retention policy on this evidence.

#### PERF-029 implementation evidence

PERF-029 adds the versioned
`bloodwake.metal-pipeline-artifact/v1` compatibility envelope and validator.
The fixed 176-byte little-endian header keys backend ABI, stable-shader
SHA-256, pipeline-key schema, executable SHA-256, exact OS and build-SDK
versions, Metal feature level, GPU family, artifact kind, payload size and
entry count, and payload SHA-256. Total files are capped at 64 MiB and 4,096
entries. The validator reads with fixed stack storage and does not create,
load, or interpret a Metal library, trace payload, or binary archive.

Only a fully compatible result selects an artifact. Missing, unsupported,
truncated, trailing, malformed, stale, incompatible, symlinked,
policy-rejected, over-limit, corrupt, and I/O results retain distinct stable
status strings and select explicit runtime compilation. Stable-shader and
executable hash mismatches are stale; ABI, key-schema, artifact-kind, OS, SDK,
feature-level, and GPU-family mismatches are incompatible.

Device-local validation requires an absolute `.bwmpa` path, `O_NOFOLLOW`, a
private single-link regular file owned by the current user, exact declared
length, payload-hash verification, and unchanged file metadata across the
read. The envelope contains no descriptors, combiner words, function
constants, texture identities, addresses, or raw retail-derived state.
`.bwmpa` is ignored and blocked by the pre-commit guard. Project-owned offline
shader output remains a normal build product outside this private envelope.

Six focused compatibility, stale, incompatible, bounds/format, file-policy,
and corruption cases pass normally and under ASan/UBSan with leak detection
disabled. The complete Metal backend suite passes 349/349, and the production
synthetic-input target rebuilds successfully. These are implementation
results only: PERF-029 generated no runtime trace, key set, `.metallib`, or
binary archive and changed no pipeline selection behavior.

#### PERF-030 implementation evidence

The stable shader is now one 20,571-byte project-owned
`src/gpu/bw_metal_shaders.metal` source with SHA-256
`3fda260b7e0584eeea7d6a6d55229854dd67b310a447f9c1b45e21f617a2dec7`.
The extracted bytes exactly match the former runtime C string. A deterministic
bounded generator validates all 15 existing entry points and embeds the source
plus paired default-math and `-fno-fast-math` libraries. No shader entry point,
equation, constant index, or runtime pipeline key changed.

When Xcode supplies `metal` and `metallib`, CMake compiles and embeds both
variants. Startup first loads the variant matching the existing quincunx math
policy and requires every entry point before selecting it. Missing, rejected,
allocation-failed, or incomplete offline data retains an explicit status and
compiles the identical source bytes. A failed source fallback remains a
sticky backend failure. Pipeline lifecycle schema v2 reports library load,
rejection/unavailability, fallback count, and source compilation separately.

The initial Command Line Tools validation exercised the explicit source
fallback, while a deterministic fake toolchain proved the offline CMake branch,
paired embedding, rejection, and fallback behavior. Real-toolchain validation
then used Xcode 27.2 build 27B52, Metal Toolchain 27B47, and Apple metal
32023.922. A fresh Release configuration reported that stable Metal shaders
would be compiled offline, compiled both variants, embedded them, and built the
backend test executable.

The default-math and safe-math selection cases both load the real selected
library and assert one timed offline load, zero runtime source compilations,
zero fallbacks, and zero rejections. The complete eight-case focused set passes
normally and under ASan/UBSan with leak detection disabled. The complete Metal
backend suite passes 354/354. Strict Objective-C warnings pass, and a
reconfigured production synthetic-input target reports offline shader
compilation and rebuilds successfully.

Two private bounded production Battle-seed attempts provide runtime selection
evidence only. Each reported an available and loaded offline library, one
timed load (0.078 ms and 0.118 ms), zero runtime source compilations, zero
fallbacks/rejections, zero dropped events, and zero timing, allocation, or
Metal failures. The host had no physical controller and the title displayed
its reconnect overlay, so neither attempt is retained as exact-default cold
performance evidence; their frame maxima do not advance PERF-033. The logs,
save data, relaunch record, event hashes, and other private artifacts remain
outside the repository.

#### PERF-031 implementation evidence

PERF-031 defines the kind-1 trace payload under pipeline-key schema version 1.
Each canonical key is exactly 57 little-endian 32-bit words (228 bytes);
records are bytewise sorted, unique, and capped at 128 entries to match the
unchanged draw-pipeline cache limit. Encoding and decoding use fixed storage.
Capture is disabled by default and records only successful draw states when
`BW_METAL_PIPELINE_TRACE_OUTPUT` names a bounded absolute `.bwmpa` path.
Frame-boundary writes use an owner-only temporary file, payload digest,
`fsync`, and atomic replacement inside an owner-controlled directory.

`BW_METAL_PIPELINE_WARMUP` validates the PERF-029 envelope and identity,
reads at most the fixed trace bound, verifies ordering and key semantics before
creating any state, then preloads every state before the first draw. A missing,
stale, corrupt, over-limit, malformed, semantically invalid, allocation-failed,
or Metal-failed artifact emits one bounded aggregate
`bloodwake.metal-pipeline-warmup/v1` status and retains synchronous runtime
creation. Partial preload is cleared on failure. Reports contain only statuses
and aggregate counts, never paths, keys, descriptors, combiner words, function
constants, texture identities, or addresses. The current 128-entry whole-cache
policy is unchanged.

Four deterministic payload cases, seven artifact-envelope/I/O cases, two
direct backend switch cases, and one multi-process capture/load/rejection
integration case pass (14/14) normally and under ASan/UBSan with leak detection
disabled. The integration case covers compatible preload hits, missing,
stale, corrupt, semantically invalid, forced allocation failure, owner-policy
write failure, bounded 128-entry capture, stable JSON shape, and prohibited
field absence. The macOS diagnostic runner suite passes 61/61, strict C and
Objective-C warning builds pass, the complete Metal backend suite passes
362/362, and the production target rebuilds with real offline shader
libraries.

A private no-controller production smoke pair exercised the runner path only.
The first bounded run wrote a private mode-0600, single-link artifact with 15
entries, zero drops, two successful atomic updates, and no write failure. The
second run validated and preloaded all 15 states; its lifecycle aggregate
contained the four fixed startup pipelines plus 15 preload creations and 14
startup specializations, followed by 1,073,994 draw-cache hits and zero misses.
The title remained paused behind its reconnect-controller overlay, so this is
capture/preload mechanics evidence, not representative gameplay, cold-maxima,
or performance evidence. All artifacts and key contents remain private and
uncommitted.

PERF-031 deliberately preloads validated canonical keys and does not yet create
an opaque Metal binary archive. PERF-033 cold qualification can determine
whether startup preload time warrants the separately contract-defined kind-2
archive; it must not be added speculatively or shipped from retail-derived
data.

#### PERF-032 implementation evidence

A fully compatible `BW_METAL_PIPELINE_WARMUP` artifact is the existing opt-in
boundary for asynchronous rare misses. An absent or rejected artifact leaves
the original synchronous path unchanged. After a compatible preload,
an untraced specialized key immediately creates or reuses the existing generic
extended pipeline. That generic shader evaluates the same dynamic texture,
stage, dot, combiner, final-combiner, and alpha routing that the specialized
function constants freeze, so rendering remains exact while Metal executes
`newFunctionWithName:constantValues:completionHandler:` followed by
asynchronous render-pipeline creation.

Pending, ready, and failed entries share one fixed 32-entry bound and coalesce
by the unchanged canonical key. Metal callbacks hold their descriptor and
result but never touch the renderer-owned dictionary. A lock-protected handoff
lets the serial renderer install ready states at draw or flip boundaries.
Successful entries leave the handoff queue; failed entries remain retained to
prevent silent retry. Stop waits on a dispatch group, drains completion state,
then releases the shader library and device. The measured 128-entry draw cache
and its whole-cache reset rule are unchanged because PERF-028 reached only 25
entries and zero resets.

Failures do not become missing or stale success: the exact generic state
continues rendering, a bounded human-readable warning identifies allocation,
function, or pipeline failure, and
`bloodwake.metal-pipeline-async/v1` reports aggregate occupancy, scheduled and
coalesced misses, generic fallback draws, installations, stage-specific
failures, capacity fallbacks, and separate specialization/pipeline timing.
Output is capped at 16 summaries and contains no path, key/hash, descriptor,
combiner word, function constant, texture identity, address, or other
retail-derived field. With lifecycle timing enabled, asynchronous
specialization and pipeline durations also retain canonical-key hashes and
frame/role classification under the existing privacy contract.

Ten focused scenarios cover default-disabled behavior, exact generic output,
successful asynchronous completion and later specialized reuse, duplicate
coalescing, allocation/function/pipeline failure, the 32-entry bound and
unchanged 128-entry cache policy, stable machine output and prohibited-field
absence, stop with outstanding work, and restart isolation. The two focused
CTest selectors pass 2/2 normally, 2/2 under ASan/UBSan, and 2/2 under
ThreadSanitizer; the integration selector executes all artifact and async
subcases against real Metal APIs. The complete Metal backend suite passes
363/363. Strict Objective-C warnings pass and the production offline-library
target rebuilds. The changed strict-C test translation unit compiles cleanly;
the aggregate strict-C target then stops on a pre-existing `-Wcomment` error
in the pinned toolkit's `kernel.h`.

These are implementation and real-API mechanics results, not cold gameplay
qualification. No physical controller is available, so the title remains
paused behind its reconnect overlay; no paused-run maximum or trace coverage
advances PERF-033.

#### PERF-034 implementation evidence

`BW_METAL_FRAME_TIMING=1` now also enables fixed-capacity
`bloodwake.metal-presentation-timing/v1` JSONL. No new environment contract is
needed. The disabled path allocates no presentation state. An enabled run
retains at most 512 records and streams each completed record once, so a
bounded diagnostic timeout does not depend on backend stop. Stop waits at
most one second for owned command-completion and presented callbacks, emits
any remaining records and one summary, and reports unresolved callbacks
explicitly.

The executor records flip stall immediately before backend presentation and
flip release when the existing host-paced hold releases, including the
immediate/no-hold case. The Metal backend correlates those milestones with
drawable acquisition, present request, command commit/completion, and the
actual-presentation callback on `xbox_HostNowNs()`. A valid
`MTLDrawable.presentedTime` is calibrated onto that monotonic clock at callback
time. Exact consecutive intervals remain integer nanoseconds, including
fractional and variable cadence; no display rate is rounded or inferred.

The explicit `presentDrawable:` API does not expose a display target
timestamp. It therefore reports
`display_target_status:"unavailable_explicit_presenter"` and a null target.
Zero, non-finite, unavailable, or backward actual times retain distinct
explicit status instead of a success-shaped value. Output contains only
sequence/frame, classifications, status, timestamps, intervals, durations,
and aggregate counters. It contains no descriptor, combiner value, function
constant, key/hash, retail address/state, or texture identity.

Eleven focused direct cases cover default-off behavior, lifecycle
correlation, runtime streaming without stop, delayed host-paced release,
stable private output, exact fractional/variable intervals, unavailable and
invalid timing, the 512-record bound, allocation failure, stop with
outstanding callbacks, and restart isolation. The separate integration case
validates exact JSON field sets, bounds, privacy, and deterministic output.
All 12 selectors pass normally, under ASan/UBSan, and under ThreadSanitizer.
The complete Metal backend suite passes 375/375. Strict Objective-C and
changed-source C warning checks pass; the production offline-library target
rebuilds. The aggregate production build still reports the pre-existing
nested-comment warning in the pinned toolkit `kernel.h`.

#### PERF-034 paused-renderer mechanics evidence

The first private bounded exact-default smoke revealed that stop-only
reporting produced no records when the runner reached its configured limit.
After adding bounded in-run streaming and a focused no-stop regression case,
a second private smoke retained four consecutive records, frames 0 through 3,
before the controller-reconnect overlay became static.

All four records used the direct presenter, acquired a drawable, completed
their Metal command, and invoked the presented callback. Drawable acquisition
ranged from 0.290 to 3.756 ms, command commit-to-completion from 4.368 to
11.469 ms, and observed flip hold from 0.314 to 15.166 ms. There were zero
malformed records, ordering failures, or prohibited fields; the largest JSON
record was 719 bytes.

All four callbacks returned an unavailable actual-presentation timestamp, and
the explicit presenter reported display targets unavailable as designed.
Consequently this run provides no actual-present interval and cannot
attribute the historical wall p95. It validates real drawable, completion,
callback, streaming, and explicit-unavailability mechanics only. The title
was paused without a physical controller, so none of these phase durations
are retained as gameplay pacing or PERF-034 causal evidence.

#### Controller-free workload calibration

The existing diagnostic keyboard bridge presents a connected neutral title
controller without a physical gamepad. Live guided input was allowed
alongside the deterministic poll-epoch script only for script calibration.
The user-observed missing confirmations became six additional spaced A steps
at the one-player script tail. Guided frame dumps and manual commands are
never retained as performance evidence.

The revised script and bridge pass all 11 input-integration tests normally,
under ASan/UBSan, and under ThreadSanitizer. All 61 isolated diagnostic-runner
tests pass. A calibration run reached 72,729 validated marked Battle scopes
with balanced lit/reflection/foam counts and zero rejection or fingerprint
mismatch.

The first validated relaunch and private save from that run were then used
directly with the neutral bridge, no script, no guided input, and no physical
controller. A 90-second trace run reached 273,229 validated Battle scopes,
captured all 25 observed canonical states with zero drops, and emitted no
scripted or guided input event. Its artifact and every log, save, relaunch
record, key, fingerprint, and capture remain private.

#### PERF-033 seeded runtime evidence

Two clean direct seeded boots used the same executable, Native/auto 4x,
match-display pacing, Retail water, accepted quincunx approximation, audio,
private save/relaunch seed, and compatible 25-state artifact. Neither enabled
GPU audit, capture, profiling, guided input, synthetic script input,
render-scale override, or frame-cap override.

Both validated and preloaded all 25 states. Each performed the four fixed
startup pipeline creations plus 25 preload creations and 24 startup
specializations at frame 0. No later specialization or pipeline creation
occurred. Both reported zero cache misses, asynchronous schedules/fallbacks,
whole-cache resets, allocation failures, Metal failures, malformed reports,
or rejected markers. The one library-load event before the first correlated
flip retained an explicitly unavailable wall interval in each boot.

| Metric | Cold boot 1 | Cold boot 2 |
| --- | ---: | ---: |
| Offline library load | 0.082 ms | 0.088 ms |
| Startup specialization | 24 / 191.553 ms | 24 / 192.632 ms |
| Startup pipeline creation | 29 / 328.293 ms | 29 / 328.971 ms |
| Preloaded / runtime misses | 25 / 0 | 25 / 0 |
| Last retained cache hits / high-water / resets | 648,217 / 25 / 0 | 612,626 / 25 / 0 |
| Post-frame-zero specialization/pipeline events | 0 | 0 |
| Retained wall maximum | 227.223 ms, frame 1 | 225.080 ms, frame 1 |
| Validated marked Battle scopes | 281,832 | 258,621 |

The runs did not reproduce the historical 296-330 ms band. No retained cold
maximum contained a library load, specialization, pipeline creation, archive
operation, cache miss, or reset after the first flip. This closes the measured
renderer-thread pipeline-work gate but does not retroactively attribute the
unreproduced historical band.

The selected consecutive qualification windows were:

| Metric | Cold boot 1, frames 1200/1500 | Cold boot 2, frames 1800/2100 |
| --- | ---: | ---: |
| Completed-game FPS | 60.08-60.11 | 59.66-60.17 |
| Wall median | 16.62 ms | 16.60-16.74 ms |
| Wall p95 | 17.43-17.63 ms | 17.32-17.95 ms |
| Combined GPU median | 14.76-14.79 ms | 14.56-14.57 ms |
| Combined GPU p95 | 15.48-15.52 ms | 15.55-15.79 ms |
| Submission-latency p95 | 15.67-15.78 ms | 15.69-15.80 ms |

The FPS, wall median, and GPU budgets pass. Boot 2's earlier windows warmed
from 51.82 FPS / 17.65 ms GPU p95 at frame 900 to the selected pair. Wall p95
still exceeds 17 ms, and submission p95 is 0.17-0.30 ms above the prior
15.50 ms upper baseline. No physical input-latency or output-hash comparison
was collected. PERF-033 therefore remains active rather than claiming that
all steady-state behavior is non-regressed. Existing focused tests continue
to prove explicit synchronous fallback for missing, stale, corrupt, and
unsupported artifacts.

#### PERF-034 representative presentation evidence

The same clean boots each retained the first 512 presentation events. All 512
commands completed. Actual presentation was available for 507 and 508
records; five and four remained explicitly unavailable. Consecutive actual
presentation medians were exactly 16.667 ms. Boot 1's actual p95/max were
16.671/33.334 ms, while boot 2's colder sample was
33.333/50.000 ms. Fractional and variable cadence is therefore retained
without rounding. All 1,024 display-target fields remained explicitly
unavailable under `presentDrawable:`.

After excluding each frame-zero cold maximum and intervals above 50 ms, 147
and 372 retained wall intervals exceeded 17 ms. None had drawable acquisition
of 5 ms or more. Only one and two had an 8 ms manual hold; seven and 91 had
command completion of at least 16 ms; five and 62 coincided with an actual
presentation interval of at least 25 ms. The remaining 135 and 237 had none
of those measured conditions. Actual-present interval had the strongest
measured correlation with the retained wall interval (0.610 and 0.430), while
command completion was material only in the colder second run.

Thus drawable acquisition and the current manual host hold are not the
general 17 ms tail source. Cold command/GPU completion and missed display
intervals contributed in boot 2, but most residual tails remain consistent
with scene work or OS scheduling that the explicit presenter cannot separate
without a target timestamp.

#### PERF-033 refreshed current-executable evidence

The final qualification refresh used executable SHA-256
`7448d6033ccfe58f0341ce8c4f06e7b41339f20262f5c4ea573deb39a7724c43`,
the original validated direct Battle relaunch, and its byte-identical private
save snapshot. All traces, saves, relaunch records, logs, fingerprints, and
frames remain owner-private and outside the repository.

The first attempt started from the post-run relaunch and reached only 2,354
unscoped markers, so it failed the workload gate and is excluded. The valid
refresh started from the original calibration relaunch and retained the
following evidence:

| Metric | Trace capture | Cold preload 1 | Cold preload 2 | Warm repeat |
| --- | ---: | ---: | ---: | ---: |
| Compatible entries | 25 captured | 25 loaded | 25 loaded | 25 loaded |
| Runtime misses | 25 | 0 | 0 | 0 |
| Cache resets | 0 | 0 | 0 | 0 |
| Post-frame-zero specialization/pipeline events | 24/25 | 0/0 | 0/0 | 0/0 |
| Retained wall maximum | 272.721 ms | 232.569 ms | 239.077 ms | 228.506 ms |
| Maximum frame | 1 | 1 | 1 | 1 |
| Validated marked scopes | 276,381 | 277,215 | 277,713 | 276,051 |
| Event drops / allocation / timing / Metal failure | 0/0/0/0 | 0/0/0/0 | 0/0/0/0 | 0/0/0/0 |

The private trace is an owner-only 5,876-byte, 25-entry artifact. Its capture
reported 706,556 cache hits, 25 misses, a high-water count of 25, and no
whole-cache reset. The two clean preloaded boots reported 670,555 and 708,336
cache hits, zero miss, and high-water 25. Neither scheduled asynchronous work
or used a generic fallback. Cold boot 1 stopped with one marked view in
progress at the bounded process boundary; cold boot 2 was balanced. Both had
zero marker rejection.

The longer warm request still ended at the title's own relaunch rather than
the runner's 300-second bound and retained ten complete rolling windows. Its
final three windows measured 59.95-60.03 FPS, 16.98-17.07 ms wall p95, and
15.39-15.40 ms combined-GPU p95. The final two submission-p95 values were
15.60 and 15.62 ms. This is materially closer to the accepted baseline than
the earlier 15.67-15.80 ms evidence, but it still fails the exact 15.50 ms
PERF-036 gate; the 17.07 ms wall p95 also fails the exact 17.0 ms gate.

Across the original instrumented pair, the earlier preloaded pair, the
refreshed trace, the refreshed preloaded pair, and this warm repeat, no
retained maximum reached 296 ms. The historical 296-330 ms band is therefore
retired as unreproduced for the validated current path. This closes
PERF-028's collection decision without claiming to know which old,
non-reproduced work caused those historical samples.

#### PERF-033 correctness companion

Correctness collection was isolated from performance runs. A current
scripted/guided capture shared four exact private framebuffer hashes with the
earlier executable, including three at the same capture index. Local review
classified all four as mission/UI screens rather than Battle gameplay, so
they are retained as static rendering evidence only.

Two direct neutral-controller Battle companions then used matching private
seeds and GPU audit:

| Metric | Earlier-source executable | Current executable |
| --- | ---: | ---: |
| Private host-timed BMPs | 10 | 10 |
| Validated marked scopes | 264,777 | 270,685 |
| Fingerprinted marked batches | 278,094 | 282,639 |
| Distinct metadata fingerprints | 17 | 18 |
| Fingerprint overflow / within-scope mismatch | 0 / 0 | 0 / 0 |

All 17 earlier metadata fingerprints appeared in the current run; the current
run observed one additional fingerprint. Counts differ because the bounded
runs retained different numbers of frames. No private fingerprint value is
published. These are stable draw-metadata fingerprints, not final
framebuffer hashes.

The 10+10 host-timed Battle captures had zero exact cross-run hash match
because capture time did not select the same simulated frame. A second
bounded comparison used the existing four-frame supported-water capture in
each executable. It completed all four 1280x480 frames in each run with zero
partial sequence, marker rejection, fingerprint overflow, or mismatch, but
the time-evolving title-demo scene again had zero exact cross-run hash match.
Local review confirmed changing animation/simulation phase rather than a
static aligned frame. Neither comparison is converted into an output-hash
pass.

The follow-up indexed capture removes host sampling time as a variable.
`--indexed-frame-capture` privately sets the exact-value
`BW_INDEXED_FRAME_CAPTURE=1` diagnostic for one boot. The executor publishes
and writes only guest flips 300, 550, 800, and 1050, then emits four bounded
`bloodwake.indexed-frame-capture/v1` events and one summary. Snapshot,
unsupported-backend, publication, and file-write failures are explicit.
Malformed, missing, extra, failed, symlinked, inconsistent, or incomplete
records/files fail the runner. Guided/water capture and relaunch chaining are
rejected because they share output ownership or change the boot identity.
Disabled runs perform no capture readback or output.

Four focused cases cover the environment allowlist/default-off contract,
fixed bounds/schema/privacy/failure handling, and CLI propagation. The
complete diagnostic-runner suite passes 66/66, the synthetic-input production
target rebuilds, and a real default-off direct seeded boot produces no indexed
record or file. An initial 600/1200/1800/2400 calibration failed closed after
the cell requested relaunch at about 1,136 flips; the retained fixed set is
fully inside the measured cell.

The pre-PERF-037 source revision and current source revision were rebuilt with
the same diagnostic-only capture patch. Both direct seeded runs completed all
four 1280x480 captures:

| Comparison run | Validated marked scopes | Rejected markers | Indexed captures |
| --- | ---: | ---: | ---: |
| Earlier-source executable | 275,406 | 0 | 4/4 |
| Current executable, run 1 | 271,974 | 0 | 4/4 |
| Current executable, repeat | 278,928 | 0 | 4/4 |

Earlier-source versus current produced zero same-index or cross-index exact
hash match. Crucially, the two runs of the identical current executable also
produced zero same-index and zero cross-index exact hash match. Local private
review showed materially different camera, boat, and simulation state in the
same Retail Battle scene at the same guest flip indexes. The direct seed is
therefore nondeterministic across boots; its exact framebuffer gate is
`unavailable_nondeterministic_seed`, not a renderer mismatch or pass. No frame
or frame hash is published.

No physical controller was available. Guided-command timestamps and the
neutral diagnostic bridge do not measure physical input-to-present latency,
so input latency remains `unavailable_no_device`, not an inferred pass.
Together, the exact static matches, complete fingerprint-set overlap, and
zero marker/Metal error support rendering continuity, but the strict dynamic
output-hash and physical-latency gates remain incomplete.

#### PERF-034/PERF-035 pacing decision and PERF-036 result

The refreshed current-executable cold pair each retained 512 bounded
presentation records. All 512 commands completed in both runs. Each retained
509 actual presentation timestamps and three explicit actual-time
unavailability records. Consecutive actual-present intervals measured:

| Metric | Cold preload 1 | Cold preload 2 |
| --- | ---: | ---: |
| Median | 16.667 ms | 16.667 ms |
| p95 | 16.672 ms | 16.667 ms |
| Maximum | 50.000 ms | 33.334 ms |
| Display targets available | 0/512 | 0/512 |

Cold preload 1 contained six drawable acquisitions at or above 5 ms, while
cold preload 2 contained none. Both retained variable actual-present cadence.
The earlier correlated pair already showed that drawable acquisition and the
manual host hold were not the general wall-tail source; current evidence does
not reverse that conclusion. Every direct-present record still reports
`unavailable_explicit_presenter` for the display target.

PERF-034 is therefore complete as bounded instrumentation and an explicit
decision record, not as invented target-time data. PERF-035 is a no-go:
current evidence does not show manual pacing as a material limiter, so an
availability-gated `CAMetalDisplayLink` path is not implemented. The explicit
presenter remains unchanged.

PERF-036 also records a no-go acceptance result. FPS and combined-GPU p95
pass in the recovered warm windows, but wall p95 reaches 17.07 ms,
submission-latency p95 remains 15.60-15.62 ms, and physical input latency is
unavailable. The current presenter is retained; no default, rendering,
submission-ownership, in-flight-depth, quincunx, or fallback behavior changes.

This follows Apple's current
[Metal 4 compilation guidance](https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api),
[pipeline-state creation APIs](https://developer.apple.com/documentation/metal/pipeline-state-creation),
and
[binary-archive workflow](https://developer.apple.com/documentation/metal/creating-binary-archives-from-device-built-pipeline-state-objects).
The Metal 4 path is optional and availability-gated; the artifact contract
must retain a supported fallback.

### 2. Presentation pacing

GPU p95 is under budget while wall p95 sometimes exceeds one 59.94 Hz display
interval. That does not prove that the manual host clock is responsible.
Before changing the default, record flip-release, command completion,
drawable-wait, requested-present, and actual-present timestamps on one
correlated time base.

If pacing is material, evaluate a gated `CAMetalDisplayLink` path for
match-display/vsync operation, driven by target presentation timestamps and
the drawable supplied by the display link. Keep the existing bounded
semaphore/explicit-drawable path for vsync-off, uncapped, unsupported, or
failed initialization. Drain owned submissions before switching modes.
Apple documents the timing model in
[CAMetalDisplayLink](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink)
and its
[Game Porting Toolkit frame-pacing guidance](https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/presenting-metal-drawables/references/frame-pacing.md).

### 3. Broader qualification

PERF-037 freezes the following matrix before any broader pass/fail claim.
This is an implementation and collection contract, not PERF-038 runtime
evidence. Public workload identifiers describe only a load class. Saves,
relaunch records, traces, archives, logs, frames, and any retail-derived
contents remain private.

#### Private workload slots

| Workload ID | Mode | Required coverage | Initial state |
| --- | --- | --- | --- |
| `BWQ-00-battle-control-1p` | One-player Battle | Existing representative control, geometry, ocean markers, reflection | Current private seed is available |
| `BWQ-01-calm-reflection-1p` | One-player Battle | Calm water, reflection, open-water traversal | Seed capture required |
| `BWQ-02-shore-geometry-1p` | Mission A | Dense shore, geometry, reflection, near/far transitions | Seed capture required |
| `BWQ-03-storm-particles-1p` | Mission B | Storm water, weather, particles, combat effects | Seed capture required |
| `BWQ-04-transition-1p` | Mission C | Loading and gameplay on both sides of one deterministic transition | Seed capture required |
| `BWQ-05-split-effects-2p` | Two-player Battle | Split-screen, effects, geometry, water, two-view reflection | Seed capture required |

Mission A, B, and C must be distinct missions; a single scene cannot fill
multiple mission slots. A private seed is ready only when its owner-only save
directory and same-title relaunch record pass the existing bounds and
validation, two direct neutral-controller boots reach the named load without
scripted/guided input, expected marker/view roles are nonzero and balanced,
and rejection or fingerprint mismatch remains zero. Calibration may use
guided input, scripts, audit, or frame dumps, but no such run is qualification
evidence. Each workload gets a compatible device-local pipeline trace and
preload artifact before timing; those artifacts remain private and
executable/device specific.

#### Per-workload run set

Every workload/device cell uses the same executable and accepted
Native/auto defaults: internal 4x selection, match-display pacing, Retail
water, accepted quincunx approximation, audio enabled, and no render-scale or
frame-cap override. Clean timing runs enable only bounded frame, pipeline, and
presentation timing. They do not enable GPU audit, capture, profiling,
guided input, or a synthetic script.

| Run ID | Duration | Purpose |
| --- | ---: | --- |
| `C1`, `C2` | 300 seconds each | Two fresh process-cold boots in new private output directories |
| `W1` | 300 seconds | Immediate same-seed warm repeat after `C2` |
| `S30` | 1,800 seconds | Sustained run for every workload/device cell |
| `S60` | 3,600 seconds | Sustained run for the worst valid `S30` workload on each device |
| `T30`/`T60` | Match paired sustained run | Approved privileged telemetry companion; never substitutes for clean timing |

The `S60` workload is selected mechanically by highest combined-GPU p95,
then wall p95, then workload ID, so it cannot be cherry-picked. A new
executable, OS build, device class, display mode, pipeline artifact, or
product default invalidates reuse of an earlier cell. Process-cold means a
new guest process and private output, not a claim that opaque OS/driver
caches were cleared.

Frame 0 through 899 is the cold/warm-up band. Steady state begins at frame
900 and retains every consecutive 300-frame report. `BWQ-04` additionally
labels pre-transition, loading, transition, and post-transition intervals;
those intervals are never averaged into steady state. Sustained reports are
also grouped into first, middle, and last ten-minute bands to expose drift.
No frame, window, or run may be dropped merely because it is an outlier.

#### Device axis

Inventory every approved physical Apple Silicon machine by SoC generation,
tier (`base`, `Pro`, `Max`, or `Ultra` as applicable), model identifier,
unified-memory size, OS build, Metal device name, and active display cadence.
Run every physical generation/tier combination that is actually available;
do not infer results for an untested sibling, memory size, or GPU-core count.
The checked-in record must exclude serial numbers, hardware UUIDs, host/user
names, private paths, and raw `system_profiler` output.

Use AC power, Low Power Mode off, a fixed display arrangement, no unrelated
foreground workload, and the same pre-run idle policy. Record power source,
display mode, and pre/post `pmset -g therm` status. An unavailable class is
`unavailable_no_device`, not a pass.

#### Retained record

Each cell retains:

- workload/run IDs, executable SHA-256, revision, edition, exact controls,
  approximation flags, artifact compatibility/status, device class, memory,
  OS build, display cadence, and start/end time;
- completed-game FPS plus wall, combined-GPU, command, and submission
  median/p95/p99/max for every 300-frame window;
- maximum stall, correlated pipeline state, cache high-water/resets,
  asynchronous scheduling/fallback, the fixed bounded presentation sample,
  drawable/command status, and explicit unavailable timing;
- marker, view-role, fingerprint, Metal-error, allocation/timing-error,
  frame-order, input-latency, and output-hash evidence appropriate to the
  workload; and
- on the paired telemetry run, approved package/CPU/GPU power, temperature,
  CPU/GPU frequency or limiting, and resident-memory/pressure samples.

The timing reporter now emits nearest-rank p99 as well as median, p95, and
maximum. The private runner accepts a bounded one-hour single boot, and each
fixed timing channel retains 219,600 samples, enough for that exact-default
hour through 61 Hz. `S30` and `S60` therefore do not require relaunch
chaining or unbounded storage. If privileged telemetry is approved, sample
`cpu_power,gpu_power,thermal` with `powermetrics` in a separate paired run and
retain its machine-readable plist privately. Apple's tool warns that
estimated power is not valid for cross-device comparisons, so use it only for
within-device trends. If a sampler or privilege is unavailable, record
`unavailable_privilege` or `unavailable_sampler`; do not replace direct
evidence with an estimate.

Detailed presentation records remain bounded to the first 512 events. They
qualify cold/startup drawable and command behavior but are not evidence for a
later sustained tail. If a retained long-run maximum requires presentation
causality, report that phase as unavailable and return to the still-active
PERF-034 sampling decision; do not infer a cause from the startup sample.

#### Qualification rule

Correctness gates are zero command/Metal failure, dropped drawable,
marker rejection, fingerprint mismatch, malformed report, unexpected
approximation, and output-hash or frame-order difference. A prewarmed run
must have zero cache miss, asynchronous fallback, cache reset, or
specialization/pipeline creation after frame 0. Physical input-latency
companions remain required where neutral-controller runs cannot exercise an
input.

Every steady 300-frame window must retain 59.0-60.5 FPS, combined-GPU p95 at
or below 16.67 ms, submission-latency p95 at or below 15.50 ms, and wall p95
at or below 17.0 ms. Median, p99, and maximum remain mandatory review fields
even when those gates pass. The sustained first/middle/last bands must show
no thermal/performance warning, sustained-clock collapse, or monotonic memory
growth. Any miss remains a named PERF-038 failure and activates CPU or GPU
attribution only after the measured limiter is known.

#### PERF-037 implementation validation

- The 63 isolated diagnostic-runner tests pass, including exact acceptance of
  3,600 seconds and rejection of 3,601 seconds.
- The focused percentile-output and presentation-timing integration tests
  pass 2/2. The controlled 1-100 ms sample proves nearest-rank
  median/p95/p99/max output.
- The complete Metal backend suite passes 376/376.
- The synthetic-input production target rebuilds successfully with offline
  Metal libraries.

No ownership, callback, or concurrent reporting state changed, so no new
sanitizer-specific ownership gate was activated. No 30- or 60-minute
workload/device cell has been run; all such results remain PERF-038 runtime
evidence rather than implementation evidence.

### 4. Conditional CPU and GPU headroom

Only a broader-matrix miss activates deeper renderer work:

- For a CPU-bound miss, attribute fixed-function transform, vertex fetch,
  vertex-program interpretation, draw preparation, and submission. Port the
  existing `draw_vsh` contract to Metal only if vertex-program interpretation
  is material, retaining exact CPU comparison, bounded compilation, and CPU
  fallback for unsupported programs.
- For a GPU-bound miss, use Metal/Xcode pass counters to distinguish vertex,
  fragment, texture-filtering, bandwidth, depth/stencil, feedback, copy, and
  presentation limits before changing a pass.

The rejected lower-read quincunx variants, unbounded submissions, quality
reductions, and unevidenced MSAA are not continuation candidates.

## Private evidence index

The following local artifact directory names identify the retained evidence.
They live outside the repository and must remain uncommitted:

- `runtime-base-battle-matrix-1x-001`
- `runtime-base-battle-matrix-225x-001`
- `runtime-base-battle-profile-4x-003`
- `runtime-base-battle-gpu-timing-4x-001`
- `runtime-base-battle-gpu-duration-1x-001`
- `runtime-base-battle-gpu-duration-225x-001`
- `runtime-base-battle-gpu-duration-4x-001`
- `runtime-base-battle-surface-cache-1x-001`
- `runtime-base-battle-surface-cache-225x-001`
- `runtime-base-battle-surface-cache-4x-002`
- `runtime-base-battle-surface-cache-final-1x-001`
- `runtime-base-specialized-combiner-4x-001`
- `runtime-base-specialized-combiner-4x-002`
- `runtime-base-specialized-default-auto-001`
- `runtime-base-specialized-matrix-1x-001`
- `runtime-base-specialized-matrix-1x-002`
- `runtime-base-specialized-matrix-225x-001`
- `runtime-base-specialized-matrix-225x-002`
- `runtime-native-4x60-feedback-instrument-001`
- `runtime-native-4x60-gpu-feedback-001`
- `runtime-native-4x60-quincunx16-001`
- `runtime-native-4x60-quincunx24-001`
- `runtime-native-4x60-inflight3-001`
- `runtime-native-4x60-prefilter-experiment-001`
- `runtime-native-4x60-consolidated-001`
- `runtime-native-4x60-consolidated-002`
- `runtime-native-4x60-consolidated-fixed4-001`
- `runtime-native-4x60-consolidated-fixed4-002`
- `runtime-perf034-paused-002`
- `runtime-perf033-seeded-trace-001`
- `runtime-perf033-seeded-cold-001`
- `runtime-perf033-seeded-cold-002`

## Reproduction template

Use a new private output directory for each run:

```sh
python3 tools/macos_diagnostic.py \
  --exe build/bloodwake-native-synthetic-input/bloodwake \
  --prepared-tree game/prepare \
  --toolkit build/xboxrecomp \
  --output-dir /private/new-output \
  --seconds 60 \
  --edition remastered \
  --quincunx-approx \
  --frame-timing \
  --render-scale 2.25 \
  --frame-cap 120 \
  --experimental-keyboard-hook \
  --save-seed /private/save-seed \
  --launch-seed /private/relaunch.bin
```

Do not add GPU audit, BMP capture, menu automation, or profiling to a clean
timing run. Run profiling separately.

For the controller-free PERF-033 exact-default path, omit the scale/cap
overrides, use `--seconds 90 --pipeline-timing`, and add
`--pipeline-warmup /private/compatible-trace.bwmpa`. The experimental keyboard
hook supplies only the connected neutral controller; do not add a synthetic
script or guided input when starting directly from the validated Battle
relaunch seed.

## Conclusions

1. The low 4x frame rate is real.
2. Bounded ownership safely removes routine flip and target-switch
   publication without weakening explicit CPU-visible boundaries.
3. Generic per-fragment routing, not resolution alone, was the largest
   remaining 4x GPU cost. Metal function constants preserve the same Retail
   equations while removing that dynamic overhead.
4. Native/auto 4x now exceeds the 30 FPS acceptance target in every measured
   300-frame Battle window, with a wall median below 33.3 ms.
5. Current Native/auto, match-display, filtering, LOD, and Retail-water
   defaults are retained; no quality reduction is counted as a speedup.
6. Exact GPU-private quincunx feedback removes the per-frame guest round trip,
   while CPU-write detection retains the publication/merge/reseed fallback.
7. Three bounded submissions and multi-target command consolidation reduce
   the steady-state command shape from three commands to one and leave
   1.4-1.8 ms of p95 GPU headroom under the 16.67 ms budget.
8. Native/auto 4x is display-cap limited at 59.98-60.01 FPS after the explicit
   cold-start interval. Fixed-4x/cap-120 runs independently clear sustained
   60 FPS without changing quality.
9. Exact lower-read quincunx rewrites are measured no-go variants; the
   original 36-read loop remains both exact and faster on the representative
   Apple M5 workload.
10. Cold pipeline/workload warm-up and broader mission pacing remain open;
    the 296-330 ms startup maxima are recorded rather than hidden.
11. Direct package-power and temperature telemetry remains the open efficiency
   evidence item.
12. Pipeline attribution and a private, versioned warm-up/archive contract are
   the first follow-up; presentation pacing, broader qualification, and
   conditional vertex/GPU work follow in that order.

Related tracked documentation:

- [Validation evidence](docs/VALIDATION.md)
- [Metal quality and policy](docs/METAL_QUALITY.md)
- [Build and diagnostic workflow](docs/BUILDING.md)
- [Native feature roadmap](docs/NATIVE_FEATURE_ROADMAP.md)
