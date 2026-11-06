# Blood Wake water rendering

Updated 2026-11-05.

This document is the source-grounded map of Blood Wake's water system for the
Apple Silicon port. It separates simulation from presentation, records what
the Retail renderer actually submits, explains what the optional Metal
Realistic preset changes, and defines safe boundaries for future work.

The governing rule is:

> Improve presentation without changing water simulation, boat behavior,
> collision, AI, mission logic, visibility, or timing.

Retail files, generated code, captures, private logs, addresses, fingerprints,
and extracted assets are deliberately absent. The code links below point only
to project-owned source.

## Evidence vocabulary

- **Verified** means the behavior is directly represented in project source
  and covered by synthetic tests, or was isolated by an existing diagnostic.
- **Runtime-verified** means bounded private runs exercised the behavior, but
  the private evidence itself is not published.
- **Inferred** means multiple observations support the explanation, but no
  isolated test proves it yet.
- **Unknown** means future work must measure or classify it before changing
  production behavior.

These labels matter. A texture-stage shape or fingerprint can corroborate an
ocean draw, but neither is allowed to identify one by itself.

## System at a glance

Blood Wake's water is not one shader. It is a pipeline with separate owners:

| Layer | Owner | What it controls | Presentation-only work allowed? |
| --- | --- | --- | --- |
| Water simulation | The title's `Water`/`WaterWorld` code | Swell, shore response, ripples, wakes, splashes, sinking disturbances, and time | No, unless separately requested and proven mechanics-neutral |
| Ocean geometry | The title's ocean renderer | Tile selection, render LOD, evaluated heights/normals, and draw order | Only exact/cached render evaluation or separately gated visual LOD |
| Retail material | NV2A texture stages and register combiners | Lit surface, sky reflection, wake foam, fog, alpha, and blending | Yes, if Retail remains exact and the optional path is isolated |
| Metal translation | The project Metal backend | Equivalent texture-stage, combiner, depth, blend, fog, and presentation behavior | Yes |
| Realistic preset | A separate Metal fragment entry | A small RGB-only post-combiner treatment on one qualified ocean pass | Yes; default-off and Remastered-only |

The current optional shader therefore cannot create a new water model. It can
only reshape data already submitted by the title for the one pass that is
explicitly marked and qualified.

## 1. Simulation and render geometry

### Water state

**Verified.** The title maintains a double-buffered wave height field and
cached bicubic patches. Render-vertex evaluation combines the broad swell with
shore maps, static calm zones, ripples, primary and secondary wake trails,
projectile splashes, boat-line disturbances, and sinking disturbances.

Those inputs are also used outside ocean rendering. Boats, hulls, projectiles,
AI, cameras, and a secondary water-render path can query the same underlying
system. They must not be redirected through an experimental presentation
shader or modified to make the picture look different.

### Ocean tiles

**Verified.** `Ocean_Render` selects visible tiles through a quadtree and
`Ocean_FillVertices` asks `WaterWorld_EvaluateVertexBatch` for each tile's
render vertices. The validated ordinary tile layout contains a regular grid
of positions, heights, normals, and related interpolants.

The native-ocean optimization in
[`bw_ocean_native.c`](../src/bw_ocean_native.c) is deliberately restricted to
this render-vertex call. Its translated arithmetic keeps the title's
operation order and floating-point behavior, rejects unsafe ranges, and falls
back to the title's generated body. It does not replace gameplay water
queries.

### High-frame-rate interpolation

**Verified.** The high-frame-rate path in
[`bw_hifps_hooks.c`](../src/bw_hifps_hooks.c) caches render-vertex results and,
between simulation steps, blends the render outputs from the previous and
current water states. Its slower diagnostic mode temporarily blends the
height field and clock, then restores the water object, patch cache, and clock
byte-for-byte before simulation resumes.

This is an important precedent for future work: rendering may interpolate a
read-only view of simulation state, but the next game step must observe the
same state it would have observed without the extra frame.

### Split-screen ocean detail

**Verified.** The Remastered split-detail option changes the factor passed to
the title's ocean quadtree walk. Full uses the one-player sea detail in every
view; Balanced uses a coarser title-defined level for the heaviest
split-screen cases. It changes presentation cost and geometric detail, not
wave state or gameplay queries.

## 2. The three Retail ocean passes

The title exposes developer switches for three ocean passes. Project
diagnostics and the Direct3D renderer establish this high-level division:

