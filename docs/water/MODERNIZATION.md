# Modern water implementation plan

Updated 2026-11-05.

This plan describes how to move Blood Wake's Apple Silicon water from the
current experimental pass-1 color treatment toward a modern ocean that can
approach the visual quality associated with Assassin's Creed IV: Black Flag.
Black Flag is a perceptual quality target, not a promise to reproduce its
implementation, assets, or exact appearance.

Implementation progress and dependencies are tracked in
[ROADMAP.md](ROADMAP.md).

Phase 0 and the first pass-aware parity item are complete. The marker protocol
and executor support one-shot draws, persistent pass runs, balanced view
scopes, and legacy-marker compatibility. Validated title wrappers bracket the
complete per-view ocean render plus every lit, reflection, and foam submission
group. A bounded one-player Battle run recorded 186,893 scopes for each pass,
2,915 balanced view-0 pairs, zero marker rejection, and clean fingerprints.
Metal now implements the private-depth distant-water-line fix without writing
authoritative scene depth; focused lifecycle, fallback, sanitizer, and full
333-test Metal coverage pass.

The later Black Flag-style resource, shader, optical-volume, SSR, foam, and
geometry phases remain preserved in this plan but are paused while the
highest-value Native 4x/60 continuation removes cold pipeline stalls,
qualifies presentation pacing and broader workloads/devices, and establishes
the accepted headroom available to each visual phase.

The plan is grounded in:

- [the authoritative water rendering map](../WATER_RENDERING.md);
- [the lighting and renderer modernization map](../LIGHTING_RENDERING.md);
- [the current Metal quality evidence](../METAL_QUALITY.md);
- [the settings contract](../SETTINGS.md);
- [the validation record](../VALIDATION.md); and
- [the current Apple Silicon status](../CURRENT_STATUS.md).

## Executive decision

The current `realistic` shader is not a suitable foundation for a
Black Flag-quality result by itself. It changes only the lit/base ocean pass,
has no scene color or sampled depth, uses no world-space position or explicit
light, leaves the later cube-reflection and wake-foam passes untouched, and
must preserve the Retail alpha and composition. Increasing its Fresnel
strength would mostly produce a stronger tint under the unchanged reflection
pass, not modern water.

The recommended solution is a **semantic hybrid water renderer**:

1. Keep the title's water simulation, disturbances, boat interaction,
   quadtree selection, and gameplay queries unchanged.
2. Explicitly identify all three ocean passes and each view in the title's
   command stream.
3. Add a water-specific semantic contract for camera, world-space geometry,
   normals, lighting, and frame resources.
4. Render a complete modern water result into private Metal resources while
   the exact Retail draws remain available as an atomic fallback.
5. Composite the modern result only after the whole water view succeeds.
6. Add physically motivated reflection, micro-normal detail, absorption,
   refraction, wake/crest/intersection foam, and optional screen-space
   reflection in measured stages.

This route can deliver a large visual improvement without replacing the
game's water mechanics or the entire scene renderer. Exact Black Flag parity
cannot be guaranteed because the surrounding art, water mesh, lighting,
camera, and effects were authored for a 2001 Xbox title. The target becomes
credible only after the new semantic and frame-resource contracts exist.

## Quality target

The modern path should reproduce the visual qualities that make a high-end
ocean convincing rather than copy a particular game's implementation:

| Quality pillar | Target behavior |
| --- | --- |
| Large wave shape | Preserve Blood Wake's swell, shore response, wakes, splashes, and storm silhouette without changing simulation. |
| Surface detail | Add stable, world-anchored multi-scale normal detail that does not swim with the camera or expose tile seams. |
| Reflection | Use view-angle Fresnel, roughness-aware sky reflection, coherent sun/moon glints, and optional nearby-object reflection. |
| Water volume | Use water thickness for deep-water color, shallow-water transmission, and distance-dependent absorption/scattering. |
| Refraction | Distort the opaque scene behind water with depth-aware limits and safe edge fallback. |
| Foam | Preserve the title's authored wake masks, then improve breakup, crest foam, and shoreline/object intersection foam. |
| Atmosphere | Match the title's fog, weather, day/night state, and sky rather than applying a second unrelated haze model. |
| Stability | Avoid temporal shimmer, reflection popping, LOD rings, cracks, and visible transitions between quality tiers. |
| Readability | Keep boats, wakes, hazards, projectiles, shore boundaries, and mission cues at least as readable as Retail. |

