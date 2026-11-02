# Apple Silicon handoff

Status updated 2026-11-02 after the native F1 settings integration, gameplay
mouse input, live LOD, Metal anisotropic filtering, upstream parity audit,
deterministic menu-to-gameplay diagnostics, and bounded audio validation.

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

## Required opt-ins

The currently verified path requires:

- A build containing `BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON`
- `BW_MACOS_KEYBOARD_HOOK=1` at runtime
- `BW_METAL_QUINCUNX_APPROX=1` at runtime

Gameplay mouse is optional and additionally requires `BW_MOUSE=1`. F1 itself
requires no extra runtime switch in the native build.

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
- [METAL_QUALITY.md](METAL_QUALITY.md) records native LOD/filtering behavior
  and the current MSAA no-go gate.
- [UPSTREAM_BASELINE.md](UPSTREAM_BASELINE.md) records the 42-fix baseline and
  the classified DX11-only upstream commits.

## Remaining limitations

- Formal visual comparison with trustworthy reference frames is outstanding.
- Direct presentation and 4x scaled rendering have hands-on visual
  confirmation; resize behavior, frame time, and memory still need a
  controlled retail check.
- Quincunx filtering is approximate and not hardware-verified.
- Frame pacing reaches the 30 FPS average target but retains 45-46 ms p95
  intervals in the measured scene.
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
- Ocean draws now carry an explicit, false-positive-safe game-to-executor
  marker. A deterministic Remastered flow recorded 154 scopes and 297
  fingerprinted batches across 10 aggregate hashes with no rejection,
  overflow, or mismatch. A realistic preset remains blocked on representative
  calm/storm, lighting, shore, split-screen, reflection, wake, and visual
  coverage. Retail water remains unchanged.
- Physical controllers, guest USB, audible/sustained native audio, saves, FMV,
  other editions, and broad mission coverage are not validated Apple Silicon
  features.
- The bounded audio boot logged one SDL underrun; latency, underrun behavior,
  channel balance, and output quality need hands-on measurement.
- The headless audio audit retains no samples and cannot prove audible output;
  its analytical queue observed scheduling underflows, so it is signal/timing
  instrumentation rather than a device-quality result.
- General CPU/GPU guest-memory ownership, aliases, DMA lifetime, and safe
  in-process device shutdown are not solved.
- Windows behavior after the Apple Silicon changes has not been revalidated.

## Next priorities

### 1. Preserve the playable path

- Keep the user-confirmed menu and in-game path covered by repeatable retail
  diagnostics.
- Compare private menu and gameplay frames against a trustworthy reference.
- Collect a representative 60-second active-gameplay sample and continue
  reporting average FPS, median, p95, maximum stalls, resolution, and enabled
  approximations.
- Repeat that sample with direct presentation at 1x and automatic native scale
  so the CPU-copy savings, GPU cost, and memory growth are measured separately.
- Investigate the remaining frame-pacing jitter without weakening correctness
  or silently skipping unsupported work.

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
- Validate the existing scoped ocean marker and state fingerprints across all
  representative water passes before changing the Metal shader. The automated
  boot-to-gameplay path is complete; the remaining boundary is scene and
  visual coverage.
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
