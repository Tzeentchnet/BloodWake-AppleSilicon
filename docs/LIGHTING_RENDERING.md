# Blood Wake lighting and rendering

Updated 2026-11-03.

This document is the source-grounded map of Blood Wake's lighting and rendering
path on Apple Silicon. It describes where the title, the NV2A executor, and the
Metal backend each own work; identifies which lighting and material semantics
survive into Metal; and evaluates incremental Metal improvements, real-time
per-pixel lighting, selective PBR, and renderer replacement.

The governing rules are:

> Preserve an exact Retail path, and do not change simulation, visibility,
> camera behavior, draw ordering, split-screen behavior, mission logic, or
> timing merely to improve presentation.

> Do not infer a physical material or light from a low-level shader pattern
> when a validated title boundary can identify it explicitly.

Retail files, generated code, captures, private logs, addresses, program
fingerprints, and extracted assets are deliberately absent. The code links
below point only to project-owned source.

## Evidence vocabulary

- **Verified** means behavior is directly represented in project source and
  covered by synthetic tests, or the current interface makes the fact
  unambiguous.
- **Runtime-verified** means a bounded private run exercised the behavior, but
  the private evidence itself is not published.
- **Inferred** means the implementation and observations support the
  explanation, but no isolated test proves it for every scene.
- **Unknown** means representative instrumentation must classify the behavior
  before production rendering changes.

## Executive conclusion

The Apple Silicon build already has a native Metal renderer, but it is a
**low-level NV2A compatibility backend**, not a scene renderer. It receives
triangles, textures, fog, blend/depth state, and register-combiner programs
after most high-level meaning has been erased.

For the current Apple path:

- the title builds the scene and submits Xbox D3D/NV2A work;
- the executor evaluates fixed-function transform and lighting on the CPU;
- the executor also interprets the title's NV2A vertex programs on the CPU;
- Metal receives projected vertices with diffuse/specular colors, fog, and
  texture coordinates;
- Metal samples the title's textures and reproduces texture-stage and
  register-combiner behavior; and
- Metal scales, resolves, applies the title's gamma ramp, and presents.

The title therefore already has real-time lighting, but much of it is
per-vertex and its result reaches Metal as color rather than as lights,
materials, world-space positions, and normals. A replacement Metal fragment
shader cannot recover all of those inputs reliably.

The recommended direction is a **semantic hybrid renderer**:

1. inventory representative fixed-function, vertex-program, shadow, and
   material use;
2. close Retail fidelity gaps before using those states as an enhancement
   foundation;
3. add explicit title-to-renderer identities and semantic batch contracts;
4. pilot opt-in per-pixel lighting on one opaque fixed-function draw family;
5. introduce linear/HDR intermediate rendering and explicit material
   parameters before calling any path PBR; and
6. replace selected 3D material families before considering a complete scene
   renderer.

A new backend that still consumes only the same low-level post-transform
command stream could improve organization or performance, but it would not
solve the missing-semantics problem and should not be described as a PBR
renderer.

## 1. Current frame path

The current path is:

```text
title scene graph, world, ocean, effects, shadow work, and UI
    -> Xbox D3D/NV2A push-buffer methods and guest resources
    -> push-buffer scanner and NV2A executor
    -> fixed-function transform/lighting OR NV2A vertex-program interpreter
       on the CPU
    -> projected triangles plus colors, fog, texture coordinates, texture
       stages, combiners, and render state
    -> Metal render targets, rasterization, sampling, combiners, depth,
       blending, scaling, gamma, and CAMetalLayer presentation
```

There are three practical draw paths:

| Path | Transform/lighting owner | What Metal receives |
| --- | --- | --- |
| Fixed-function 3D | Executor CPU code | Projected position, computed diffuse color, empty or supplied specular color, fog, and texture coordinates |
| Vertex-program 3D | Executor CPU interpreter | Projected position and the title program's `oD0`, `oD1`, fog, and four texture-coordinate outputs |
| Pre-transformed/screen-space | Executor basic path | Screen-space position, packed diffuse color, and stage-0 coordinates |

The extended contract in
[`bw_backend_ext.h`](../src/toolkit_patches/kernel/bw_backend_ext.h) exists
because the basic toolkit interface did not carry enough state to reproduce
Blood Wake's register combiners. It adds the data consumed by the Retail
pixel path, not a high-level scene description.