The whole scene will still use legacy assets and lighting for some time.
Modern water should therefore be art-directed to fit Blood Wake rather than
become a photorealistic surface that looks disconnected from everything
around it.

## Current foundation and hard constraints

### What is already useful

- The title has a real water simulation with broad swell, shore maps, calm
  zones, ripples, wakes, splashes, boat-line disturbances, and sinking
  disturbances.
- The title evaluates render-only positions, heights, and normals for
  quadtree-selected ocean tiles.
- High-frame-rate interpolation and native ocean evaluation already preserve
  mechanics by operating only on render output and restoring simulation state.
- Pass 1 supplies an encoded water normal and view basis.
- Pass 2 supplies the title's sky cube through verified dot-reflection stages.
- Pass 3 supplies authored ocean-frame/wake-mask alpha.
- Metal already supports private scaled color/depth targets, render-to-texture
  ordering, the Retail combiner path, anisotropy, cube sampling, and direct
  presentation.
- The versioned in-stream ocean marker is a proven model for carrying semantic
  identity through the executor without guessing from shader patterns.
- The Metal color surfaces are already created with render-target and
  shader-read usage, although there is no safe scene-color snapshot contract
  yet.

### What blocks a modern result today

| Missing capability | Why it matters |
| --- | --- |
| Explicit pass-2 and pass-3 identity | The backend cannot safely replace reflection or foam by recognizing a coincidental stage pattern. |
| Water-view begin/end identity | Resource capture and all-or-Retail fallback need a bounded per-view transaction, including split-screen. |
| World/eye-space position | Needed for stable detail, depth reconstruction, lighting, screen-space reflection, and fog consistency. |
| Stable geometric normal contract | The current pass-through value is useful but not a general world-space shading basis. |
| Camera and projection data | Needed to reconstruct positions and linear water thickness from depth. |
| Explicit sun/moon/light data | A guessed highlight will conflict with weather, night scenes, lightning, or mission lighting. |
| Scene color before water | Needed for refraction and atomic modern composition; the active render target cannot be sampled while it is also being written. |
| Shader-readable opaque depth | Needed for thickness, shoreline/intersection cues, depth-aware refraction, and screen-space reflection. |
| Water-surface depth | The title does not write authoritative water depth, so opaque depth alone cannot give water thickness. |
| Linear/HDR working target | Physically motivated reflection and absorption are not reliable when accumulated only in the current `BGRA8Unorm` target. |
| Roughness/prefiltered environment data | The existing sky cube has no modern material contract or roughness mip chain. |
| Motion/history contract | Temporal denoising and temporal water effects are unsafe until per-view history and motion are explicit. |

### Non-negotiable boundaries

- Do not modify wave state, disturbance records, buoyancy, collision, boat
  handling, projectiles, AI, camera water queries, mission logic, or
  simulation timing.
- Keep Original and default Retail rendering exact.
- Select modern rendering only from validated title semantics, never texture
  hashes, addresses, asset names, metadata fingerprints, or stage-program
  patterns alone.
- Keep split-screen views isolated.
- Do not silently drop a draw, silently lower quality, or continue with a
  partially valid modern view.
- Do not use fog as a substitute for water thickness.
- Do not read and write the same color attachment in an unsupported feedback
  loop.
- Do not suppress a Retail pass until the replacement view has passed all
  preflight and fallback requirements.
- Do not describe the path as PBR until color space, light, material,
  reflection, and linear/HDR composition are explicit.
- Do not commit retail-derived textures, captures, hashes, generated title
  code, or extracted assets.

## Why the current shader should not simply be made stronger

The current shader computes the complete Retail pass-1 output, then applies a
small RGB-only Fresnel-shaped blend using the stage-0 sample and fog color.
The later Retail reflection pass can dominate that change, and the Retail
foam pass is unaffected. The controlled benchmark already measured about
4.5%-4.8% mean and 7.8%-10.1% p95 whole-command-buffer overhead in the
exercised scene while hands-on review found almost no visible difference.

That result establishes two planning rules:

1. Spend GPU time only after the complete pass contribution can be measured.
2. Judge each milestone by visible quality gained per unit of GPU time and
   memory, not by whether a new shader branch executed.

## Recommended architecture

### 1. Preserve the title's simulation and tile selection

Continue using the title's:

- water state and disturbance system;
- `Ocean_Render` visibility and quadtree selection;
- `Ocean_FillVertices` render geometry;
- high-frame-rate render interpolation;
- native render-vertex evaluation; and
- split-screen LOD policy.

