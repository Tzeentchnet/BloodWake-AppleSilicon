# Modern water TODO

Updated 2026-11-05.

This is the implementation tracker for [the modern water plan](NEWWATER.md).
It records dependencies, completion evidence, and the difference between
protocol groundwork and runtime-validated behavior.

## Status legend

- `[x]` Complete and validated.
- `[ ]` Ready when its dependencies are complete.
- `IN PROGRESS` Implemented, but the listed exit evidence is incomplete.
- `PAUSED` Preserved and ready to resume after the current higher-priority
  runtime-efficiency work.
- `BLOCKED` Requires the listed research or contract first.
- `EXPERIMENT` May be removed if it does not pass its exit gate.

## Current implementation boundary

Completed in the first two implementation slices:

- [x] Added a versioned water event encoding for view begin/end and lit,
  reflection, and foam draw events.
- [x] Kept the old version-1 ocean marker as an accepted legacy lit event.
- [x] Replaced the initial `Ocean_FillVertices` lit one-shot with a complete
  lit pass run while retaining exact legacy-wire compatibility in the
  executor.
- [x] Added a versioned pass/view record to extended executor batches.
- [x] Preserved the existing `BW_X_DRAW_MARKER_OCEAN` compatibility marker.
- [x] Kept the current Realistic shader restricted to typed lit draws.
- [x] Added synthetic coverage for all three pass types, view inheritance,
  legacy compatibility, unknown versions, mismatched views, pending-draw view
  termination, and fail-closed recovery.
- [x] Added persistent pass-run begin/end events so one validated title
  boundary can classify every tile draw in a pass.
- [x] Located the title's per-view ocean-render loop and its one-based current
  view ordinal; the wrapper converts it to the protocol's zero-based view ID.
- [x] Wrapped the complete ocean render with view begin/end events.
- [x] Wrapped the pass-2 reflection and pass-3 foam submission functions with
  persistent pass runs.
- [x] Kept Original and every unvalidated call context marker-free while
  always executing the generated title body.
- [x] Added aggregate per-pass scope and per-view begin/end diagnostics.
- [x] Verified unchanged Retail and current Realistic rendering tests.

Runtime validation status:

- [x] A freshly prepared private one-player Battle run reached sustained
  gameplay and demonstrated nonzero lit/reflection/foam scopes plus balanced
  view-0 begin/end events.
- [x] That run exposed 604 false rejections from valid empty `BEGIN/END`
  scopes inside persistent reflection/foam runs. The executor now
  distinguishes persistent-run scopes from one-shot draws, accepts the empty
  run scopes, and retains strict one-shot rejection; 324/324 Metal tests pass.
- [x] A post-fix bounded one-player Battle run recorded 186,893 lit, 186,893
  reflection, and 186,893 foam scopes; 2,915 balanced view-0 pairs; 560,679
  marked batch scopes; zero rejection; and 552,477 fingerprinted batches with
  zero overflow or scope mismatch.
- [x] The same run activated Realistic water with no unsupported-pass fallback
  and activated Metal private reflection depth.
- Split-screen/view-1 validation remains deferred and does not block
  single-player visual work.
- Later modern-water visual phases are paused while base runtime efficiency is
  prioritized; their dependencies and acceptance gates remain below.

Validation completed for this implementation:

- Metal backend suite: 333/333 tests passed.
- Hook portability suite: 10/10 tests passed.
- Focused marker, grouped-pass, Retail-water, Realistic-water, private-depth,
  scale, resize, target-switch, Original/disabled, and fallback tests passed.
- All eight private-depth sanitizer cases passed.
- The regenerated executor and presenter patches apply cleanly to the pinned
  toolkit source.

## Critical path