| Pass | Verified role | Texture-stage evidence | Current Realistic coverage |
| --- | --- | --- | --- |
| 1 | Lit/base water surface | `1/4/4/4`: stage 0 sampled, stages 1-3 pass through interpolants | Yes, only when explicitly marked |
| 2 | Sky reflection | `1/17/11/12`: sampled input, dot product, diffuse cube reflection, specular cube reflection | No; Retail path only |
| 3 | Wake foam | Uses the ocean-frame/wake-mask alpha relationship and is blended over the sea | No; Retail path only |

All three passes are part of the final picture. The sea uses blending and does
not make the scene depth buffer an authoritative water-depth field.

### Pass 1: lit/base surface

**Verified and runtime-verified.** The explicitly marked batches exercised by
the Apple Silicon diagnostics consistently use `1/4/4/4`:

- stage 0 samples an existing water input;
- `tc1` carries an encoded interpolated water normal;
- `tc2` carries an encoded eye/view basis;
- `tc3` is another pass-through interpolant;
- the Retail register combiners produce the pass output using the title's
  diffuse/specular colors, constants, fog, alpha, and stage sample.

The exact artistic role of every stage-0 channel over all animated water
frames is not fully documented. It is safe to call it the **stage-0 water
sample**. It is not safe to describe it as the complete sky reflection,
because the title has a later, dedicated cube-reflection pass.

### Pass 2: sky reflection

**Verified.** The Retail reflection path uses a sampled first stage followed
by NV2A dot-product reflection stages. The renderer:

1. maps the sampled input into a signed vector;
2. dots it with interpolated basis rows;
3. builds a perturbed normal;
4. forms diffuse and specular cube lookup directions;
5. samples the existing sky cube;
6. feeds those samples through the title's register combiners and blending.

Both the DX11 shader generator and Metal's Retail `extended_shade` implement
these stage semantics. The cube is uploaded as six slices, with explicit
Direct3D-compatible face selection.

This pass is drawn after the lit pass. **Inferred:** because it remains Retail
and blends over pass 1, it can visually dominate or mute a small color change
made only to pass 1. That is a likely reason the current Realistic preset looks
almost identical in the exercised scene, but representative isolated pass
captures are still required.

### Pass 3: wake foam

**Verified at the system level.** The title keeps animated ocean frames and
corresponding wake masks. Mask alpha contributes to the third pass, weighted
by title-provided vertex alpha. The Remastered distant-wake fix rebuilds
incorrect lower mip alpha from the correct top level; it changes distant foam,
not wave geometry, sea color, or the sky.

The pass now has an explicit title-level submission boundary and semantic
marker. Its complete Metal state family has not yet been catalogued.
Replacement foam work must use that boundary rather than texture names,
addresses, hashes, or broad stage-pattern guesses.

## 3. From the title to Metal

### Explicit water marker protocol

The classifier uses versioned commands in the title's own graphics stream:

1. The title's split-view loop stores its current one-based view ordinal and
   calls the complete ocean renderer once for that view.
2. [`bw_water_hooks.c`](../src/bw_water_hooks.c) validates that call context,
   converts the ordinal to zero-based protocol identity, and brackets the
   complete call with view begin/end events.
3. The validated `Ocean_FillVertices` path still emits a one-shot lit/base
   event through [`bw_hifps_hooks.c`](../src/bw_hifps_hooks.c).
4. Separate validated wrappers bracket the title's reflection and foam
   submission functions with persistent pass-run begin/end events. Each
   generated body always executes; marker emission is Remastered-only and
   also requires the expected title object, call site, pass enable, tile
   count, and draw root.
5. The executor in
   [`nv2a_pb_exec.c`](../src/toolkit_patches/kernel/nv2a_pb_exec.c) applies a
   one-shot event to only the next `BEGIN/END` scope, or a pass run to every
   scope until its matching end.
6. Every classified extended batch receives `BW_X_DRAW_MARKER_OCEAN` plus a
   versioned `BwXWaterDraw` containing the pass and inherited view.
7. Draw state clears at `END`; unconsumed one-shots, active runs, and open
   views reject and clear at the frame boundary.

