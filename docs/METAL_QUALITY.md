# Metal quality settings

## Adjustable LOD

The native Apple Silicon build uses the same Remastered draw-distance hook as
upstream. Original remains fixed at the console's 1x behavior. Remastered can
select 1x through 8x, and the active value is now an atomic runtime setting:
the visibility hook can read a new value without rebuilding game state or
mutating the edition registry.

## Anisotropic filtering

Metal now prebuilds immutable 1x, 2x, 4x, 8x, and 16x sampler variants for the
existing address/filter/mip combinations. The selected anisotropy changes at
an executor flip boundary. It is applied only when a texture has multiple mip
levels and uses linear minification. Point-filtered and unmipped textures,
including UI paths, remain 1x.

Invalid `BW_VFX_AF` values stop Metal startup with an explicit error. Original
ignores the Remastered setting and stays at 1x. The settings UI exposes the
existing Off, 4x, 8x, and 16x choices.

## Current base-performance envelope

The bounded ownership path keeps at most three Metal submissions in flight
and assigns mutable upload, feedback-resolve, and readback resources to their
submission. One submission can own multiple ordered color-surface
generations; eight target switches force a fallback commit. Exact tracked
XRGB anti-aliased feedback stays GPU-private while unchanged, with the
publication/32-bit CPU-write merge/reseed path retained for changed guest
memory and other CPU-visible consumers. Immutable NV2A texture-stage,
combiner, final-combiner, and alpha-test routing remains specialized through
Metal function constants without changing dynamic materials or quality.

The representative seeded Battle qualification produced:

| Mode | Completed-game FPS | Wall median | Combined GPU median | Combined GPU p95 |
| --- | --- | --- | --- | --- |
| Native/auto 4x, match display, two boots | 59.98-60.01 | 16.66-16.68 ms | 14.18-14.84 ms | 14.87-15.26 ms |
| Fixed 4x, cap 120, boot 1 (all windows) | 59.36-100.78 | 9.82-16.59 ms | 9.47-13.25 ms | 14.67-20.46 ms |
| Fixed 4x, cap 120, boot 2 (all windows) | 60.33-100.10 | 10.01-16.01 ms | 9.38-12.65 ms | 14.85-20.74 ms |

The exact-default row uses consecutive rolling reports emitted at or after
presented frame 900 and before the guest relaunch transition. Cold windows
remain recorded:
they include 296-330 ms shader/workload warm-up maxima and average as low as
53.22 FPS. Current Native/auto, LOD, filtering, quincunx approximation, draw
coverage, and Retail-water defaults remain unchanged. Sustained 4x/60 is
claimed for the representative workload; cold pipeline warm-up and broader
mission pacing remain open.

The GPU metric includes direct presentation and is not directly comparable to
the older render-only command metric. Fixed-cap p95 ranges include later,
lighter scene phases whose GPU-duration windows overlap command-role changes;
the exact-default qualification is the stable default-policy evidence.
The final code passes 337/337 Metal backend tests, seven focused
ownership/publication cases under ASan/UBSan, and 62/62 macOS presenter tests.
Direct package-power and temperature evidence remains blocked on approved
privileged instrumentation.

### Historical synchronized baseline

A clean, seeded one-player Battle matrix disabled GPU audit and screenshot
capture while holding render scale fixed. Lightweight Metal timing produced
these representative rolling 300-frame windows:

| Fixed scale | Internal target | Completed-game FPS | GPU command median | Command-wait median | Wall-frame median |
| --- | --- | --- | --- | --- | --- |
| 1x | 640x480 | 30.53-31.89 | 17.16-17.23 ms | 17.77-17.90 ms | 30.48-30.72 ms |
| 2.25x | 1440x1080 | 28.37-29.60 | 20.13-20.35 ms | 20.76-20.95 ms | 33.38-33.65 ms |
| 4x | 2560x1920 | 15.76 | 48.25 ms | 48.84 ms | 62.03 ms |

