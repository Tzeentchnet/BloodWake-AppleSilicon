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

## Optional realistic water

`graphics.water_rendering` is a Remastered-only macOS Metal frame-boundary
setting with `retail` and `realistic` choices. Retail is the default and keeps
the existing fragment entry and output path. `BW_METAL_WATER` strictly accepts
the same two values; invalid input fails startup, and Realistic is rejected in
Original.

Realistic uses a distinct pipeline key and fragment entry. Selection requires
the explicit in-stream ocean marker, a valid stage-0 water/reflection texture,
and the validated passthrough normal/view basis. Every unmarked draw and every
unsupported marked state uses Retail. Pipeline creation failure is reported,
returns the active/requested setting to Retail, and retries the retail
pipeline instead of dropping the draw.

The shader uses only existing game inputs. It decodes the interpolated water
normal and view basis, computes a restrained Schlick-style Fresnel term,
desaturates the existing reflection slightly as a roughness proxy, and blends
it with the current fog-atmosphere color. The additional weight is bounded by
the existing fog visibility and retail alpha is preserved. There are no new
textures, art, depth reads, refraction, depth absorption, foam, or shoreline
effects.

The complete 320-case Metal suite includes exact default-Retail output,
marked-only enhancement, unmarked recovery, unsupported-state fallback,
frame-boundary activation, edition gating, and strict startup parsing. A
private four-boot scripted Remastered diagnostic activated the shader on real
marked ocean batches with no observed fallback or Metal error. Hands-on A/B
visual review and controlled GPU-time/memory measurements remain required;
the preset is therefore experimental, default-off, and excluded from quality
presets.

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
