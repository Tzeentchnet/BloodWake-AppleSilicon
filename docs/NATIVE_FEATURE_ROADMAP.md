# Native feature roadmap

This roadmap records the investigation and implementation status for native
runtime settings, mouse input, data-driven mods, renderer quality, upstream
fixes, and water rendering. Retail files, generated code, captures, and
locally exported definition data remain private and must not be committed.

## Status

| Area | Current status | Next evidence or implementation boundary |
| --- | --- | --- |
| F1 settings | Implemented and integration-tested | Add a title-state pause hook only after menus, gameplay, cutscenes, and transitions can be distinguished safely. |
| Gameplay mouse | Implemented | Validate sensitivity and steering/aim choices in sustained gameplay. |
| Point-and-click menus | Blocked on menu UI mapping | Identify menu state and selectable rectangles, then map letterboxed drawable coordinates into the 640x480 guest UI. |
| Ship/weapon profiles | Research blocked on field ownership | Instrument definition construction locally and prove named fields, units, ranges, aliases, and lifetimes before writing any override. |
| Standalone mod editor | Planned after the shared schema | Build only after the runtime parser can validate and apply a useful profile with synthetic fixtures. |
| Adjustable LOD | Implemented | Validate 1x-8x visually in both editions; Original remains fixed at 1x. |
| Metal filtering | Implemented | Collect oblique water/cliff comparisons and GPU timings for 1x/4x/8x/16x. |
| Metal MSAA | No-go; synthetic smoke complete | Apple M5 passes 2x/4x/8x synthetic color/depth/resolve, but game-scene attachment ownership and the controlled visual/GPU-time/memory matrix are still required. |
| Upstream fixes | Registry/source parity verified; one Metal water-backend gap identified | Re-run the offline parity checker whenever upstream changes, and port or explicitly disable the DX11-only distant-water-line fix on Metal. |
| Realistic water | Optional preset, runtime activation, GPU benchmark, and system research complete; hands-on result is near-Retail | Mark and isolate all three ocean passes, build same-execution A/B capture, then decide whether pass-1 or pass-2 shader work is worthwhile. |

## F1 runtime settings

F1 is intercepted on the presenter main thread before the guest input mapper.
The matching key-up is consumed, repeats do not reopen the panel, and opening
the panel releases held guest input and relative mouse capture. A floating
AppKit panel is generated from the shared C settings schema.

The schema now reports platform/renderer availability and one of these apply
modes:

- `live`: a subsystem-owned callback applies a validated value atomically;
- `frame`: an immutable renderer choice is selected at a flip boundary;
- `restart`: the panel saves the setting and marks it pending;
- `unsupported`: the control is disabled with a reason.

The panel uses the shared strict validator and atomic INI writer. It reports
read, validation, apply, and save failures. It does not force a pause: no
validated title-state hook currently distinguishes every state in which
synthesizing Start is safe, so the panel explicitly says the game continues.

The presenter integration target exercises F1 down/repeat/up through the real
presenter path, main-thread AppKit panel/controller lifecycle, guest-input and
relative-mouse release, capability reasons, live LOD, frame-boundary
anisotropy and water-preset selection, restart-pending state, isolated atomic
save, and visible apply/save errors. The real-window case skips only when
WindowServer is unavailable.

## Mouse

The SDL input wrapper now tracks synchronized relative motion, focus, capture,
and configuration. Holding or toggling the right mouse button enters relative
mode; Escape, F1, focus loss, hiding, or minimizing releases it and clears
pending motion. Sensitivity, Y inversion, linear/accelerated response,
steering/aim stick, and hold/toggle capture are live settings.

The game-specific input bridge consumes each relative delta once, clamps it to
the signed Xbox stick range, and merges the larger-magnitude axis with existing
keyboard/controller input. Mouse support requires the fingerprint-checked
macOS input-hook build and `BW_MACOS_KEYBOARD_HOOK=1`.

The native cursor remains visible while uncaptured. True point-and-click menus
are not implemented. Blood Wake's controller UI exposes no host hitboxes, and
guessing from rendered pixels would activate the wrong controls in unverified
menus. The required next step is a read-only local inventory of menu state,
selection, and control rectangles; clicks must enter the same guest A/B paths
as controller input, with directional navigation retained as fallback.

## Diagnostic automation