Repeated identical current-version markers may coalesce before one later draw.
The exact old version-1 ocean marker remains accepted as a legacy lit event.
Unknown versions/events, payload misuse, conflicting markers, markers inside a
draw, nested or mismatched water views or pass runs, one-shot events inside a
run, unmatched ends, frame carry-over, and one-shot marked draws that produce
no extended batch are rejected explicitly. Empty `BEGIN/END` scopes inside a
valid persistent pass run are accepted because the title emits them during
real reflection/foam gameplay. Rejection leaves the draw on Retail.

Executor tests cover persistent multi-draw runs, empty runs and empty scopes
within runs, all three passes, four-view inheritance, and fail-closed
recovery. Hook tests cover exact event order, Original passthrough, disabled
and empty passes, invalid views, and guest register/stack preservation. A
freshly prepared one-player Battle run demonstrated sustained gameplay,
nonzero scopes for all three passes, and balanced view-0 counts. A post-fix
repeat must still demonstrate zero rejection before the runtime boundary task
is accepted; view-1 validation is a later non-blocking protocol check.

The marker is authoritative because it originates at a validated title
boundary and travels in command order. A register-combiner pattern is not
authoritative: another material could use the same pattern.

### Metadata fingerprints

Optional GPU auditing computes bounded fingerprints only after a marker has
already classified a batch. The fingerprint covers stable copied render
state, active combiner state, texture layout/sampler metadata, fog state, and
vertex-program instructions.

It deliberately excludes guest addresses, host pointers, vertices, texture
bytes, asset names, and captures. Fingerprints are consistency evidence, not
classifiers and not a public asset signature.

### Extended batch contract

[`bw_backend_ext.h`](../src/toolkit_patches/kernel/bw_backend_ext.h) hands the
GPU backend the inputs consumed by the Retail NV2A pixel path:

- projected position and reciprocal clip W;
- diffuse and specular colors;
- processed and raw fog visibility;
- four texture-coordinate vectors;
- up to four texture stages and sampler/layout metadata;
- register-combiner inputs, outputs, constants, and final combiner;
- dot-product mapping and earlier-stage selection;
- blend, depth, stencil, cull, alpha-test, and color-mask state;
- fog color; and
- the scoped compatibility marker and versioned water pass/view identity.

The contract is borrowed synchronously. It does not provide world-space
position, a material database, scene-color history, motion vectors, or a
shader-readable water-depth field.

### Retail Metal translation

Metal's `extended_fragment` calls the common `extended_shade` body with the
optional treatment disabled. That body reproduces:

- projective sampling and address/filter modes;
- pass-through stages;
- diffuse/specular dot-reflection stages;
- one to eight general register-combiner stages;
- the final combiner;
- alpha testing; and
- the title's final RGBA output.

The existing Retail entry, key, and output path remain the default for every
draw. Internal render scaling changes private target dimensions and
presentation sharpness, not the title's water state or material program.
Metal anisotropy changes only linear, mipmapped samples; point-filtered and
unmipped stages remain 1x.

## 4. What the optional Realistic preset does

`graphics.water_rendering=realistic` is Remastered-only, macOS Metal-only,
default-off, and applied at an executor flip boundary. It uses a separate
pipeline key and `realistic_water_fragment`; it does not replace
`extended_fragment`.

### Qualification

A batch qualifies only when:

- it carries the explicit ocean marker;
- stage 0 is a valid sampled 2D texture; and
- the stage program is the verified `1/4/4/4` shape.

An unmarked draw always uses Retail. An unsupported marked draw falls back for
that pass. If the separate pipeline cannot be created, the backend reports the
failure, restores both requested and active presets to Retail, and retries the
Retail pipeline instead of dropping the draw.

### Fragment operation

The common shader first computes the complete Retail result and performs the
Retail alpha test. Only then does the optional branch:

1. decode `tc1.rgb * 2 - 1` as the interpolated normal;
2. decode `tc2.rgb * 2 - 1` as the eye/view basis;
3. reject non-finite or near-zero vectors by leaving Retail unchanged;
4. compute a restrained Schlick-style view-angle term with water-like base
   reflectance;
5. slightly desaturate the stage-0 sample as a roughness proxy;
6. mix that proxy toward the title's existing fog/atmosphere color;
7. blend a small amount into the Retail RGB, bounded by the existing fog
   visibility; and
8. return the original Retail alpha unchanged.

This is an experimental color treatment, not physically based water. It does
not alter geometry, displacement, foam, pass ordering, depth, blending, or
simulation.

### Why it currently looks close to Retail

The small visual difference is consistent with the implementation:

