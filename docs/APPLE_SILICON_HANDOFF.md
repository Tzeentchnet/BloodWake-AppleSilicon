# Apple Silicon handoff

Status updated 2026-10-31 after hands-on gameplay confirmation.

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

## Required opt-ins

The currently verified path requires:

- A build containing `BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON`
- `BW_MACOS_KEYBOARD_HOOK=1` at runtime
- `BW_METAL_QUINCUNX_APPROX=1` at runtime

The quincunx path is a narrowly scoped legacy Gaussian approximation, not
verified NV2A filtering. Unsupported renderer states continue to fail
explicitly.

Guest USB and APU MMIO remain disabled. Do not set `RECOMP_USB` or
`RECOMP_AC97_READY`, even to `0`; both are presence-based in the current host.

## Detailed records

- [WORK_COMPLETED.md](WORK_COMPLETED.md) records implemented renderer, runtime,
  input, diagnostic, and device-research work.
- [VALIDATION.md](VALIDATION.md) records retail performance, hands-on
  confirmation, regression coverage, integrity checks, and test commands.
- [BUILDING.md](BUILDING.md) contains build, run, and isolated diagnostic
  instructions.

## Remaining limitations

- Formal visual comparison with trustworthy reference frames is outstanding.
- Quincunx filtering is approximate and not hardware-verified.
- Frame pacing reaches the 30 FPS average target but retains 45-46 ms p95
  intervals in the measured scene.
- Keyboard feedback, reconnect, focus-loss recovery, and the complete device
  lifecycle remain experimental.
- Physical controllers, guest USB, audio, saves, FMV, other editions, and
  broad mission coverage are not validated Apple Silicon features.
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
- Implement safe MMIO dispatch, DMA ownership, IRQ/DPC retirement, and
  callback quiescence before enabling OHCI or APU devices.
- Add physical-controller and audio validation only after those contracts are
  safe.

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