The first modern releases should re-shade existing geometry. A presentation-
only tessellation step can be evaluated later if captures prove that geometry,
not shading, is the remaining quality limit.

### 2. Replace one ocean marker with an explicit water semantic stream

Extend the versioned marker protocol so the executor receives explicit events
for:

- water view begin;
- lit/base draw;
- sky-reflection draw;
- wake-foam draw; and
- water view end.

Each event must include a validated version and enough identity to distinguish
the split-screen view. Begin/end events must not be inferred from an arbitrary
non-water draw. The existing one-shot scope, conflict rejection, frame-boundary
clear, and unknown-version fallback behavior should remain.

The executor should copy pass identity into a water-specific semantic record.
Do not keep adding guessed meanings to `BwXBatch`; keep the exact NV2A batch
auditable and add a separate, versioned contract for modern water.

### 3. Add a water semantic batch

The exact C layout should be designed after representative instrumentation,
but the contract needs these responsibilities:

```text
BwWaterView
    version and view ID
    viewport/scissor rectangle
    view, projection, inverse-view-projection, and camera position
    near/far or an explicit depth-linearization transform
    title fog color and visibility convention
    validated sun/moon direction, color, and intensity
    weather/time parameters that are proven stable

BwWaterDraw
    explicit pass ID
    world- or eye-space position
    stable geometric normal
    projected position for exact title coverage
    title texture coordinates and pass textures
    Retail blend/depth/fog/alpha state
    pass-specific wake/foam alpha semantics
    first/last or equivalent pass-boundary information
```

The batch remains synchronously borrowed unless the backend explicitly copies
it. Missing or unsupported semantic data makes the entire modern view
ineligible; Retail still renders.

Before fixing the attribute layout, audit the marked draws to prove which raw
vertex inputs and title transforms produce the current projected position and
normal. Do not assume an input register is world position or a constant is a
sun direction merely because it looks plausible.

### 4. Use an atomic shadow-render-and-composite path

The safest first complete modern renderer should not suppress Retail draws.
Instead:

1. At water-view begin, preflight all pipelines and private resources.
2. Copy the opaque scene color for that view before the first water draw.
3. Preserve or linearize the opaque scene depth for shader access.
4. Let every Retail water draw continue into the title target unchanged.
5. In parallel, render the modern water result into private Metal targets
   from the same semantic batches.
6. Track whether every required modern pass completed successfully.
7. At water-view end, overwrite only modern water coverage with the completed
   modern composite.
8. If any modern preparation or draw failed, skip the composite. The already
   rendered Retail result remains exact.

This costs more than immediate pass replacement, but it gives an atomic
per-view fallback even if failure occurs after some water batches have been
seen. Once the complete path is stable and measured, an optimized mode may
suppress redundant Retail pass work only after all resources and pipelines
are guaranteed ready before water-view begin.

### 5. Use explicit private frame resources

At the currently verified 2560x1920 internal resolution, one four-byte
attachment is 18.75 MiB and one `RGBA16Float` attachment is 37.5 MiB. A
high-quality first design should budget approximately 75 MiB for:

| Resource | Suggested format | Purpose | Approximate 2560x1920 size |
| --- | --- | --- | --- |
| Scene color before water | `BGRA8Unorm` copy | Refraction source and exact pre-water background | 18.75 MiB |
| Opaque linear depth | `R32Float` when the existing depth cannot be sampled safely | Thickness, SSR, intersections | 18.75 MiB |
| Water composite plus coverage | `RGBA16Float` | Linear modern result and coverage | 37.5 MiB |

A separate water-depth attachment may reuse the `RGBA16Float` alpha only if
precision and ordering tests prove it safe; otherwise budget another
`R32Float` or depth attachment. Screen-space reflection also needs a
downsampled depth/color hierarchy. Allocate lazily per active size and release
or recreate only at a completed frame boundary.

Resource ownership must explicitly cover:

- active target changes and render-to-texture;
- full and partial clears;
- guest-seeded surfaces and writeback;
- guest AA and private render scaling;
- window resize and scale-epoch changes;
- split-screen view rectangles;
- command-buffer and render-encoder boundaries;
- screenshots and framebuffer callbacks; and
- failure before and after any private encoder has started.

No new path should add a CPU readback or command-buffer wait per water draw.

### 6. Compose water in a linear working space

The modern offscreen path should:

1. Decode the copied scene color using an explicitly tested legacy-to-linear
   transform.