| ID | Status | Task | Depends on | Exit condition |
| --- | --- | --- | --- | --- |
| NW-000 | `[x]` | Write the source-grounded architecture plan | None | `NEWWATER.md` exists and links resolve |
| NW-001 | `[x]` | Define typed water marker events | NW-000 | Versioned begin/end/lit/reflection/foam encoding exists |
| NW-002 | `[x]` | Propagate pass/view identity through the executor | NW-001 | Extended batches receive validated semantic data |
| NW-003 | `[x]` | Preserve legacy marker and shader behavior | NW-001 | Legacy lit events and current Realistic tests pass |
| NW-004 | `[x]` | Locate validated water-view boundaries | NW-002 | The title view loop and wrapper prove balanced per-view ordering |
| NW-005 | `[x]` | Locate validated reflection-pass boundary | NW-002 | Pass 2 is bracketed at its title submission function |
| NW-006 | `[x]` | Locate validated foam-pass boundary | NW-002 | Pass 3 is bracketed at its title submission function |
| NW-007 | `[x]` | Emit runtime lit/reflection/foam/view events | NW-004, NW-005, NW-006 | Representative gameplay has balanced view 0, all passes, zero rejection, and clean fingerprints |
| NW-008 | `PAUSED` | Build same-execution Retail/modern capture | NW-007 | Pixel-aligned full-water A/B pairs are produced |
| NW-100 | `[x]` | Port `OCEAN_REFLECT_SINGLE` to Metal | NW-005, NW-007 | Private-depth behavior matches the DX11 design and activates in gameplay |
| NW-200 | `BLOCKED` | Add water frame-resource contract | NW-004, NW-007 | Scene color/depth/water depth survive all ownership tests |
| NW-300 | `BLOCKED` | Implement Modern Surface v1 | NW-200 | Obvious accepted improvement within budget |
| NW-400 | `BLOCKED` | Implement optical volume and SSR | NW-300 | Stable depth/refraction/absorption with safe fallback |
| NW-500 | `BLOCKED` | Implement modern foam and wakes | NW-006, NW-300 | Authored wakes remain correct and added foam is stable |
| NW-600 | `BLOCKED` | Evaluate geometry refinement | NW-300, NW-400, NW-500 | Captures prove geometry is the remaining quality limit |
| NW-700 | `BLOCKED` | Productize settings and quality tiers | NW-300 | Retail/Enhanced/Modern behavior and budgets are accepted |

## Phase 0 - Evidence and semantic boundaries

### Marker and executor foundation

- [x] Reserve a distinct water-event operation in
  [`src/bw_draw_marker.h`](src/bw_draw_marker.h).
- [x] Encode protocol version, event type, and four-view payload in one marker
  byte.
- [x] Accept the old ocean-draw marker only at its exact legacy version.
- [x] Coalesce repeated identical pass markers before one draw.
- [x] Apply persistent pass runs to every draw until a matching run end.
- [x] Reject unknown versions/events, pass payload misuse, nested views,
  nested or mismatched pass runs, one-shot events inside pass runs, mismatched
  view ends, view ends with pending draws or active runs, markers inside
  draws, conflicting markers, frame leakage, and marked draws without an
  extended batch.
- [x] Carry semantic version, pass, and inherited view in `BwXWaterDraw`.
- [x] Keep an unknown-view value for the validated lit hook until view
  boundaries are proven.
- [x] Keep metadata fingerprints as evidence, never as classifiers.

### Boundary implementation and runtime gate

- [x] Prove that the title's ocean render is called once inside the split-view
  loop after the current one-based view ordinal is stored.
- [x] Bracket the complete ocean render so no-water views and internal early
  exits still produce balanced begin/end events.
- [x] Identify the title-level function immediately responsible for pass 2
  tile submissions.
- [x] Identify the title-level function immediately responsible for pass 3
  tile submissions.
- [x] Confirm pass 2 and pass 3 reuse geometry without another
  `Ocean_FillVertices` call.
- [x] Add source-boundary tests for Remastered emission, Original passthrough,
  disabled passes, empty tile lists, invalid views, event order, and guest
  register/stack preservation.
- [x] Repeat a freshly prepared bounded private diagnostic after the
  persistent-empty-scope correction and verify zero marker conflicts,
  unmatched views, frame carry-over, or cross-view leakage.
- [x] Record nonzero lit/reflection/foam scope counts and balanced view counts
  in sustained single-player gameplay.
- [ ] Validate view 1 in split-screen as a later non-blocking protocol check.
- [x] Publish only aggregate counts; keep addresses, hashes, captures, and
  retail data private.

### Same-execution evidence

- [ ] Design dual private targets fed by the same frame and semantic batches.
- [ ] Capture Retail and candidate output without advancing simulation twice.
- [ ] Add pass-isolated lit/reflection/foam outputs.
- [ ] Add difference images and short motion sequences.
- [ ] Cover calm/storm, day/night, fog, shore, wake-heavy, grazing-angle, and
  split-screen scenes.

## Phase 1 - Pass-aware parity

- [x] NW-100: Port the DX11 private-depth distant-water-line fix to Metal.
- [x] Allocate private reflection depth without changing authoritative scene
  depth.
- [x] Prime it from lit geometry and test reflection against the nearest water
  layer.
- [x] Restore the title depth attachment for non-reflection work.
- [x] Reuse one scene-depth snapshot across grouped lit/reflection/foam work in
  one validated view.
- [x] Add target-switch, fixed-scale, live-resize, grouped-pass,
  Original/disabled, and allocation-failure tests.
- [ ] Add a focused nonzero title polygon-offset parity case.
- [ ] Add pass-2-only diagnostic shaping after pass 2 has explicit identity.
- [ ] Measure eligible water mip/filter states before tuning anisotropy.
- [ ] Keep all enhancement paths default-off until representative review.

## Phase 2 - Semantic geometry and frame resources

### Water semantic contract

- [ ] Audit raw vertex inputs and transforms for explicitly marked water.
- [ ] Define `BwWaterView` with view ID, rectangle, camera matrices, depth
  reconstruction, fog, and validated lighting.
