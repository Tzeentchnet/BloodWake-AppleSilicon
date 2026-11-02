# Native feature roadmap

This roadmap records the investigation and implementation status for native
runtime settings, mouse input, data-driven mods, renderer quality, upstream
fixes, and water rendering. Retail files, generated code, captures, and
locally exported definition data remain private and must not be committed.

## Status

| Area | Current status | Next evidence or implementation boundary |
| --- | --- | --- |
| F1 settings | Implemented | Add a title-state pause hook only after menus, gameplay, cutscenes, and transitions can be distinguished safely. |
| Gameplay mouse | Implemented | Validate sensitivity and steering/aim choices in sustained gameplay. |
| Point-and-click menus | Blocked on menu UI mapping | Identify menu state and selectable rectangles, then map letterboxed drawable coordinates into the 640x480 guest UI. |
| Ship/weapon profiles | Research blocked on field ownership | Instrument definition construction locally and prove named fields, units, ranges, aliases, and lifetimes before writing any override. |
| Standalone mod editor | Planned after the shared schema | Build only after the runtime parser can validate and apply a useful profile with synthetic fixtures. |
| Adjustable LOD | Implemented | Validate 1x-8x visually in both editions; Original remains fixed at 1x. |
| Metal filtering | Implemented | Collect oblique water/cliff comparisons and GPU timings for 1x/4x/8x/16x. |
| Metal MSAA | No-go; harness implemented | Fill and evaluate the controlled visual/GPU-time/memory record in `METAL_QUALITY.md`; do not expose the DX11 control on Metal before it passes. |
| Upstream fixes | Verified | Re-run the offline parity checker whenever upstream changes. |
| Realistic water | Marker implemented; capture validation pending | Validate marker coverage and state fingerprints across every ocean pass before adding a shader variant. |

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
checks, or visual review are missing.

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
reflection programs. A validated native ocean boundary now appends a versioned
NOP through the title's push buffer. The executor scopes it to the next
BEGIN/END draw, copies the classification synchronously into each extended
batch, clears it at END/frame boundaries, and rejects duplicate, unknown,
inside-draw, or unconsumed markers. Marked and unmarked draws still select the
same retail Metal path.

A combiner/texture fingerprint alone may match non-water reflective geometry,
so it remains only a consistency check. Missing or mismatched marker evidence
must select retail rendering and report the unknown state rather than guess.

After marker coverage is proven across calm/storm, day/night, shore,
split-screen, reflection, and wake passes, the first optional preset may add
restrained Fresnel-weighted reflection and roughness/specular shaping using
existing inputs. Retail remains the exact default. Depth absorption,
refraction, foam, and shoreline effects wait for separate depth/material
ownership.