2. Compute reflection, transmission, absorption, scattering, highlights, and
   foam in linear space.
3. Apply the title-compatible fog at a documented point.
4. Encode the final water-covered pixels back to the legacy target convention.
5. Leave the title's final gamma-ramp and presentation ownership unchanged.

This does not require converting the whole game to HDR in the first release.
It creates a bounded linear/HDR island for water composition. A future full
linear scene target should remain part of the broader renderer roadmap rather
than become an accidental dependency of the water feature.

## Rendering model

### Large-scale geometry and normals

- Use the title's evaluated height and normal as the authoritative large-scale
  shape.
- Keep the title's quadtree visibility and tile LOD.
- Treat high-frame-rate interpolation as the only temporal interpolation of
  simulation output.
- Add two or three world-anchored normal octaves for capillary detail. Use
  different direction, frequency, and speed for each octave, with weather-
  dependent strength.
- Keep micro detail normal-only initially. Added vertical displacement can
  create visible separation from boats, wakes, projectiles, and shorelines.

Project-owned procedural noise or runtime-generated textures are preferable
to extracted replacement assets. Any authored replacement-water texture pack
needs a separate asset and licensing policy.

### Reflection

Use a hybrid reflection model:

1. Schlick Fresnel with a water-like base reflectance around 0.02.
2. Roughness-aware sampling of a prefiltered version of the title's sky cube.
3. A validated directional sun/moon GGX highlight.
4. Optional screen-space reflection for nearby boats, structures, and bright
   effects.
5. Sky-cube fallback when screen-space reflection misses, exits the screen, or
   lacks valid depth.

The title's one-level sky cube can be convolved at runtime into a private mip
chain when its source changes. The derivative remains a transient resource
from the user's runtime data and is never committed.

Do not add a guessed fixed sun direction. Extract and validate a title-level
directional-light or sky-light semantic first. Until then, ship environment
reflection without the direct glint.

### Refraction and water volume

For each covered pixel:

1. Reconstruct the opaque position from sampled opaque depth.
2. Compare it with linear water-surface depth.
3. Reject negative, non-finite, cross-view, or out-of-rectangle thickness.
4. Distort the scene-color lookup with the combined large and micro normal.
5. Limit distortion by thickness and screen-edge confidence.
6. Apply Beer-Lambert transmittance using art-directed shallow/deep water
   coefficients.
7. Add low-frequency in-scattering so deep water does not become black.
8. Fade to reflection/fog when no valid refracted sample exists.

Depth color must remain an optional presentation treatment. It must not make
hazards, boats, projectiles, or mission targets harder to read.

### Foam and wakes

Foam has three distinct sources:

- **Authored wake foam:** pass 3 remains authoritative for where boat wakes
  exist and how they age. Preserve its mask and vertex alpha, including the
  distant-mip fix.
- **Crest foam:** derive a conservative visual mask from validated geometric
  slope/curvature and world-anchored breakup noise. Do not write back to the
  disturbance system.
- **Intersection foam:** use water thickness and opaque-depth gradients for a
  narrow shoreline/object contact band after false positives are evaluated.

Shade foam in linear space, keep its edge stable under render scaling, and
retain enough contrast for wake readability. Avoid unbounded additive white
that clips every storm scene.

### Optional screen-space reflection

SSR is the highest-value late quality feature for reflecting boats and nearby
structures, but it should not block the first modern release.

- Build a per-view depth pyramid after opaque depth is available.
- Ray march from the water surface in view space.
- Use roughness-dependent step count and confidence.
- Reject hits outside the view rectangle or behind invalid/cleared depth.
- Fade by edge distance, ray length, and hit confidence.
- Fall back to the prefiltered sky cube.
- Start at half resolution with bilateral upsampling.

Planar reflections would require a second scene render with safe object,
camera, culling, shadow, and split-screen ownership. That is a scene-renderer
project and is not recommended for this water roadmap.

## Phased implementation

### Phase 0 - Evidence, pass isolation, and same-execution comparison

Work:

1. Add aggregate diagnostics for water pass order, draw counts, state families,
   target/depth identity, viewport, and split-screen view boundaries.
2. Validate title boundaries that can emit explicit lit, reflection, foam,
   view-begin, and view-end markers.
3. Isolate each pass with the existing developer toggles across calm/storm,
   day/night, fog, shore, wake-heavy, reflection-heavy, and split-screen
   scenes.