Native sampling attributes 87.3% of the guest thread to `bw_flush_wait` and
79.3% of the NV2A/Metal worker to `finish_command` waiting for Metal command
completion. This explains why 1x and 2.25x wall time remains much higher than
GPU duration: every flip then serialized CPU and GPU work while
synchronizing the private render surface with guest memory.

The 4x command buffer already consumes a 48.25 ms median on the GPU. Perfect
CPU/GPU overlap therefore could not make that 4x path reach 60 FPS;
the GPU-only ceiling is about 20.7 FPS. The corresponding measured ceilings
are about 49 FPS at 2.25x and 58 FPS at 1x before further GPU optimization.
This motivated the bounded asynchronous ownership model
for vertex uploads, readback resources, and cached render surfaces. Simply
removing `waitUntilCompleted` would reuse live resources and violate
render-target, guest-memory, snapshot, and shutdown correctness.

The first ownership-safe optimization is now implemented. Cached color
targets retain exact guest-resolution synchronization snapshots, so an
unchanged target can reuse its private Metal texture without another
guest-to-private upload and scale pass. Any byte change forces the established
seed path. The 333-case Metal suite covers unchanged reactivation, CPU
mutation, subsequent reuse, target switching, and the existing CPU-write
counterexample.

Clean seeded runs recorded 599 target reuses and one present-time guest reseed
in their first 300 frames at every measured scale. The first 4x window improved
from the earlier 48.25 ms GPU / 62.03 ms wall medians to 43.38 ms / 56.75 ms;
the first 1x window measured 16.76 ms / 30.02 ms. Sustained windows still
reached 50.27 ms / 65.24 ms at 4x and 22.40 ms / 36.77 ms at 2.25x. Because
scene progression, process ordering, power, and temperature were not held
constant, these repeats establish that the cache path is active and
correct—not a precise performance percentage. CPU/GPU serialization remains
the dominant next boundary.

The matrix does not change product defaults. Native scale, a 1080p-class
default, adaptive internal resolution, and a 60 FPS cap remain explicit
policy choices to evaluate after the synchronization work is measured. A
separate sustained 4x run slowed from about 14.8 to 12.7 FPS, but scene
progression was not fixed and no direct thermal or package-power telemetry was
collected, so that observation is not attributed to thermal throttling.

## Optional realistic water

The authoritative system and shader research is in
[WATER_RENDERING.md](WATER_RENDERING.md). In particular, the current marker
contract identifies lit/base, reflection, and foam passes, but the optional
shader still covers only lit/base. The later Retail dot-product
cube-reflection and wake-foam passes remain unchanged.

`graphics.water_rendering` is a Remastered-only macOS Metal frame-boundary
setting with `retail` and `realistic` choices. Retail is the default and keeps
the existing fragment entry and output path. `BW_METAL_WATER` strictly accepts
the same two values; invalid input fails startup, and Realistic is rejected in
Original.

Realistic uses a distinct pipeline key and fragment entry. Selection requires
the explicit in-stream ocean marker, a valid stage-0 water sample, and the
validated passthrough normal/view basis. Every unmarked draw and every
unsupported marked state uses Retail. Pipeline creation failure is reported,
returns the active/requested setting to Retail, and retries the retail
pipeline instead of dropping the draw.

The shader uses only existing game inputs. It decodes the interpolated water
normal and view basis, computes a restrained Schlick-style Fresnel term,
desaturates the stage-0 sample slightly as a roughness/reflection proxy, and
blends it with the current fog-atmosphere color. The title's actual later cube
reflection pass is not modified. The additional weight is bounded by the
existing fog visibility and Retail alpha is preserved. There are no new
textures, art, depth reads, refraction, depth absorption, foam, or shoreline
effects.

The complete 333-case Metal suite includes exact default-Retail output,
marked-only enhancement, unmarked recovery, unsupported-state fallback,
frame-boundary activation, edition gating, strict startup parsing, and the
diagnostic benchmark state machine. It also covers typed lit, reflection, and
foam propagation, persistent multi-draw and empty pass runs, water-view
inheritance, legacy marker compatibility, and malformed or mismatched
view/event/run rejection. Hook coverage separately verifies title-boundary
event order and Original passthrough. A later one-player Battle run recorded
186,893 scopes for each pass and 2,915 balanced view-0 pairs with zero marker
rejection, fingerprint overflow, or within-scope mismatch. Metal private
reflection depth and the optional shader both activated without fallback.
Split-screen/view-1 evidence remains deferred.