- only pass 1 is changed;
- the blend strength is intentionally low;
- the title's later sky-reflection pass remains Retail;
- wake foam remains Retail;
- no new normal detail, reflection source, depth cue, refraction, absorption,
  shoreline cue, light direction, or art is available; and
- the effect fades with the title's existing fog visibility and preserves
  Retail alpha.

The current name should not be read as a claim of physical realism or a
quality promotion. The preset remains experimental and absent from quality
presets.

## 5. Shader-input inventory

| Input or capability | Current availability | Confidence | Current optional use | Important limit |
| --- | --- | --- | --- | --- |
| Complete Retail RGBA | Yes | Verified | RGB is the base; alpha is preserved | Already includes title combiner decisions |
| Stage-0 water sample | Yes on qualified pass | Verified | Reflection/roughness proxy | Not the complete later cube reflection |
| Encoded interpolated normal | `tc1.rgb` | Runtime-verified | View-angle term | No separate micro-normal or roughness map |
| Encoded eye/view basis | `tc2.rgb` | Runtime-verified | View-angle term | Not a full camera or world-space position |
| Fog color | Yes | Verified | Atmosphere target | It is scene haze, not water absorption color |
| Fog visibility | Yes | Verified | Bounds optional strength | It is not water thickness |
| Diffuse/specular colors | Available to common shader | Verified | Not specially reinterpreted | Semantics vary by pass |
| Four texture-coordinate vectors | Available to common shader | Verified | Two are used on pass 1 | Remaining semantics need pass-specific proof |
| Retail sky cube | Yes on reflection pass | Verified | Not used by current optional branch | Pass 2 is not explicitly marked |
| Sampler/mip state | Yes | Verified | Retail sampling and optional anisotropy | Some water stages may be unmipped or point filtered |
| Scene color before water | No shader contract | Verified absent | None | Required for conventional refraction |
| Shader-readable scene depth | No | Verified absent | None | Bound depth testing is not a sampled thickness field |
| Water-surface depth/thickness | No | Verified absent | None | Scene depth alone would usually describe seabed/opaque geometry |
| World-space position | No | Verified absent | None | Projected position cannot replace it generally |
| Sun/light direction | No pass-specific contract | Unknown | None | A guessed highlight could conflict with scene lighting |
| Shoreline/material mask | No | Verified absent | None | Shore response exists in simulation but is not exposed as a material mask |
| Motion vectors/history | No | Verified absent | None | No safe temporal water treatment |

Metal hardware may support techniques beyond this table. The limitation is
the current renderer contract and evidence, not a claim that Apple GPUs are
incapable of them.

## 6. Interactions with other features

### Fog

Fog is part of the Retail combiner contract. The optional shader uses the same
fog color and visibility rather than inventing a second distance system. This
keeps distant water integrated with the scene, but fog cannot stand in for
depth-dependent water absorption.

### Reflection and blending

The Retail cube-reflection pass is distinct from the currently modified lit
pass. All ocean passes use the title's ordering and blend state. Changing
Retail alpha would change both appearance and how repeated layers accumulate,
so the optional shader preserves it exactly.

### Wakes and disturbances

There are two related but separate representations:

- simulation disturbances affect evaluated heights and normals; and
- the foam pass uses title-authored frame/mask alpha.

A presentation shader may shade the resulting normal or preserve/reweight
existing foam after its pass is explicitly identified. It must not edit the
disturbance records, wake lifetime, boat interaction, or projectile response.

### LOD

Ocean tile LOD changes geometric sampling density. Anisotropic filtering
changes eligible texture sampling. Neither adds simulation detail. Visual
tests must include split-screen because its title-defined tile factor and
cost differ from one-player rendering.

### Internal scaling

Private Metal render targets improve edge and texture presentation while the
guest framebuffer contract remains unchanged. Scaling provides more output
pixels, but it cannot recover geometry, normals, foam masks, or reflection
detail that the title did not submit.

### Texture restoration

The DX11 texture-restoration feature deliberately excludes water textures,
normal maps, cube maps, and effect sprites. The Metal water path therefore
continues to use the title's original water and effect art. Any future asset
replacement would be a separate policy decision and is out of scope here.

## 7. Existing Remastered water-related work

