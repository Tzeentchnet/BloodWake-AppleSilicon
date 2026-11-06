# Apple Silicon handoff

Status updated 2026-11-06 after the native F1 settings integration, gameplay
mouse input, live LOD, Metal anisotropic filtering, optional realistic-water
pipeline and same-scene GPU benchmark, dedicated water-system research,
upstream parity audit, deterministic menu-to-gameplay diagnostics, and bounded
audio validation, plus GPU-private feedback, multi-target Metal submission
ownership, and the Native 4x/60 default-quality performance matrix.

## Current status

The current arm64 development build boots a retail North American copy of
Blood Wake, reaches the game, and is user-confirmed for menu navigation and
interactive play. The native path uses the project Metal renderer and the
game-specific macOS keyboard bridge.

A coordinated 640x480 retail run sustained the original title's 30 FPS cap:

- Three active 300-frame windows measured 29.99, 29.96, and 30.00 FPS using
  monotonic wall time.
- Median frame times were 32.73-33.27 ms.
- p95 frame times were 45.59-45.76 ms.
- The worst active-window interval was 58.45 ms.
- The run completed 607 flips, 319,133 draws, and 44,934,511 vertices without
  a sticky Metal or command-buffer error.
- The title exited normally through `HalReturnToFirmware` routine 2.

The first measured window included a 2.71-second loading stall and averaged
23.62 FPS. The older `[GPU] ... fps` line uses C `clock()` CPU time and is not
accepted as wall-clock evidence.

Since that measured 640x480 run, the Metal path has gained direct
`CAMetalLayer` presentation and private, independently scaled colour and depth
targets. A real-window run with automatic scaling selected a 4x internal
target (2560x1920); the user reported no visual issues during gameplay, and
the process completed without a sticky Metal or command-buffer error. This is
hands-on confirmation rather than a formal reference comparison. Frame time
and memory were not measured, so the older 640x480 numbers are not
scaled-rendering measurements.

The later seeded Battle investigation now provides that timing evidence.
Direct display and explicit guest publication are separate, mutable resources
belong to one of three bounded submissions, exact XRGB anti-aliased feedback
stays GPU-private while guest bytes are unchanged, and safe target switches
share one ordered command. Immutable NV2A combiner routing remains specialized
with Metal function constants. Two exact Native/auto, match-display boots
selected 2560x1920 and produced post-warm-up windows at 59.98-60.01 FPS,
16.66-16.68 ms wall medians, and 14.18-14.84 ms GPU medians. Two fixed-4x,
cap-120 boots independently produced consecutive windows above 60 FPS.
Current quality defaults are retained; representative sustained 4x/60 is now
claimed. Cold 296-330 ms shader/workload warm-up stalls and broader mission
pacing remain explicit follow-up work. Validation passes 337/337 Metal
backend tests, seven focused ownership/feedback cases under ASan/UBSan, and
62/62 macOS presenter tests.

That 4x run had no sound because it predated the native APU dispatch path; it
was not a Metal presentation or scaling regression. Native audio is now
default-on through guarded same-thread deferred APU MMIO dispatch and the
existing SDL output backend. Dispatch, configuration, startup failure, and
pre-guest shutdown are regression-tested, but audible retail output and sound
quality have not yet been confirmed by the user.

A subsequent 20-second bounded retail boot initialized the CoreAudio driver at
48 kHz stereo, started the title-controlled APU and both DSP programs, produced
periodic DSP output reports, and delivered APU interrupts. It remained active
until the intentional run limit without a deferred-dispatch or Metal error.
One SDL underrun was logged. This confirms runtime integration, not that sound
was audible or correct.

The current source also adds a schema-driven AppKit panel opened by F1,
validated atomic settings persistence, and explicit live/frame/restart/
unsupported capability metadata. Remastered LOD is live from 1x through 8x.
Metal applies 1x/4x/8x/16x anisotropy at a flip boundary only to linear,
mipmapped stages. Optional gameplay mouse input maps synchronized relative
motion to the steering or aim stick and releases capture on Escape, F1, focus
loss, hide, or minimize. These additions are regression-tested and the native
Release target builds.