- [ ] Define `BwWaterDraw` with explicit pass, position, normal, projected
  coverage, title texture inputs, and composition state.
- [ ] Keep lifetimes synchronous and borrowed unless the backend copies them.
- [ ] Reject incomplete or unsupported semantic data for the whole modern
  view.

### Metal resource ownership

- [ ] Capture scene color before the first water draw in each view.
- [ ] Make opaque depth safely shader-readable or copy it to owned
  `R32Float`.
- [ ] Add nearest-water depth.
- [ ] Add linear `RGBA16Float` water composition and coverage.
- [ ] Handle private scale, guest AA, partial clears, target switches,
  render-to-texture, writeback, resize, screenshots, and backend stop.
- [ ] Isolate split-screen rectangles and clear reused regions.
- [ ] Add failure injection at every allocation/pipeline/state transition.
- [ ] Prove no per-draw readback or command-buffer wait.

### Atomic fallback

- [ ] Continue rendering exact Retail water to the title target.
- [ ] Shadow-render Modern water into private targets.
- [ ] Composite only after every required pass succeeds.
- [ ] Skip the composite and retain Retail after any modern failure.
- [ ] Test failure before water, after lit, after reflection, during foam, and
  during final composition.

## Phase 3 - Modern Surface v1

- [ ] Add a bounded linear working space for water.
- [ ] Add world-anchored multi-scale micro normals.
- [ ] Runtime-prefilter the title sky cube into roughness mips.
- [ ] Add water Fresnel and roughness-aware environment reflection.
- [ ] Extract and validate sun/moon direction before adding a direct glint.
- [ ] Match the title's fog and presentation gamma ownership.
- [ ] Preserve Retail wake foam until the modern foam path is ready.
- [ ] Add debug views for normals, Fresnel, roughness, environment mip,
  coverage, fog, and luminance.
- [ ] Pass same-execution calm/storm/day/night/fog/split-screen review.

## Phase 4 - Optical volume and nearby reflection

- [ ] Reconstruct linear opaque and water-surface depth.
- [ ] Reject invalid, negative, stale, and cross-view thickness.
- [ ] Add bounded depth-aware refraction.
- [ ] Add Beer-Lambert absorption and low-frequency in-scattering.
- [ ] Add art-directed shallow/deep coefficients by validated weather state.
- [ ] Build a per-view depth pyramid.
- [ ] Add half-resolution SSR with confidence and sky-cube fallback.
- [ ] Test screen edges, disocclusion, cleared depth, split boundaries, and
  target transitions.
- [ ] Complete gameplay visibility review.

## Phase 5 - Foam and interaction polish

- [ ] Reproduce pass-3 wake mask and vertex alpha in the linear composite.
- [ ] Preserve the distant wake-mip fix.
- [ ] Add world-stable breakup without changing wake extent or lifetime.
- [ ] Add conservative slope/curvature crest foam.
- [ ] Add depth-gradient shoreline/object intersection foam.
- [ ] Tune calm, storm, day, and night foam independently.
- [ ] Evaluate render-only crest spray only if foam is insufficient.
- [ ] Verify boats, hazards, projectiles, and shore boundaries remain clear.

## Phase 6 - Optional geometry refinement

- [ ] Confirm with accepted Phase 3-5 captures that polygonal geometry is the
  remaining limit.
- [ ] Expose validated 5x5 render-patch positions, heights, and normals.
- [ ] Prototype Metal tessellation or compute subdivision.
- [ ] Add patch adjacency/stitch data before variable edge factors.
- [ ] Keep title culling, tile selection, and broad wave shape.
- [ ] Start with zero added vertical displacement.
- [ ] Reject the experiment if cracks, boat separation, or weak normal-play
  gains remain.

## Phase 7 - Productization

- [ ] Add explicit Retail, Enhanced, and Modern choices.
- [ ] Preserve the existing `realistic` value through an explicit compatibility
  rule and visible migration message.
- [ ] Add Balanced, High, and Ultra feature budgets.
- [ ] Report resource/pipeline failure and fall back to Retail for the whole
  view.
- [ ] Do not silently downgrade quality.
- [ ] Measure GPU mean/median/p95, CPU encoding time, warm-up, and peak memory
  at 1x, 1080p, 1440p, 2560x1920, and 4K where supported.
- [ ] Complete single-player and worst-case split-screen review.
- [ ] Update settings, quality, roadmap, validation, and user documentation.

## Tracking rules

1. Check off a task only after its exit condition is verified.
2. Record synthetic evidence separately from runtime evidence.
3. Do not call a pass identified until a validated title boundary emits it.
4. Do not call a resource implemented until failure and ownership tests pass.
5. Do not call a visual milestone accepted until same-execution motion review
   and performance measurements pass.
6. Preserve exact Retail fallback at every phase.