| Feature | Scope | Apple Silicon status |
| --- | --- | --- |
| Native ocean evaluation | Exact acceleration of render-vertex work only | Implemented with fallback and verification |
| High-FPS water interpolation/cache | Extra render frames only; simulation state restored | Implemented |
| Split-screen sea detail | Title-defined render LOD factor | Implemented |
| Distant wake foam | Rebuilds lower mip alpha for pass 3 | Implemented in shared game-side hook |
| Retail dot/cube reflection | Translates NV2A reflection stages | Implemented in Metal |
| Metal anisotropy | Eligible linear, mipmapped stages only | Implemented |
| Optional Realistic pass-1 treatment | Separate marked fragment pipeline | Implemented, experimental, default-off |
| Distant-water reflection line | Private-depth gating of pass 2 overlap | Implemented in DX11 and Metal |

Metal copies authoritative scene depth into a private
`Depth32Float_Stencil8` target once per validated water view. Lit groups prime
nearest-water depth with `LESS_EQUAL`, depth writes, and the DX11-compatible
additional bias; reflection groups use that private depth with writes
disabled. Foam preserves the view-level snapshot for the next group, while
clears, flips, basic/non-water draws, target changes, view changes, and backend
stop reset its lifetime. Scene depth and stencil are never modified. Failure
disables the optional fix for that backend lifetime and continues with Retail
reflection depth.

## 8. Safe improvement opportunities

### Tier A: existing inputs and presentation-only state

These are the safest next investigations:

1. **Tag all three ocean passes explicitly.** Add versioned pass identity at
   validated title boundaries rather than classifying by shader state.
2. **Build same-execution A/B capture.** Alternate one isolated treatment
   within a single bounded run and pair adjacent stable frames. Separate runs
   drift too much for pixel comparison.
3. **Re-evaluate pass-1 shaping.** Treat stage 0 as a water sample, not as the
   complete sky reflection, and tune only after representative pass-isolated
   evidence.
4. **Prototype optional pass-2 shaping.** Once pass 2 is explicitly marked,
   adjust the existing cube result with the title's own normal/view basis.
   Keep alpha, pass order, and fallback exact.
5. **Measure anisotropy on actual water stages.** Record which stages are
   linear and mipmapped before expecting a visible difference.

Every item remains default-off until Retail isolation, representative visual
coverage, and GPU cost are accepted.

### Tier B: requires a new renderer contract

These effects are possible only after explicit resource ownership is designed:

- scene-color copy or safe framebuffer input for refraction;
- shader-readable opaque depth plus a separate water-surface depth for
  thickness;
- depth-based absorption and shoreline intersection;
- dedicated shoreline or foam material identity;
- stable world-space position and lighting inputs;
- temporal history and motion data; or
- new procedural or replacement art.

These features need correctness work for target switches, partial clears,
guest-seeded surfaces, scaling, guest AA, readback, resize, and pass ordering.
They are not safe one-line shader additions.

### Out of scope without a separate mechanics project

- changing wave FFT/state evolution;
- changing shore maps or disturbance amplitudes;
- changing wake, splash, sinking, or ripple lifetime;
- changing buoyancy, collision, handling, projectile interaction, AI, or
  camera water queries;
- changing simulation tick rate;
- making water more transparent in a way that changes target or hazard
  visibility; and
- modifying retail assets in place.

## 9. Validation requirements

Any future optional water change must pass all of these gates:

1. **Retail identity:** default Retail and marked-Retail output remain exact in
   deterministic tests.
2. **Scope isolation:** only the intended explicit pass marker selects the new
   pipeline; the first unmarked draw returns to Retail.
3. **Fallback:** unsupported state and pipeline failure render through Retail,
   with an explicit diagnostic.
4. **Mechanics isolation:** deterministic game-step state is unchanged; any
   temporary render interpolation restores state before simulation resumes.
5. **Representative scenes:** calm and storm water, day and night lighting,
   shorelines, wake-heavy action, reflection-heavy angles, split-screen, fog,
   and mission transitions.
6. **Same-execution visual evidence:** fixed private crops from aligned
   frames. Captures and retail-derived images stay outside source control.
7. **Performance:** warm-up, mean/median/p95 GPU duration, resolution,
   hardware, OS, scene, and run order. Whole-command-buffer timing must not be
   called isolated shader cost.
8. **Visibility review:** no reduction in readable wakes, hazards, boats,
   projectiles, shore boundaries, or mission cues.
9. **Privacy:** no addresses, hashes, asset bytes, generated code, logs, saves,
   or captures in commits or reports.