### Controlled water GPU benchmark

`BW_METAL_WATER_BENCHMARK=1` is accepted only by the private diagnostic
runner in Remastered with GPU audit enabled and Retail selected initially. It
waits for a successfully encoded supported marked-ocean frame, then measures
four blocks in Retail/Realistic/Realistic/Retail order. Each block discards 10
supported-water warm-up frames and records 50 supported-water frames, yielding
100 samples per preset. Frames without a supported marked-ocean draw do not
advance the protocol. External preset requests cannot change the active mode
during measurement, and completion restores Retail.

Each sample sums Metal's completed-command-buffer GPU duration for command
buffers containing an encoded supported-ocean draw. It therefore includes
other work sharing those command buffers and is not an isolated fragment
shader timer. Missing timestamps, command failures, state changes, sample
overflow, incomplete runs, malformed summaries, or inconsistent deltas fail
explicitly.

A private four-boot run completed independent ABBA samples in two boots:

| Sample | Retail mean / median / p95 | Realistic mean / median / p95 | Mean / median / p95 delta |
| --- | --- | --- | --- |
| 1 | 51.126104 / 51.121375 / 54.506500 ms | 53.591635 / 52.075000 / 60.007250 ms | +4.822451% / +1.865413% / +10.091916% |
| 2 | 52.924863 / 53.103750 / 56.421125 ms | 55.331963 / 54.999625 / 60.814792 ms | +4.548148% / +3.570134% / +7.787273% |

Across the two completed samples, the Realistic delta ranges were
+4.548%-+4.822% mean, +1.865%-+3.570% median, and +7.787%-+10.092% p95.
No unsupported-pass fallback or Metal runtime error was observed. No
automatic acceptance threshold is inferred from these measurements. Hands-on
review found the current result almost identical to Retail. Separate bounded
Retail and Realistic capture runs completed, but scene/camera phase drift
prevents treating them as pixel-aligned A/B evidence. Representative
same-execution visual coverage and any desired process/driver memory
measurement remain open; the preset is therefore still experimental,
default-off, and excluded from quality presets.

## Metal MSAA decision

The DX11 `BW_VFX_MSAA` setting is not implemented by the Metal backend and is
reported as unsupported rather than silently accepted.

At the currently verified 2560x1920 internal size, one BGRA8 colour target is
18.75 MiB and one Z24S8 depth/stencil target is another 18.75 MiB. Keeping the
authoritative single-sample pair while adding multisample colour and depth has
these lower-bound attachment costs:

| Sample count | Multisample pair added | Single + multisample pairs |
| --- | ---: | ---: |
| 2x | 75 MiB | 112.5 MiB |
| 4x | 150 MiB | 187.5 MiB |
| 8x | 300 MiB | 337.5 MiB |

These figures exclude presentation, seed/readback resources, cached inactive
targets, textures, resolve traffic, and driver allocation overhead. MSAA would
also require sample-count pipeline variants, persistent multisample colour and
depth ownership, resolves before sampling/readback/presentation, and correct
handling of target switches, partial clears, guest-seeded surfaces, resize,
render-scale changes, and quincunx.

The project therefore has a current **no-go** on advertising or implementing
Metal MSAA without a controlled retail A/B run showing a visible edge-quality
benefit beyond native render scaling at an acceptable GPU-time and memory
cost. A future evaluation must record monotonic frame-time percentiles, GPU
duration where available, peak attachment allocation, resolve cost, and fixed
reference crops containing hulls, rigging, shorelines, particles, HUD, and
alpha-tested edges at 1x, 2x, and 4x render scale. If that evidence does not
justify multisampling, native render scale and the validated guest AA path
remain the Metal alternatives.

### Evidence harness