### Title-level ownership

**Verified.** The title still owns:

- scene traversal and drawable submission;
- view culling, mesh LOD, and depth sorting;
- object/world/camera transforms;
- material and texture choice;
- vertex-program and constant selection;
- shadow, reflection, ocean, particle, transparent, and UI pass ordering; and
- split-screen view construction.

The existing LOD hook in
[`recomp_manual.c`](../src/recomp_manual.c) identifies
`RenderMgr_CullItemTree`, `World_DrawGridCells`, and `ShadowMap_Render` as
callers of the title's visibility routine. That proves useful title-level
boundaries exist. It does not yet establish the complete shadow algorithm,
map count, light ownership, or Metal state family.

No current project interface exports a frame-wide list of render objects,
meshes, materials, lights, cameras, or passes. By the time a normal draw
reaches Metal, object and material identity are generally gone.

## 2. Current lighting

### Fixed-function lighting

**Verified.** For fixed-function batches, the executor reads the mirrored
NV2A light and material-related registers and computes one diffuse color per
vertex in [`nv2a_pb_exec.c`](../src/toolkit_patches/kernel/nv2a_pb_exec.c).

The implemented model supports:

- a scene ambient color;
- up to eight enabled light slots;
- infinite and local light directions;
- local-light range and constant/linear/quadratic attenuation;
- ambient and diffuse light terms;
- vertex-color routing through the supported color-material fields;
- material alpha; and
- normalized eye-space normals.

The implemented RGB equation is equivalent to:

```text
scene ambient
  + sum(light attenuation
        * (light ambient + light diffuse * max(0, normal dot light)))
```

The XDK has already folded material emission plus material ambient times global
ambient into the scene-ambient value, and has pre-multiplied material terms
into the light colors. That is sufficient to reproduce the intended NV2A
sum, but it means the executor no longer has clean, physically meaningful
base-material and light-intensity values to feed a PBR model.

The current fixed-function implementation explicitly does **not** model:

- specular lighting;
- spot cones;
- back-face material colors or hardware two-sided lighting;
- inverse-transpose normal transformation for non-uniform scale;
- fixed-function skinning;
- texture-coordinate generation or texture matrices; or
- a semantic material object.

These are fidelity gaps, not enhancement opportunities. They should be
measured against Blood Wake's actual use and completed where required before
an optional modern path relies on the same state.

### Vertex-program lighting

**Verified.** When the title selects program mode, the executor runs the
NV2A program through the interpreter in
[`nv2a_vsh_interp.c`](../src/toolkit_patches/kernel/nv2a_vsh_interp.c). The
program can consume raw vertex attributes and the NV2A constant file and can
produce projected position, diffuse and specular colors, fog, point size, and
four texture-coordinate vectors.

The interpreter knows instruction behavior; it does not know whether a
constant is a sun direction, matrix row, material color, animation parameter,
or unrelated value. Likewise, an output color can contain lighting, a mask,
or another title-defined intermediate.

[`BwXVsBatch`](../src/toolkit_patches/kernel/bw_backend_ext.h) can carry a
program, constants, raw inputs, and indices to a backend that implements
`draw_vsh`. The DX11 backend uses that option. The Metal backend currently
registers only `metal_draw_extended`; its `draw_vsh`, readback, and raw-input
callbacks are null. Apple Silicon therefore runs the title's vertex programs
on the CPU and sends only their post-transform outputs to Metal.

Porting GPU vertex-program execution to Metal is a valid performance and
architecture project. By itself it would still emulate the title's program;
it would not turn opaque constants and attributes into PBR semantics.

### Fragment shading

**Verified.** Metal's
[`extended_shade`](../src/gpu/bw_metal_backend.m) reproduces the current
Retail pixel pipeline:

- up to four projective texture stages;
- pass-through and the used dot-product reflection programs;
- one to eight general register-combiner stages;
- the final combiner;
- fog input and color;
- alpha testing;
- blending, color masks, culling, depth/stencil, and polygon offset; and
- the title's final RGBA result.

The Metal fragment path normally performs no new scene-lighting equation. It
interpolates colors already produced by the executor or title vertex program,
samples the title's textures, and evaluates the title's combiner program.

The optional water fragment is a deliberately narrow exception. As documented
in [WATER_RENDERING.md](WATER_RENDERING.md), it uses explicitly qualified
water inputs for a small presentation-only treatment. It does not establish a
general material or lighting contract.