The presenter suite now includes a real AppKit integration target for F1
down/repeat/up, main-thread panel/controller lifecycle, input and mouse
release, capability reasons, live and restart-pending settings, isolated
atomic save, and visible error sheets. The real-window case skips explicitly
when WindowServer is unavailable.

A post-change 30-second isolated Original-edition retail boot reached its
intentional run limit with direct Metal attached at 2560x1920 and native
audio/DSP active. Its 25-second GPU audit recorded 1,490 flips, 4,503 draws,
1,252,771 vertices, and 1,490 clears; no sticky Metal/command-buffer error was
reported, and the prepared tree, toolkit, and executable remained unchanged.
This proves current-build boot integration only. It did not exercise the F1
panel, Remastered LOD/anisotropy, mouse capture/feel, or audible output.

A later diagnostic-only Remastered flow combined a built-in poll-epoch
menu-to-gameplay script with a silent headless audio sink that preserves APU
timing without CoreAudio. Three boots completed, two validated relaunches were
followed, and the run stopped at its configured boot limit. It aggregated
12,670 ocean markers into 154 clean scopes and 297 fingerprinted batches
across 10 aggregate hashes, with zero marker rejection, table overflow, or
within-scope mismatch. Audio aggregates showed nonzero signal in both channels
without clipping, overflow, or internal audit failure.
This is deterministic technical evidence, not audible or representative
visual validation; private hashes, logs, samples, and retail data remain
uncommitted.

Remastered now has an experimental, default-off realistic-water choice in F1.
Retail retains the existing fragment path. Realistic selects a separate Metal
pipeline only for explicitly marked batches with the validated water-stage
shape, decodes the existing normal/view basis, applies restrained
Fresnel-weighted reflection/atmosphere shaping, and preserves retail alpha.
Unmarked or unsupported passes remain Retail.

A fresh private four-boot scripted run explicitly selected Realistic. It
recorded 40,790 markers in 499 clean scopes and 602 fingerprinted batches
across 16 aggregate hashes. The optional fragment pipeline activated in two
boots; no unsupported-pass fallback, marker rejection, fingerprint
overflow/mismatch, realistic-pipeline failure, or Metal runtime error was
observed. This proves runtime activation, not improved visual quality or
acceptable performance. All private evidence remains uncommitted.

A subsequent diagnostic-only run started in Retail and measured four
Retail/Realistic/Realistic/Retail blocks after reaching supported marked-ocean
rendering. Each block discarded 10 warm-up frames and recorded 50 frames. Two
of four boots independently completed 100 Retail and 100 Realistic samples.
Across those completions, Realistic increased water-containing Metal
command-buffer GPU duration by 4.548%-4.822% mean, 1.865%-3.570% median, and
7.787%-10.092% p95. The four-boot aggregate recorded 38,330 markers, 467 clean
scopes, and 604 fingerprinted batches across 17 aggregate hashes, with no
marker rejection, fingerprint overflow/mismatch, unsupported-pass fallback,
or Metal runtime error. These are whole command-buffer timings in the
exercised scene, not isolated shader cost or a visual-quality approval.

Initial hands-on review found the optional treatment almost identical to
Retail. That is consistent with its intentionally low-strength, pass-1-only
design: the title's later cube-reflection and wake-foam passes remain Retail.
Separate bounded capture runs completed for both presets, but scene/camera
phase drift means they are not pixel-aligned A/B evidence. The complete
simulation/render/pass/input analysis and mechanics-safe follow-up plan is in
[WATER_RENDERING.md](WATER_RENDERING.md).

## Required opt-ins

The currently verified path requires:

- A build containing `BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON`
- `BW_MACOS_KEYBOARD_HOOK=1` at runtime
- `BW_METAL_QUINCUNX_APPROX=1` at runtime

Gameplay mouse is optional and additionally requires `BW_MOUSE=1`. F1 itself
requires no extra runtime switch in the native build.