4. Replace separate-process visual comparison with a same-execution path.
   Prefer dual private targets fed by the same frame's batches so both images
   use identical simulation, camera, weather, and tile selection.
5. Add GPU counter sampling around water work where supported. Keep the
   existing whole-command-buffer benchmark, but do not use it as isolated
   shader timing.

Exit gate:

- All three passes and each water view have validated semantic boundaries.
- Same-execution Retail/experimental pairs are pixel aligned.
- Representative pass contribution is understood well enough to select the
  first visual milestone.
- Unknown or conflicting identity remains Retail and reports a diagnostic.

### Phase 1 - Pass-aware parity foundation

Work:

1. Introduce the versioned pass/view marker protocol and executor tests.
2. Carry explicit water pass identity separately from metadata fingerprints.
3. Port `OCEAN_REFLECT_SINGLE` to Metal using a private depth target, matching
   the DX11 behavior without modifying authoritative scene depth.
4. Add pass-2-only experimental shaping using the title's actual cube result,
   normal, and view basis.
5. Keep the current pass-1 treatment available for comparison, but do not tune
   it as the final solution.
6. Measure which water textures actually have linear mipmapped sampling before
   expecting anisotropy to help.

Exit gate:

- Metal closes the distant-water-line parity gap.
- Lit, reflection, and foam paths each have exact Retail fallback.
- Pass isolation proves which enhancement is visible and which is overwritten
  by later composition.

Expected quality:

- Cleaner distant reflection and a potentially more visible environment
  response, but still below the Black Flag target.

### Phase 2 - Semantic geometry and frame-resource contract

Work:

1. Audit raw marked-ocean vertex inputs and transforms.
2. Define `BwWaterView` and `BwWaterDraw` contracts with explicit spaces and
   borrowed lifetimes.
3. Carry world/eye position, geometric normal, projected position, view data,
   and validated directional-light data.
4. Add pre-water scene-color capture.
5. Make opaque depth shader-readable or copy/linearize it into an owned
   `R32Float` resource.
6. Add a private nearest-water depth path.
7. Implement per-view resource rectangles and split-screen isolation.
8. Add the atomic shadow-render-and-composite state machine with a diagnostic
   flat-color water shader.

Synthetic coverage must include target switches, partial clears, guest AA,
private scaling, resize, guest seeding/writeback, render-to-texture, split
rectangles, allocation failure, pipeline failure, and a failure after the
first modern draw.

Exit gate:

- Disabled mode is byte-identical Retail.
- A failed modern view leaves the already rendered Retail view unchanged.
- Scene color, opaque depth, water depth, camera reconstruction, and view
  rectangles agree at 1x and scaled resolutions.
- No per-draw synchronization or readback is introduced.

### Phase 3 - Modern Surface v1

Work:

1. Create a bounded linear `RGBA16Float` water composite.
2. Add world-anchored multi-scale micro normals.
3. Runtime-prefilter the title sky cube into roughness mips.
4. Add Fresnel and roughness-aware environment reflection.
5. Add the validated sun/moon specular lobe when semantic light data is ready.
6. Apply title-compatible fog after modern surface lighting.
7. Keep Retail wake foam visually on top or reproduce it unchanged in the
   private composite.
8. Add debug views for geometric normal, micro normal, Fresnel, roughness,
   environment mip, coverage, and final linear luminance.

Exit gate:

- Same-execution review shows an obvious improvement in calm, storm, noon,
  sunset, night, fog, and split-screen scenes.
- Tile seams and camera-locked texture swimming are not visible.
- The modern reflection does not double with the Retail pass.
- The result remains coherent with the title's sky and fog.

Expected quality:

- This is the first release that should look decisively modern, even before
  refraction and depth color.

### Phase 4 - Optical volume and nearby reflection

Work:

1. Add depth-derived thickness.
2. Add bounded normal-based refraction from the pre-water scene copy.
3. Add absorption and in-scattering with art-directed weather presets.
4. Add shallow/deep color transitions without using fog as thickness.
5. Add half-resolution SSR with sky-cube fallback.
6. Add edge, disocclusion, and invalid-depth confidence masks.
7. Add optional underwater/above-water transition work only after the camera
   and composition boundaries are explicitly identified.

Exit gate:

- Shorelines and shallow water gain convincing depth without halos.
- Boats and structures can reflect when valid and fall back without popping.
- Screen edges, split boundaries, cleared depth, and target transitions do not
  leak stale pixels.
- Visibility review passes every representative mission scene.