### Shadows, reflections, and fog

- **Shadows:** A title-level `ShadowMap_Render` boundary is known, and the
  renderer supports ordered render-to-texture use of color surfaces. The
  complete Blood Wake shadow pass, receiver materials, address modes, light
  matrices, scene coverage, and Metal parity remain **unknown** until isolated
  by diagnostics.
- **Reflections:** The Retail ocean cube-reflection stages are implemented and
  documented in [WATER_RENDERING.md](WATER_RENDERING.md). A reflection vector
  or cube sample is not a general PBR environment-lighting system.
- **Fog:** The executor computes the title's fixed-function/program fog factor,
  and the Metal combiner receives it with the fog color. Fog is a verified
  part of the Retail shading contract, but it is not volumetric lighting or a
  substitute for scene depth.

## 3. Current Metal renderer

### Surface and resource model

**Verified.** [`bw_metal_backend.m`](../src/gpu/bw_metal_backend.m) owns:

- cached private color and depth/stencil surfaces;
- guest-memory seeding and ordered writeback;
- target switches and render-to-texture ordering;
- clears, partial clears, depth/stencil tests, and blending;
- decoded and cached guest textures;
- a shared vertex upload arena;
- command-buffer and render-encoder reuse;
- render scaling and guest-AA resolve;
- gamma-ramp application; and
- direct `CAMetalLayer` presentation.

The current draw and presentation targets use `BGRA8Unorm`; depth/stencil uses
`Depth32Float_Stencil8`; and the layer is `BGRA8Unorm`. There is no current
linear HDR scene target, sRGB render-target contract, tone mapper, or HDR
Metal presentation path.

At the currently verified 2560x1920 internal size:

- one four-byte-per-pixel attachment is 18.75 MiB; and
- one eight-byte-per-pixel `RGBA16Float` attachment would be 37.5 MiB.

A modest deferred material buffer containing two four-byte attachments and
one eight-byte attachment would therefore add at least 75 MiB before depth,
scene-color copies, shadow maps, history, staging, or driver overhead.
Forward or forward-plus shading is a better first PBR pilot than a full
deferred rewrite at the existing 4x path.

### Apple and Windows feature split

The Apple source list in [`CMakeLists.txt`](../CMakeLists.txt) builds the
Metal backend, GPU audit, gamma, and native LOD support. It does not build the
DX11 VFX renderer, post chain, texture-restoration path, or generated HLSL
vertex/pixel shader helpers.

As a result, these existing Remastered features are currently DX11-only:

- screen-space ambient occlusion;
- bloom;
- sharpening and FXAA;
- high-precision VFX targets and HDR output;
- contour text/HUD filtering;
- texture restoration; and
- the DX11 GPU vertex-program path.

[`bw_settings.c`](../src/bw_settings.c) reports those capabilities as
unsupported on Metal rather than accepting no-op settings.

### Fail-closed behavior

Unsupported texture, combiner, sampler, surface, or render state produces a
sticky renderer error. This is the correct foundation for future work:
unknown state must not be silently treated as a valid modern material.

## 4. What reaches Metal today

| Input or capability | Current availability on Apple Metal | PBR significance |
| --- | --- | --- |
| Projected position and reciprocal W | Yes | Rasterization only; not stable world position |
| Diffuse and specular vertex colors | Yes | Already-combined title outputs; base color and lighting are not separable |
| Fog factor and fog color | Yes | Useful for Retail composition; not light or material data |
| Four texture-coordinate vectors | Yes | Semantics vary by title program and pass |
| Up to four title textures | Yes for supported layouts | Existing art, but no declared albedo/normal/ORM roles |
| Texture-stage and combiner program | Yes | Reproduces Retail; does not identify a physical material |
| Blend/depth/stencil/cull state | Yes | Essential composition state |
| Explicit ocean marker | Yes for the validated pass scope | Precedent for safe semantic classification |
| Object-space position and normal | Present before fixed-function transform, not in `BwXBatch` | Required for robust relighting |
| Fixed-function light records | Present in the executor register mirror, not in `BwXBatch` | Usable for a new semantic FFP contract |
| Model-view/composite matrices | Present in executor state, not in the Metal draw contract | Needed for position/normal reconstruction |
| Raw vertex-program inputs/program/constants | Interface exists, but Metal does not implement `draw_vsh` | Necessary but not sufficient; meanings remain program-specific |
| Stable object/material identity | No general contract | Required for selective replacement and material parameters |
| Tangent basis | No general contract | Required by conventional tangent-space normal maps |
| Normal, roughness, metallic, AO maps | No declared roles | Required for authored PBR materials |
| Scene color before a draw | No shader contract | Required for many refraction and composition effects |
| Shader-readable scene depth | No current contract | Required for SSAO, reconstruction, contact effects, and many post passes |
| Shadow-map identity and light matrix | Not classified | Required to preserve or replace title shadows safely |
| Motion vectors and temporal history | No | Required for temporal effects and stable temporal reconstruction |
| Linear HDR scene buffer and tone mapping | No | Required for energy-consistent PBR and bright-light range |