Realistic water is also optional. Select it in Remastered through F1 or set
`BW_METAL_WATER=realistic`; `retail` is the strict default. Other values and
Realistic in Original are rejected. The diagnostic runner exposes the same
choice through `--water-preset`.

The runner's `--water-benchmark` mode is diagnostic-only and additionally
requires Remastered, `--water-preset retail`, and `--gpu-audit`. It temporarily
owns the active preset, restores Retail on completion or failure, and rejects
missing or malformed aggregate results.

The quincunx path is a narrowly scoped legacy Gaussian approximation, not
verified NV2A filtering. Unsupported renderer states continue to fail
explicitly.

Native audio requires no opt-in. `BW_MACOS_AUDIO=0` disables it;
`BW_MACOS_AUDIO=1` enables it explicitly, and other values are rejected. Do
not set `RECOMP_AC97_READY` directly, even to `0`; the presence-based switch
is managed internally. Guest USB remains disabled, and `RECOMP_USB` is
rejected even when set to `0`.

## Detailed records

- [WORK_COMPLETED.md](WORK_COMPLETED.md) records implemented renderer, runtime,
  input, diagnostic, and device-research work.
- [VALIDATION.md](VALIDATION.md) records retail performance, hands-on
  confirmation, regression coverage, integrity checks, and test commands.
- [BUILDING.md](BUILDING.md) contains build, run, and isolated diagnostic
  instructions.
- [NATIVE_FEATURE_ROADMAP.md](NATIVE_FEATURE_ROADMAP.md) records the F1,
  mouse-menu, mod-editor, quality, upstream, and realistic-water work plan.
- [METAL_QUALITY.md](METAL_QUALITY.md) records native LOD/filtering behavior,
  the realistic-water GPU benchmark, and the current MSAA no-go gate.
- [UPSTREAM_BASELINE.md](UPSTREAM_BASELINE.md) records the 42-fix baseline and
  the classified DX11-only upstream commits.

## Remaining limitations

- Formal visual comparison with trustworthy reference frames is outstanding.
- Direct presentation and 4x scaled rendering have hands-on visual
  confirmation and controlled Battle frame timing; formal reference imagery
  and a complete memory-budget report remain outstanding.
- Quincunx filtering is approximate and not hardware-verified.
- Exact-default post-warm-up windows reach 59.98-60.01 FPS with
  17.06-17.46 ms wall p95. Cold startup still contains 296-330 ms
  shader/workload warm-up stalls; broader mission and long-duration pacing
  coverage remains outstanding.
- Keyboard feedback, reconnect, and the complete device lifecycle remain
  experimental. Synthetic focus-loss and mouse-capture release are covered.
- Native point-and-click menu hitboxes are not implemented; directional
  controller/keyboard navigation remains the fallback.
- Ship/weapon stat profiles and the standalone editor are blocked until local
  instrumentation proves named fields, ranges, definition ownership, and
  lifetime. No retail files are modified.
- Metal MSAA remains unsupported pending controlled visual/GPU-time/memory
  evidence. The F1 schema reports that limitation instead of accepting a
  no-op.
- The current Metal backend is a low-level NV2A renderer: fixed-function
  lighting and title vertex programs run on the CPU, and Metal receives their
  projected color/fog/texture outputs rather than a semantic scene, material,
  light, or world-space geometry contract. The staged investigation and
  recommended hybrid path are in
  [LIGHTING_RENDERING.md](LIGHTING_RENDERING.md); representative light,
  material, shadow, and vertex-program classification is still required
  before per-pixel lighting or PBR work.
- Ocean draws carry an explicit, false-positive-safe game-to-executor marker,
  and the default-off realistic preset is implemented and runtime-activated.
  Retail water remains the exact default/fallback. The current marker and
  optional pipeline cover the lit/base pass only; later reflection and foam
  remain Retail. Representative calm/storm, lighting, shore, split-screen,
  reflection, and wake A/B review remains open. The current automated scene
  has a completed same-scene GPU benchmark and a near-Retail hands-on
  assessment, but no aligned visual evidence or promotion threshold, so the
  preset is still experimental.