`tools/metal_msaa_evidence.py` is a standalone decision harness. It does not
create multisample resources, alter rendering, or make `BW_VFX_MSAA` supported.
It accepts only the currently documented BGRA8/Z24S8 attachment pair, 2x/4x/8x
sample counts, 1x/2x/4x render-scale labels, and positive Metal texture
dimensions no larger than 16384. All retained-attachment, resolve-target, and
minimum resolve-traffic calculations use checked unsigned 64-bit arithmetic.

Fix the limits before collecting a run, then create a JSON record template.
For example, this records a 4x cell whose attachment limit is exactly the
187.5 MiB lower bound; it is not a project-wide approval policy:

```sh
python3 tools/metal_msaa_evidence.py template \
  --width 2560 --height 1920 --sample-count 4 --render-scale 4 \
  --color-format bgra8unorm \
  --depth-stencil-format depth24unorm_stencil8 \
  --max-attachment-bytes 196608000 \
  --max-gpu-time-regression-percent 10 \
  --max-resolve-gpu-time-ms 1 \
  --min-measured-frames 600 \
  --output build/metal-msaa-4x.json
```

The generated record deliberately contains null evidence and therefore
evaluates to no-go. A controlled A/B collector must fill the shared device,
OS, build, scene, GPU timer, protocol, and run-order fields; warm-up and frame
counts; monotonic baseline/MSAA GPU-time percentiles; resolve p95; measured
peak attachment bytes; every named correctness check; and SHA-256-identified
baseline/MSAA crops for every required visual feature. At least one crop must
show an improvement, all crops must remain correct, and the reviewer must
affirm a visible benefit beyond render scaling.

Evaluate the completed record with:

```sh
python3 tools/metal_msaa_evidence.py evaluate build/metal-msaa-4x.json \
  --output build/metal-msaa-4x-report.json
```

The evaluator exits 0 only for `go`, 1 for a valid `no-go`, and 2 for malformed
or unsafe input. Missing evidence, failed correctness or visual checks,
non-monotonic timing data, an implausible measured peak below the computed
lower bound, or any limit exceedance is no-go. A `go` applies only to that
recorded matrix cell; it neither enables MSAA nor changes the current
unsupported status. Every intended sample-count/render-scale cell and the
renderer ownership cases listed above still require review before any future
implementation decision.

### Device capability probe

The standalone `tests/metal_msaa_capability` target queries
`-[MTLDevice supportsTextureSampleCount:]` for 2x, 4x, and 8x without creating
multisample resources or changing renderer policy:

```sh
cmake -S tests/metal_msaa_capability -B build/test-metal-msaa-capability \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/test-metal-msaa-capability
ctest --test-dir build/test-metal-msaa-capability --output-on-failure
build/test-metal-msaa-capability/bw_metal_msaa_capability
```

Its fixed-shape JSON always reports `bw_vfx_msaa_policy` as `unsupported`.
On the recorded Apple M5 host, Metal reports 2x, 4x, and 8x sample counts as
supported. Device capability is only a prerequisite: it does not satisfy
attachment ownership, resolve correctness, performance, or visual gates.

### Diagnostic MSAA smoke

The standalone `tests/metal_msaa_smoke` target creates small synthetic
multisample color and depth/stencil attachments, a sample-count-matched
pipeline, a single-sample resolve texture, and synchronized readback:

```sh
cmake -S tests/metal_msaa_smoke -B build/test-metal-msaa-smoke \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/test-metal-msaa-smoke
ctest --test-dir build/test-metal-msaa-smoke --output-on-failure
build/test-metal-msaa-smoke/bw_metal_msaa_smoke
```

It validates clear color, accepted/rejected depth writes, stencil rejection,
resolve/readback, explicit command-buffer errors, checked lower-bound resource
bytes, and GPU timing when Metal exposes it. Unsupported sample counts are
reported as skipped, not passed.

The recorded Apple M5 passed 2x, 4x, and 8x. This proves the device/API
mechanics on an 8x8 synthetic target only. It does not prove game-renderer
attachment ownership, target switches, partial clears, guest seed/readback,
resize/scaling, scene correctness, useful visual improvement, or acceptable
retail memory and GPU cost.