Metal hardware is not the limiting factor in this table. The missing pieces
are renderer contracts, title semantics, resource ownership, and evidence.

## 5. Improvements possible without replacing the scene renderer

### A. Retail fidelity and performance

These changes preserve the existing lighting model:

1. Measure actual use of fixed-function specular, spot, two-sided, normalize,
   skinning, texture-generation, and material states.
2. Implement required Retail gaps with isolated synthetic tests.
3. Port the DX11-style GPU vertex-program path to Metal, with exact CPU/Metal
   comparison and a bounded pipeline cache.
4. Build Metal pipeline warm-up only from native trace evidence; do not reuse
   the DX11 installer workflow.
5. Measure CPU transform/interpreter cost before treating GPU vertex execution
   as a frame-rate priority.

This work can improve correctness and CPU cost. It should not be marketed as
new lighting.

### B. Metal post-processing

The existing DX11 AO, bloom, sharpen, FXAA, precision, and HDR work provides a
design reference, but not code that is active on Apple Silicon. A Metal port
would need:

- an explicit end-of-3D/per-view boundary before HUD composition;
- shader-readable scene depth;
- scene-color ownership and temporary targets;
- split-screen rectangles and independent per-view history where applicable;
- gamma/linear-color decisions; and
- exact bypass behavior for Original and disabled Remastered settings.

AO can add contact shading and bloom can improve bright effects, but neither
creates material-aware real-time lights. These are useful, lower-risk visual
improvements and should remain described as post-processing.

### C. True per-pixel lighting for fixed-function draws

This is the most bounded route to genuine new real-time lighting.

A new fixed-function semantic batch could preserve:

- object or eye-space position and normal;
- model-view and normal transform;
- the active light records;
- scene ambient and the already-folded Retail material terms;
- the title's textures and coordinates; and
- the exact Retail blend, fog, alpha, depth, and pass state.

Retail would continue to compute the existing per-vertex color. An opt-in
pipeline for an explicitly marked opaque draw family could instead interpolate
position and normal and evaluate the same ambient/diffuse lights per fragment.
A conservative Blinn-Phong-style specular pilot could follow only if the
material and light semantics are proven.

This would reduce Gouraud artifacts and make local lights respond at pixel
resolution. It would still not be PBR because the available light colors are
material-folded and there are no roughness, metallic, tangent, or linear-HDR
contracts.

### D. Selective PBR materials

PBR becomes credible only after a selected material family has explicit:

- base-color ownership and color-space interpretation;
- stable geometric normal and, if needed, tangent basis;
- roughness, metallic/specular, and optional emissive parameters;
- camera and world/eye-space position;
- light type, position/direction, color, range, and intensity;
- shadow ownership;
- reflection or image-based-lighting source;
- linear HDR accumulation and tone mapping; and
- pass identity, fallback, and composition rules.

The first pilot should use a small opaque family with no refraction, no
multi-pass transparency, and no HUD role. Project-owned scalar material
parameters can be supplied by an explicit semantic ID. Texture byte hashes,
addresses, or a coincidental combiner shape must not be the production
classifier.

Existing art can remain in use, but old diffuse textures alone do not contain
authored normal, roughness, or metallic information. Guessed parameters can
produce a stylized optional treatment; they should not be called an asset-
complete PBR conversion.

## 6. Renderer replacement options

