# Apple Silicon performance investigation

Status: tracked performance record
Last updated: 2026-11-06

This document records the base-game frame-rate and efficiency investigation
that temporarily takes priority over later modern-water visual work. It
separates measured behavior from hypotheses and product-policy decisions.
Retail-derived saves, launch records, profiles, logs, frames, and prepared
game data remain private and must not be committed.

## Investigation goals

1. Verify whether the displayed low frame rate is real.
2. Separate diagnostic overhead from normal renderer cost.
3. Measure resolution scaling independently from CPU/GPU serialization.
4. identify the highest-confidence optimization that preserves rendering,
   guest-memory, and gameplay behavior.
5. Avoid silently reducing visual quality or changing frame-rate policy.

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
- rolling 300-frame FPS, median, p95, and maximum reports;
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

Mutable vertex, surface-upload, and surface-readback resources now belong to
one of two bounded submissions. Queue saturation retires only the oldest
submission. Completion, failure, color generation, and guest publication are
processed in order on the executor thread. A CPU change detected while a
private generation is newer is merged at 32-bit pixel granularity before the
surface is reseeded.

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
6. Native 4x/60 remains outside the demonstrated envelope and is not claimed.
7. Direct package-power and temperature telemetry remains the open efficiency
   evidence item.

Related tracked documentation:

- [Validation evidence](docs/VALIDATION.md)
- [Metal quality and policy](docs/METAL_QUALITY.md)
- [Build and diagnostic workflow](docs/BUILDING.md)
- [Native feature roadmap](docs/NATIVE_FEATURE_ROADMAP.md)