Expected quality:

- This phase is required to approach the depth and richness of the Black Flag
  reference instead of producing only a better reflective surface.

### Phase 5 - Foam, wakes, and interaction polish

Work:

1. Explicitly classify the complete pass-3 state family.
2. Reproduce the authored wake mask and alpha in the linear water composite.
3. Add world-stable breakup detail without changing wake lifetime or extent.
4. Add conservative crest foam from validated slope/curvature.
5. Add thickness/gradient-based shoreline and object-intersection foam.
6. Evaluate render-only crest spray only if foam still cannot convey storm
   energy; keep it deterministic and independent of gameplay state.
7. Tune day/night luminance so foam remains readable without glowing.

Exit gate:

- Wake-heavy action is clearer than or equal to Retail.
- No foam appears on unrelated transparent geometry or across split views.
- Distant foam remains stable with mipmapping and render scaling.
- Calm water stays calm; storm water gains energy without covering hazards.

### Phase 6 - Optional presentation-only geometry refinement

Do this only if Phase 3-5 captures show that shading is limited by visible
polygonal silhouettes or LOD transitions.

Work:

1. Capture the title's validated 5x5 render patch positions, heights, and
   normals in a water-specific geometry contract.
2. Prototype Metal tessellation or compute-generated subdivision using the
   title surface as the exact boundary.
3. Add explicit patch adjacency or stitch data before attempting variable
   edge factors.
4. Preserve title culling, tile choice, and broad wave shape.
5. Keep added detail presentation-only and initially use zero extra vertical
   displacement.
6. Test boat contact, wake alignment, shore boundaries, horizon silhouette,
   and coarse/fine tile edges.

Exit gate:

- Shared edges are crack-free across quadtree LOD transitions.
- Boats and effects remain visually attached to the simulated surface.
- The GPU cost produces a visible gain at normal play distance.

Do not replace the water with a separate FFT or camera-centered ocean. A
second wave state would visibly disagree with buoyancy, wakes, splashes,
projectiles, shore maps, and AI queries and would violate the current
mechanics-preservation rule.

### Phase 7 - Productization

Recommended settings:

| Setting | Behavior |
| --- | --- |
| Retail | Exact existing three-pass path and default fallback. |
| Enhanced | Lower-risk pass-aware reflection cleanup using existing inputs. |
| Modern | Complete semantic hybrid path with surface detail, volume, and enhanced foam. |

Preserve `realistic` as a recognized compatibility value. Map it explicitly to
the old experimental treatment or migrate it with a visible log/UI notice;
do not silently change an existing saved value into the expensive Modern
path.

Modern quality tiers can then control cost:

| Tier | Suggested features |
| --- | --- |
| Balanced | Prefiltered sky, dual micro normals, direct glint, authored foam, half-resolution thickness/refraction, no SSR. |
| High | Full-resolution thickness/refraction, half-resolution SSR, enhanced wake/crest/intersection foam. |
| Ultra | Higher SSR quality, deeper environment filtering, optional geometry refinement after it passes Phase 6. |

Resource or pipeline failure should report the reason and use Retail for the
whole view. It should not silently downgrade from Ultra to High or from Modern
to Enhanced.

## Provisional performance and memory budgets

These are engineering gates to validate, not claims about current measured
performance:

| Tier | Same-scene GPU p95 overhead over Retail | Extra private memory at 2560x1920 |
| --- | --- | --- |
| Balanced | At most 15% | At most 64 MiB |
| High | At most 25% | At most 96 MiB |
| Ultra | At most 35% | At most 160 MiB |

Also require:

- no added CPU readback in normal rendering;
- no per-water-draw command-buffer completion;
- no unbounded pipeline or texture cache;
- no frame-to-frame resource allocation after warm-up;
- reported pipeline compilation and first-use cost;
- mean, median, p95, and worst-case water/whole-frame GPU time;
- CPU executor time and command-encoding time;
- peak private-resource allocation at 1x, 1080p, 1440p, the verified
  2560x1920 path, and 4K where supported; and
- separate single-player and worst-case split-screen results.

If a milestone exceeds its budget, optimize or reduce that tier's documented
feature set. Do not accept a large regression merely because the screenshot
looks better.

## Validation matrix

### Synthetic and unit coverage

- Every marker version, pass ID, view ID, begin/end order, conflict, unknown
  version, unconsumed marker, and frame carry-over case.
- Exact Retail output with modern mode disabled, unmarked, unsupported, or
  failed.