| Option | What changes | Real-time lighting/PBR potential | Relative risk | Recommendation |
| --- | --- | --- | --- | --- |
| Complete Retail Metal backend | Fill NV2A gaps and optionally run vertex programs on Metal | None beyond title behavior | Low to medium | Do first where audits show real use |
| Port DX11 post chain | Add depth/color-aware Metal post passes | Better depth cues and highlights, not material lighting | Medium | Good near-term quality work |
| Enhanced fixed-function path | Preserve normals/lights and shade qualified draws per pixel | Real dynamic lighting; limited material model | Medium | Best first lighting pilot |
| Marked-material hybrid PBR | Replace shading for explicit opaque material families | Strong, bounded PBR potential | High | Recommended long-term direction |
| New low-level NV2A renderer | Rebuild backend around the same command stream | Still lacks semantic materials/lights | High | Not justified as the PBR route |
| High-level scene renderer | Extract title objects, cameras, materials, lights, and passes before D3D submission | Full control, modern shadows, PBR, temporal effects | Very high | Consider only after hybrid coverage proves the contracts |

### Why a command-stream rewrite is not enough

The current Metal backend already translates the command stream directly. A
cleaner render graph, pipeline cache, or shader generator could improve
maintainability and performance, but the stream commonly contains the
*result* of title lighting rather than the concepts that produced it.

Automatic rules such as "texture 0 is albedo," "`v2` is the normal," or "this
constant is the sun" will fail across program families, effects, UI, water,
shadow work, and split-screen. A low-level rewrite only moves the point at
which those unsafe guesses are made.

### What a high-level replacement entails

A complete scene renderer would need to identify or intercept:

- every view and camera, including split-screen and transitions;
- world cells, boats, characters, weapons, particles, sky, ocean, effects,
  transparent objects, reflections, shadows, and HUD;
- mesh LOD and skinning inputs;
- material/texture assignments and animation;
- light creation, weather, time-of-day, flashes, and mission-specific state;
- culling, sorting, pass order, render-to-texture dependencies, and visibility
  queries; and
- suppression of the matching Retail draws without suppressing gameplay.

That is closer to replacing Blood Wake's title renderer than replacing a host
API backend. It is possible in stages because static recompilation permits
validated hooks, but it is not a single shader or Metal-port task.

## 7. Recommended staged plan

### Phase 0: representative inventory

Use the existing `BW_GPU_AUDIT` and A/B facilities to collect aggregate,
private evidence for:

1. fixed-function, CPU vertex-program, GPU vertex-program, screen-space, and
   dropped draw proportions;
2. lighting enable, active-light masks/types, specular, two-sided, normalize,
   color-material, skin, and fog states;
3. vertex-program families, input/output masks, and constant-change cadence;
4. color/depth target switches and render-to-texture dependencies;
5. shadow-pass boundaries and receiver draws; and
6. opaque, alpha-tested, blended, water, particle, and UI draw families.

Cover menus, multiple missions, day/night and weather variation, lightning and
explosions, calm/storm water, shadow-heavy scenes, split-screen, and
transitions. Publish only aggregate classifications, never private hashes,
addresses, captures, or generated code.

**Exit gate:** enough evidence to select one explicit opaque pilot family and
to prove which current fidelity gaps affect Blood Wake.

### Phase 1: Retail foundation

1. Fix only audited NV2A lighting/material gaps that the title uses.
2. Add synthetic fixed-function light tests for direction, attenuation,
   material routing, normal transforms, and any implemented specular/spot
   behavior.
3. Decide independently whether a Metal `draw_vsh` path is justified by CPU
   measurements.
4. Add stable pass markers at validated title boundaries for shadow, opaque
   scene, transparent scene, water, and HUD work as each is proven.

**Exit gate:** exact Retail output and explicit fallback remain testable; no
modern path is selected by low-level pattern alone.

### Phase 2: frame-resource contract

Design explicit ownership for:

- shader-readable scene color;
- shader-readable depth, with clear/partial-clear and target-switch rules;
- per-view boundaries and rectangles;
- temporary linear/HDR targets;
- tone mapping and the title's gamma ramp;
- resize/render-scale changes;
- guest readback and render-to-texture ordering; and
- failure recovery without dropped draws.

A Metal port of one low-risk post effect can validate this contract.

**Exit gate:** disabled mode is exact, split-screen is correct, and all
resource transitions have synthetic coverage.

### Phase 3: fixed-function per-pixel pilot

1. Define a borrowed semantic FFP batch carrying pre-transform geometry,
   normals, matrices, and a bounded light snapshot.