The existing ABBA benchmark and its limits are documented in
[METAL_QUALITY.md](METAL_QUALITY.md), and aggregate runtime evidence is in
[VALIDATION.md](VALIDATION.md).

## 10. Current evidence and limitations

- Synthetic tests prove exact default-Retail behavior, marked-only optional
  selection, frame-boundary activation, unmarked recovery, unsupported-state
  fallback, Original rejection, and pipeline-failure fallback.
- Bounded private runs prove that the explicit marker and optional pipeline
  activate on the earlier lit-only runtime path without observed marker
  rejection or unsupported marked-pass fallback in the exercised scene.
- Source and synthetic tests prove the new view, reflection-run, and foam-run
  lifecycle. Those new events do not yet have representative private runtime
  acceptance evidence.
- The controlled ABBA benchmark records a measurable whole-command-buffer
  cost for the optional shader. It is not isolated fragment cost.
- The user's hands-on assessment is that the current treatment looks almost
  identical to Retail.
- Separate bounded Retail and Realistic capture runs each completed a
  four-frame sequence, but camera/scene phase drift means they are not
  pixel-aligned A/B evidence. They must not be used to claim visual
  improvement or equivalence.
- Calm/storm, broad lighting, shoreline, split-screen, reflection, and
  wake-heavy coverage remains incomplete.

## 11. Open research questions

1. Do freshly prepared single-player and split-screen runs report balanced
   view scopes, nonzero counts for all three passes, and zero rejection?
2. What is the precise channel role of the pass-1 stage-0 sample across every
   animated water frame and weather state?
3. Which pass-1 and pass-2 interpolants are stable semantic inputs across all
   scenes and split-screen views?
4. What complete state family does pass 3 use on Metal, and which alpha source
   controls its final blend?
5. Which water stages have usable mip chains and linear minification, making
   anisotropy relevant?
6. Can the DX11 distant-water-line depth-copy fix be ported without adding an
   extra synchronization point or violating private-target ownership?
7. How much of the near-Retail appearance is caused by conservative strength,
   and how much is caused by the later unchanged reflection/foam passes?
8. What same-execution capture point can freeze camera and title phase without
   changing simulation behavior?
9. Would a pass-2-only optional treatment produce a meaningful improvement
   before any scene-color/depth contract is justified?

## 12. Code and documentation map

- [`src/bw_hifps_hooks.c`](../src/bw_hifps_hooks.c): water render
  interpolation and cache.
- [`src/bw_water_hooks.c`](../src/bw_water_hooks.c): validated per-view,
  lit-run, reflection-run, and foam-run marker boundaries.
- [`src/bw_ocean_native.c`](../src/bw_ocean_native.c): exact native
  render-vertex evaluation and diagnostic pass research.
- [`src/bw_visextra_hooks.c`](../src/bw_visextra_hooks.c): split-screen sea
  detail and distant-wake mip-alpha fix.
- [`src/bw_draw_marker.h`](../src/bw_draw_marker.h): versioned in-stream
  marker format.
- [`src/toolkit_patches/kernel/nv2a_pb_exec.c`](../src/toolkit_patches/kernel/nv2a_pb_exec.c):
  marker lifetime, extended batches, and bounded metadata fingerprints.
- [`src/toolkit_patches/kernel/bw_backend_ext.h`](../src/toolkit_patches/kernel/bw_backend_ext.h):
  title-to-backend shader contract.
- [`src/gpu/bw_metal_backend.m`](../src/gpu/bw_metal_backend.m): Retail Metal
  translation, optional fragment entry, fallback, and benchmark.
- [`src/gpu/bw_rc_hlsl.c`](../src/gpu/bw_rc_hlsl.c): DX11 reference
  translation for NV2A texture stages and register combiners.
- [`src/gpu/bw_d3d11.c`](../src/gpu/bw_d3d11.c): Retail reflection reference
  and the original distant-water-line fix design.
- [`src/bw_settings.c`](../src/bw_settings.c): user-facing water, filtering,
  split-detail, and visual-fix settings.
- [Metal quality settings](METAL_QUALITY.md): current optional-preset behavior
  and GPU benchmark.
- [Validation](VALIDATION.md): aggregate runtime evidence and limitations.
- [Native feature roadmap](NATIVE_FEATURE_ROADMAP.md): prioritized follow-up
  work.