- Whole-view atomic fallback after failure at each modern state-machine step.
- No double composite and no dropped Retail draw.
- Scene-color and depth ownership across clear, partial clear, target switch,
  render-to-texture, guest writeback, scale change, and resize.
- Split-screen rectangles and per-view resource clearing.
- Fresnel monotonicity and normal/view invalid-input fallback.
- Beer-Lambert monotonicity and finite thickness bounds.
- Refraction screen-edge and cross-view rejection.
- SSR miss, invalid-depth, and sky-cube fallback.
- Foam-mask alpha preservation and distant mip behavior.
- Pipeline/resource allocation failure injection.
- Bounded cache/resource counts and cleanup on backend stop.

### Representative visual coverage

| Scene class | Review focus |
| --- | --- |
| Calm noon | Subtle detail, shallow/deep color, no noisy foam |
| Sunset | Sky hue, Fresnel transition, highlight clipping |
| Night | Distant reflection line, dark-water readability, foam luminance |
| Storm | Crest detail, temporal stability, dense wakes, GPU worst case |
| Heavy fog | One coherent fog model, no double haze |
| Shoreline | Thickness, intersection foam, halos, depth discontinuities |
| Boat close-up | Hull contact, wake attachment, reflection/refraction |
| Projectile/explosion | Dynamic-light coherence and readable splashes |
| Horizon/grazing angle | Reflection, tile seams, LOD, silhouette |
| Split-screen | View isolation, rectangles, resource reuse, cost |
| Mission transition | Target/scale/scene reset and fallback |

Use fixed same-execution crops, pass-isolated images, difference images, and
short motion sequences. A still image is insufficient for shimmer, swimming
normal maps, LOD popping, foam crawl, and SSR instability.

### Acceptance rules

1. Retail remains the default until Modern passes representative visual,
   performance, memory, and failure review.
2. No change ships solely from a synthetic scene.
3. Separate-run images are illustrative only; formal A/B evidence must be
   same-execution and aligned.
4. A modern path must show a clear improvement over Retail and the existing
   `realistic` treatment.
5. Visibility and mechanics review can reject an otherwise attractive effect.
6. Private captures, saves, hashes, logs, and retail-derived data stay out of
   the repository.

## Suggested implementation sequence

Keep changes reviewable and independently testable:

1. **Pass marker patch:** add versioned water view/pass events and executor
   tests without changing pixels.
2. **Metal parity patch:** port the private-depth distant-reflection fix and
   add pass-isolated same-execution capture.
3. **Semantic audit patch:** report aggregate raw input, transform, view, and
   light classifications for marked water only.
4. **Frame-resource patch:** add scene-color/depth/water-depth ownership with a
   flat diagnostic composite and exhaustive fallback tests.
5. **Modern Surface patch:** add linear composition, micro normals, Fresnel,
   prefiltered sky, and validated directional highlight.
6. **Volume patch:** add thickness, refraction, absorption, and scattering.
7. **SSR patch:** add half-resolution nearby reflection with confidence and
   environment fallback.
8. **Foam patch:** reproduce authored wake foam, then add crest/intersection
   terms.
9. **Geometry experiment:** evaluate tessellation only if the accepted
   surface/volume path is visibly geometry-limited.
10. **Settings patch:** expose Retail/Enhanced/Modern and quality tiers after
    their support and failure behavior are complete.

Each patch should update the water, settings, quality, roadmap, and validation
documentation that it changes.

## Code surface map