2. Explicitly mark one opaque title draw family.
3. Implement a separate Metal pipeline that first matches the Retail
   ambient/diffuse result per pixel.
4. Preserve Retail alpha, fog, blending, depth, shadows, and fallback.
5. Compare same-execution Retail/enhanced crops and GPU duration.

**Exit gate:** a visible, accepted improvement with no draw leakage or
mechanics/visibility change.

### Phase 4: selective PBR pilot

1. Add a project-owned material table keyed by explicit semantic IDs.
2. Convert base color to a documented linear working space.
3. Add roughness and metallic/specular scalar parameters before considering
   replacement texture maps.
4. Add a bounded reflection/IBL source and one proven light family.
5. Render into a linear HDR intermediate and tone map back into the existing
   composition path.
6. Preserve or explicitly replace the selected family's Retail shadowing.

**Exit gate:** the result is stable across lighting/weather/split-screen,
material identity is explicit, and the memory/performance budget is accepted.

### Phase 5: hybrid scene renderer

Expand only after each material/object family has:

- validated extraction and lifetime;
- complete opaque/transparent/shadow/reflection behavior;
- safe Retail draw suppression;
- deterministic fallback; and
- representative visual and performance evidence.

Keep original water, particles, transparencies, and HUD until each has its own
contract. A hybrid can deliver most visible benefit without requiring an
all-or-nothing rewrite.

### Phase 6: full-replacement decision

Consider a complete scene renderer only if the hybrid has already proven
camera, geometry, material, light, shadow, effect, and pass coverage. At that
point the remaining Retail path should be measured as a known set of
subsystems, not estimated from code size.

## 8. Required semantic contracts

The exact C layout is future design work, but responsibilities should be
separated:

- **Draw identity:** versioned pass and material-family IDs emitted at a
  validated title boundary.
- **Geometry:** raw positions, normals, optional tangents/skinning, indices,
  and object/world/view transforms with borrowed lifetime rules.
- **Material:** explicit texture roles, scalar parameters, alpha mode,
  two-sidedness, and color-space declarations.
- **Lighting:** bounded directional/point/spot records in a declared space,
  camera position, global ambient/environment data, and shadow references.
- **Frame resources:** per-view scene color/depth, history, target scale,
  viewport, and transition ownership.
- **Composition:** opaque/transparent order, fog, alpha test, blend/depth
  state, HUD boundary, gamma, and fallback behavior.

Do not overload [`BwXBatch`](../src/toolkit_patches/kernel/bw_backend_ext.h)
with guessed semantics. Keep the exact NV2A batch and the optional semantic
contract distinct so Retail emulation stays auditable.

## 9. Validation requirements

Any optional lighting or renderer change must satisfy:

1. **Retail identity:** Original, disabled Remastered, unmarked draws, and
   fallback use the exact existing path.
2. **Explicit scope:** Only a validated title marker/ID can select replacement
   shading.
3. **No double draw:** A replaced object is emitted once, and fallback restores
   the Retail draw if replacement preparation fails.
4. **State preservation:** Camera, culling, LOD, visibility, split-screen,
   simulation, mission logic, and frame-step state are unchanged.
5. **Composition:** Shadows, fog, water, transparencies, particles, HUD,
   reflections, and render-to-texture consumers retain correct order.
6. **Color correctness:** Texture color space, linear-light work, tone mapping,
   gamma ramp, screenshots, and presentation are explicitly tested.
7. **Representative visuals:** Day/night, weather, lightning, explosions,
   interiors/exteriors where present, boats, buildings, characters, water,
   shadow edges, alpha-tested art, split-screen, and transitions.
8. **Same-execution A/B:** Aligned private crops and pass-isolated diagnostics;
   separate runs are not pixel evidence.
9. **Performance:** CPU and GPU mean/median/p95, pipeline compilation, memory,
   target resolution, device, OS, scene, and run order.
10. **Failure behavior:** Missing semantic data, unsupported state, allocation,
    compilation, and command errors are reported and use a defined Retail
    fallback rather than dropping geometry.
11. **Privacy:** No retail assets, generated code, addresses, private program
    hashes, captures, saves, or logs in commits or reports.

The synthetic capture facility in [GPU_CAPTURE.md](GPU_CAPTURE.md) is not yet
a live-game reference and does not currently capture the complete lighting,
vertex-program, texture, depth, or resource-lifetime state needed for this
validation.