The experimental synthetic-input build contains one strict built-in script,
`remastered-boot-menu-to-gameplay-v1`. It drives Xbox-equivalent Start,
D-pad Down, and A edges on guest polling epochs rather than wall time or OS
keystrokes. It is available only through explicit Remastered diagnostic
selection, rejects unknown names, and releases state on completion, focus
loss, and shutdown. It does not make menu hitboxes or physical input proven.

The bounded diagnostic runner also owns a headless 48 kHz stereo audio audit
mode. It keeps guest APU/DSP timing active when CoreAudio is unavailable,
retains no samples, rejects every sample-data writer, and reports only
aggregate signal/queue/cadence metrics. It is never an automatic fallback and
does not establish that sound is audible.

The runner also owns strict `--water-preset retail|realistic` selection.
Realistic requires Remastered, inherited `BW_METAL_WATER` values are discarded,
and a requested realistic run fails validation if no guest boot reports actual
shader activation. It reports activation and unsupported-pass fallback by boot
without exposing private render-state data.

`--water-benchmark` is a separate diagnostic-only mode. It requires
Remastered, the Retail starting preset, and `--gpu-audit`; the clean child
environment alone receives `BW_METAL_WATER_BENCHMARK=1`. The backend runs
10 warm-up plus 50 measured supported-water frames in each of four
Retail/Realistic/Realistic/Retail blocks. The runner rejects missing,
duplicate, malformed, non-finite, or internally inconsistent summaries and
rejects benchmark output when the mode was not requested.

## Ship and weapon mod profiles

Existing source-controlled hooks prove that projectiles retain references to
weapon/turret definitions and owners, but they do not establish the requested
damage, fire-rate, speed, range, ammo, durability, or handling field layouts.
They also do not prove whether definitions are shared, copied, or freed across
mission transitions. Arbitrary address patches or guessed offsets are
therefore out of scope.

The safe implementation sequence is:

1. Add bounded read-only local instrumentation at validated definition
   construction and fire/projectile creation boundaries.
2. Export a baseline from the user's prepared retail copy into a private local
   file; never commit names or values derived from retail data.
3. Prove canonical IDs, units, finite ranges, sentinels, aliases, ownership,
   and lifetime through controlled A/B probes.
4. Define a versioned UTF-8 profile containing a schema version, retail/build
   fingerprint, profile ID, and whitelisted named fields.
5. Reject the entire profile on unknown definitions/fields, duplicates,
   non-finite values, unsafe ranges, or fingerprint mismatch, with file/line
   diagnostics.
6. Apply definition-wide overrides to owned in-memory copies at a proven load
   boundary. Add player-only copies only where player ownership is explicit.
7. Keep profile changes restart-required until a mission-boundary reload
   proves that no live instance retains the old definition.
8. Build the standalone AppKit editor on the exact same parser and validator.
   It may edit profiles in the user configuration directory, never the retail
   tree.

## Metal quality

Remastered LOD now supports 1x through 8x as an atomic live value. Metal
anisotropy prebuilds immutable sampler variants and changes the selected
variant at a flip boundary. Only linear, mipmapped stages receive anisotropy;
point-sampled and unmipped stages remain 1x.

Metal MSAA remains explicitly unsupported. The memory lower bounds, required
surface/resolve ownership, and go/no-go measurements are in
[METAL_QUALITY.md](METAL_QUALITY.md). The standalone evidence tool validates a
machine-readable record and returns no-go whenever measurements, correctness
checks, or visual review are missing. A standalone real-Metal smoke target now
passes 2x/4x/8x synthetic color, depth/stencil, resolve, and readback on the
Apple M5, but it is deliberately separate from the game renderer.

## Water-system research

[WATER_RENDERING.md](WATER_RENDERING.md) is now the authoritative map of the
water path. It separates the title's simulation and ocean geometry from the
three Retail material passes, traces the versioned lit-pass marker into Metal,
inventories verified and unavailable shader inputs, and defines the
mechanics-preservation boundary.

The research explains the current near-Retail result: the optional fragment
entry applies a deliberately small RGB-only treatment to the marked lit/base
pass, while the later cube-reflection and wake-foam passes remain Retail. It
also identifies a parity gap: `OCEAN_REFLECT_SINGLE` is present in the shared
fix registry but its implementation is currently DX11-only.

The next safe water work is:

1. explicitly mark all three ocean passes at validated title boundaries;
2. replace separate-run comparisons with same-execution aligned A/B capture;
3. port or explicitly report the distant-water-line fix as unsupported on
   Metal;
4. isolate pass 1 and pass 2 before tuning either shader; and
5. keep scene color, sampled depth, refraction, absorption, shoreline foam,
   and replacement art out of scope until a new resource/material contract is
   designed and justified.