| Surface | Planned responsibility |
| --- | --- |
| [`src/bw_draw_marker.h`](../../src/bw_draw_marker.h) | Versioned water view/pass event encoding. |
| [`src/bw_hifps_hooks.c`](../../src/bw_hifps_hooks.c) | Existing validated lit-tile marker and mechanics-safe render boundary. |
| [`src/bw_ocean_native.c`](../../src/bw_ocean_native.c) | Pass diagnostics, render-patch evidence, and optional semantic render geometry source. |
| [`src/toolkit_patches/kernel/nv2a_pb_exec.c`](../../src/toolkit_patches/kernel/nv2a_pb_exec.c) | Marker lifetime, pass/view state, raw marked-ocean attribute audit, and semantic batch construction. |
| [`src/toolkit_patches/kernel/bw_backend_ext.h`](../../src/toolkit_patches/kernel/bw_backend_ext.h) | Keep exact `BwXBatch`; add a separate versioned water view/draw contract and callback. |
| [`src/gpu/bw_metal_backend.m`](../../src/gpu/bw_metal_backend.m) | Frame-resource ownership, atomic modern state machine, water pipelines, private targets, composition, and diagnostics. |
| [`src/gpu/bw_metal_backend.h`](../../src/gpu/bw_metal_backend.h) | Modern preset/quality API and explicit capability/failure reporting. |
| [`src/gpu/bw_d3d11.c`](../../src/gpu/bw_d3d11.c) | Reference for the private-depth `OCEAN_REFLECT_SINGLE` behavior. |
| [`src/bw_settings.c`](../../src/bw_settings.c) | Retail/Enhanced/Modern choices, compatibility migration, support state, and apply timing. |
| [`tests/metal_backend/test_main.c`](../../tests/metal_backend/test_main.c) | Shader, resource, fallback, pass, quality, and state-machine coverage. |
| [`tests/hook_portability/test_markers.c`](../../tests/hook_portability/test_markers.c) | Guest marker encoding and register/state preservation. |
| [`tests/hook_portability/test_ocean.c`](../../tests/hook_portability/test_ocean.c) | Render-only ocean boundary and fallback coverage. |

The modern shader and resource manager will become too large to remain one
branch inside `extended_shade`. After the semantic prototype is proven,
separate the water state/pipeline code from the generic NV2A translator while
continuing to compile through the existing Metal library mechanism. Avoid a
large backend refactor before Phase 2 has fixed the actual contract.

## Main risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Incorrect pass identity | Emit versioned identity at validated title boundaries; reject unknown/conflicting scopes. |
| Partial modern failure after Retail suppression | Keep Retail draws and use atomic private composition until complete preflight is proven. |
| Color-space mismatch | Use debug ramps and reference equations; document every decode, blend, fog, encode, and gamma step. |
| Double reflection | Integrate reflection in one modern composite and overwrite Retail water coverage only after success. |
| Depth aliases or stale data | Define ownership across target switches, clears, writeback, scaling, and view rectangles; fail closed. |
| Split-screen leakage | Carry explicit view ID/rect and clear/reuse only inside that rectangle. |
| Boat/water separation | Keep title geometry and simulation; use normal-only micro detail before any displacement. |
| Tile cracks or LOD rings | Defer tessellation; require adjacency/stitch data and representative grazing-angle tests. |
| SSR popping | Confidence fade to the sky cube; never make SSR the only reflection source. |
| Foam hides gameplay | Preserve title wake semantics and require visibility review in calm/storm/night scenes. |
| Excess memory at native resolution | Use explicit tier budgets, lazy allocation, bounded mip hierarchies, and reported failure to Retail. |
| Attractive but unstable still images | Require motion review and same-execution sequences, not screenshots alone. |
| Art/licensing problems | Use procedural/project-owned resources and never commit extracted retail art. |

## Go/no-go checkpoints

1. **After Phase 1:** If all passes cannot be identified safely, stop at
   Retail parity and do not build a replacement shader from pattern guesses.
2. **After Phase 2:** If scene color/depth ownership cannot remain exact across
   scaling, target switches, and split-screen, stop before refraction/SSR.
3. **After Phase 3:** If a linear reflective surface with semantic inputs is
   not clearly better within budget, revisit art direction and inputs rather
   than adding more expensive effects.
4. **After Phase 4:** If thickness/refraction reduce readability or remain
   unstable, ship Modern Surface without them in Balanced mode.
5. **After Phase 5:** Compare the result against the visual target in motion.
   Only then decide whether geometry refinement is necessary.
6. **After Phase 6:** If tessellation does not create an obvious normal-play
   improvement, remove it and keep the lower-risk shader path.

## Recommended first milestone

The first implementation milestone should be **pass-aware Modern Surface**,
not refraction and not a new wave simulation:

1. Explicitly mark all three passes and water-view boundaries.
2. Add same-execution dual-target capture.
3. Port the distant-water-line fix to Metal.
4. Build the atomic private composite with a flat diagnostic result.
5. Add world-stable micro normals, prefiltered title-sky reflection, Fresnel,
   and validated sun/moon glint.
6. Preserve Retail foam and all mechanics.

That milestone creates a visible improvement, validates the architecture that
later depth/refraction/foam work needs, and remains recoverable to exact Retail
for every unsupported or failed view. It is the shortest credible path toward
the Black Flag quality goal without compromising Blood Wake's simulation or
the renderer's current correctness guarantees.