## 10. Open research questions

1. What proportion of representative Blood Wake scene draws use fixed-function
   lighting versus title vertex programs?
2. Which fixed-function light types and unsupported specular, spot, two-sided,
   normalize, skin, texgen, and material states are actually used?
3. Which title vertex-program families perform lighting, and which raw
   attributes carry stable geometric normals or tangents?
4. Where are the safest title boundaries for opaque scene, shadow caster,
   shadow receiver, transparent scene, particles, water, and HUD?
5. How does `ShadowMap_Render` encode maps, light matrices, receiver state, and
   split-screen ownership?
6. Can stable material-family IDs be emitted without texture hashes, private
   addresses, or asset-derived public signatures?
7. Which title light data exists before the XDK folds material terms into NV2A
   light colors?
8. Does moving vertex programs to Metal materially reduce CPU frame cost at
   the current 4x path?
9. What scene-color/depth transition model preserves target switches, partial
   clears, guest seeding/readback, and render-to-texture behavior?
10. Which one opaque fixed-function material family offers a visible
    per-pixel-lighting improvement without multi-pass or transparency risk?
11. Can Retail shadowing be reused by a selected PBR material, or must the
    shadow path be replaced with it?
12. What linear working space and tone mapper preserve the intended title
    palette and gamma ramp?
13. How much additional attachment memory and bandwidth is acceptable at
    1x, native, and the currently verified 4x internal scale?
14. After a hybrid pilot, which remaining systems would still make a complete
    renderer replacement materially better than continued selective upgrades?

## 11. Code and documentation map

- [`src/toolkit_patches/kernel/nv2a_pb_scan.c`](../src/toolkit_patches/kernel/nv2a_pb_scan.c):
  guest push-buffer traversal.
- [`src/toolkit_patches/kernel/nv2a_pb_exec.c`](../src/toolkit_patches/kernel/nv2a_pb_exec.c):
  NV2A state, fixed-function lighting, CPU vertex-program dispatch, clipping,
  batching, and backend handoff.
- [`src/toolkit_patches/kernel/nv2a_vsh_interp.c`](../src/toolkit_patches/kernel/nv2a_vsh_interp.c):
  CPU NV2A vertex-program interpreter.
- [`src/toolkit_patches/kernel/bw_backend_ext.h`](../src/toolkit_patches/kernel/bw_backend_ext.h):
  exact extended pixel contract and optional raw vertex-program batch.
- [`src/gpu/bw_metal_backend.m`](../src/gpu/bw_metal_backend.m): Metal
  surfaces, textures, shaders, pipelines, scaling, gamma, and presentation.
- [`src/gpu/bw_metal_backend.h`](../src/gpu/bw_metal_backend.h): public Metal
  lifecycle, capabilities, and explicit limits.
- [`src/gpu/bw_gpu_audit.c`](../src/gpu/bw_gpu_audit.c): aggregate NV2A draw,
  light, material, vertex-program, texture, combiner, target, and gap audit.
- [`src/gpu/bw_d3d11.c`](../src/gpu/bw_d3d11.c): Windows reference backend,
  GPU vertex-program path, render-to-texture behavior, and VFX integration.
- [`src/gpu/bw_vsh_hlsl.c`](../src/gpu/bw_vsh_hlsl.c): DX11 NV2A
  vertex-program shader generation reference.
- [`src/gpu/bw_rc_hlsl.c`](../src/gpu/bw_rc_hlsl.c): DX11 texture-stage and
  register-combiner generation reference.
- [`src/gpu/bw_vfx.c`](../src/gpu/bw_vfx.c) and
  [`src/gpu/bw_vfx_post.c`](../src/gpu/bw_vfx_post.c): DX11-only Remastered
  resource and post-processing reference.
- [`src/recomp_manual.c`](../src/recomp_manual.c): known title render/cull/LOD
  hook boundaries.
- [`src/bw_settings.c`](../src/bw_settings.c): renderer capability reporting
  and user-facing quality settings.
- [Water rendering](WATER_RENDERING.md): the existing explicit material-pass
  precedent and its missing-input analysis.
- [Metal quality](METAL_QUALITY.md): current scaling/filtering behavior and
  MSAA evidence boundary.
- [GPU capture](GPU_CAPTURE.md): synthetic fixture scope and live-capture
  limitations.