- The shared registry includes the distant-water-line fix, but its
  private-depth implementation is currently DX11-only. Metal must port it or
  report it as unavailable before behavioral parity can be claimed.
- Physical controllers, guest USB, audible/sustained native audio, saves, FMV,
  other editions, and broad mission coverage are not validated Apple Silicon
  features.
- The bounded audio boot logged one SDL underrun; latency, underrun behavior,
  channel balance, and output quality need hands-on measurement.
- The headless audio audit retains no samples and cannot prove audible output;
  its analytical queue observed scheduling underflows, so it is signal/timing
  instrumentation rather than a device-quality result.
- Bounded renderer ownership is implemented for tracked Metal color surfaces
  and three submission-owned resource sets. Exact tracked XRGB feedback and
  up to eight target switches remain ordered within that contract. General
  guest aliases, DMA lifetime, and safe in-process device shutdown outside
  that contract are not solved.
- Windows behavior after the Apple Silicon changes has not been revalidated.

## Next priorities

### 1. Preserve the playable path

- Keep the user-confirmed menu and in-game path covered by repeatable retail
  diagnostics.
- Compare private menu and gameplay frames against a trustworthy reference.
- Continue representative active-gameplay sampling beyond the validated Battle
  seed, reporting FPS, median, p95, maximum stalls, resolution, and enabled
  approximations.
- Collect approved direct package-power, temperature, sustained-clock, and
  memory evidence; `pmset` reported no warning after the final matrix but
  cannot provide energy-per-frame data.
- Investigate remaining cross-scene frame-pacing jitter without weakening
  correctness or silently skipping unsupported work.
- Investigate asynchronous or trace-derived specialized-pipeline warm-up so
  cold seeded launches no longer contain 296-330 ms compilation/loading
  stalls; keep cold and steady-state evidence separate.

### 2. Validate the new runtime controls

- Exercise F1 open/close, failed saves, live LOD/filtering, restart-pending
  values, and gameplay mouse capture through menus, gameplay, pause, mission
  transitions, resize, fullscreen, and focus loss.
- Tune mouse sensitivity/response from hands-on play without changing the
  default-off policy.
- Capture oblique water/cliff comparisons and GPU timing for each anisotropy
  choice.

### 3. Complete evidence-gated roadmap research

- Inventory menu state and control rectangles before adding point-and-click
  activation; never infer click targets from framebuffer colour alone.
- Inventory ship/weapon definition construction and ownership locally before
  defining a profile schema or editor. Keep every retail-derived export
  private.
- Follow [WATER_RENDERING.md](WATER_RENDERING.md): explicitly mark all three
  ocean passes, use same-execution aligned A/B capture, and isolate lit,
  reflection, and foam contributions before tuning. Port or explicitly
  disable the DX11-only distant-water-line fix on Metal. Repeat the controlled
  GPU benchmark after any shader change before reconsidering the
  experimental/default-off status.
- Run the MSAA matrix in [METAL_QUALITY.md](METAL_QUALITY.md); keep the setting
  unavailable if the measured benefit does not justify it.

### 4. Complete input, audio, and device lifecycles

- Obtain hands-on confirmation of audible output, channel balance, latency,
  underruns, and sustained gameplay with the new APU path.
- Preserve process lifetime after guest entry; complete DMA ownership,
  IRQ/DPC retirement, and callback quiescence before attempting in-process
  APU teardown or enabling OHCI.
- Add physical-controller validation only after the remaining USB contracts
  are safe.

### 5. Broaden product coverage

- Validate saves across a fresh process.
- Determine whether representative missions require FMV and implement or
  document the required decoder path.
- Validate additional missions and editions without treating general Xbox
  compatibility as a project requirement.
- Re-run focused sanitizers, Metal validation, and Windows checks for each
  coordinated release candidate.

Keep disc images, unpacked assets, generated code, binaries, saves, diagnostic
logs, and captures private. Never replace a verified prepared tree merely to
rebuild it; use the separate build workflow in [BUILDING.md](BUILDING.md).
