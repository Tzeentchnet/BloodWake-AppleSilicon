# Apple Silicon handoff

Status updated 2026-11-01 after native APU dispatch implementation, sanitizer
validation, and a bounded default-audio retail boot.

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

## Required opt-ins

The currently verified path requires:

- A build containing `BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON`
- `BW_MACOS_KEYBOARD_HOOK=1` at runtime
- `BW_METAL_QUINCUNX_APPROX=1` at runtime

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

## Remaining limitations

- Formal visual comparison with trustworthy reference frames is outstanding.
- Direct presentation and 4x scaled rendering have hands-on visual
  confirmation; resize behavior, frame time, and memory still need a
  controlled retail check.
- Quincunx filtering is approximate and not hardware-verified.
- Frame pacing reaches the 30 FPS average target but retains 45-46 ms p95
  intervals in the measured scene.
- Keyboard feedback, reconnect, focus-loss recovery, and the complete device
  lifecycle remain experimental.
- Physical controllers, guest USB, audible/sustained native audio, saves, FMV,
  other editions, and broad mission coverage are not validated Apple Silicon
  features.
- The bounded audio boot logged one SDL underrun; latency, underrun behavior,
  channel balance, and output quality need hands-on measurement.
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

### 2. Complete renderer validation

- Validate quincunx weights, sample positions, edge rules, and rounding against
  suitable hardware evidence, or retain the approximation label.
- Extend mission coverage and implement any newly observed state families as
  coherent renderer features.
- Preserve explicit error propagation and render-target/texture invalidation
  at every new ownership boundary.

### 3. Complete input and device lifecycles

- Turn the current keyboard interaction into a repeatable regression covering
  press, release, focus loss, reconnect, and shutdown.
- Obtain hands-on confirmation of audible output, channel balance, latency,
  underruns, and sustained gameplay with the new APU path.
- Preserve process lifetime after guest entry; complete DMA ownership,
  IRQ/DPC retirement, and callback quiescence before attempting in-process
  APU teardown or enabling OHCI.
- Add physical-controller validation only after the remaining USB contracts
  are safe.

### 4. Broaden product coverage

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