## Upstream parity

The local registry is byte-identical to recorded upstream commit `e631edd` and
contains all 42 changes. Original initializes all of them off; Remastered uses
each explicit registry default. The two unmerged upstream commits are
Windows/DX11 install-time shader warm-up work, not gameplay fixes. Run:

```sh
python3 tools/check_upstream_parity.py --require-upstream
```

See [UPSTREAM_BASELINE.md](UPSTREAM_BASELINE.md) for the manifest and review
policy.

## Realistic water

The current Metal path reproduces the retail register-combiner ocean and cube
reflection programs. A validated native ocean boundary appends a versioned NOP
through the title's push buffer. The executor scopes it to the next BEGIN/END
draw, copies the classification synchronously into each extended batch, clears
it at END/frame boundaries, coalesces repeated identical markers while the
next scope is armed, and rejects conflicting, unknown, inside-draw, or
unconsumed markers.

A bounded four-boot Remastered diagnostic followed validated relaunches and
recorded 41,348 received markers, 512 batch scopes, and zero rejected markers.
This validates command ordering and lifetime in the exercised boot flow, not
all water passes or visual classification across representative gameplay.

A combiner/texture fingerprint alone may match non-water reflective geometry,
so it remains only a consistency check. Missing marker evidence always selects
retail rendering rather than guessing.

Marked batches now produce a versioned FNV-1a-64 fingerprint from stable,
copied render/program/combiner/texture metadata only. Private GPU audits keep a
fixed 32-entry aggregate table plus overflow and within-scope mismatch counts;
they exclude addresses, vertices, texture bytes, names, and assets. Synthetic
tests cover determinism, state differences, marker-only collection, bounds,
scope lifetime, and unchanged rendering.

A deterministic three-boot, two-relaunch Remastered run with headless audio
timing reached gameplay and produced 12,670 markers, 154 clean scopes, and 297
fingerprinted batches across 10 aggregate hashes. Marker rejection,
fingerprint-table overflow, and within-scope mismatch were all zero. This
completed runtime identification for the exercised automated flow without
publishing the private hash values.

Remastered now exposes `graphics.water_rendering=retail|realistic` in F1 and
the strict `BW_METAL_WATER` startup switch. Retail is the default and continues
to use the existing `extended_fragment` pipeline. Realistic has a separate
pipeline cache key and fragment entry, activates only at a flip boundary, and
requires both the explicit ocean marker and the validated stage-0 texture plus
passthrough water-basis shape. Unsupported marked batches fall back
individually to retail. A realistic pipeline-creation failure explicitly
returns the requested/active preset to retail before retrying the retail
pipeline.

The optional shader decodes the existing interpolated water normal and
view-direction basis, applies a restrained Schlick-style Fresnel weight to the
stage-0 water sample and fog-atmosphere colors, and preserves the Retail
alpha. The later Retail cube-reflection and wake-foam passes are unchanged. It
adds no replacement assets, depth reads, refraction, absorption, foam, or
shoreline logic. The setting is deliberately absent from quality presets.

Synthetic coverage proves default Retail behavior, frame-boundary activation,
Original rejection, strict environment parsing, deterministic marked-pixel
change, exact return to Retail on the next unmarked draw, and unsupported-state
fallback. A later private four-boot Remastered run reached 40,790 markers in
499 clean scopes and 602 fingerprinted batches across 16 aggregate hashes.
The realistic pipeline activated in the two boots that reached supported
marked passes; no unsupported-pass fallback, marker rejection, fingerprint
overflow/mismatch, or Metal runtime error was observed.

A later private four-boot diagnostic completed the same-scene ABBA benchmark
in two independent boots, each with 100 Retail and 100 Realistic measured
supported-water frames. Realistic increased water-containing command-buffer
GPU duration by 4.548%-4.822% for mean, 1.865%-3.570% for median, and
7.787%-10.092% for p95. The command-buffer timer includes non-water work
sharing those buffers, so these ranges are not isolated fragment-shader cost.
No automatic acceptance threshold or visual-quality conclusion is inferred.

Runtime activation and controlled performance evidence are complete for the
exercised automated scene. Retail/Realistic visual comparisons across
calm/storm, day/night, shore, split-screen, reflection, and wake scenes remain
open. The preset stays experimental, default-off, and absent from quality
presets. Depth absorption, refraction, replacement foam, and shoreline
effects still require separate depth/material ownership.
