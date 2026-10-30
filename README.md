# Blood Wake Recomp for Apple Silicon

An in-progress native macOS/Apple Silicon fork of
[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp), using
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). It aims to run your
own retail copy of Blood Wake using static recompilation and a Metal renderer.
It is not affiliated with the game or hardware's rights holders.

## Status

**Not playable on macOS yet.** This fork can prepare and verify translated C
and build an experimental native Apple Silicon executable. It can open a
window and enter the guest, but an interactive menu has not been verified.
Guest USB and audio MMIO remain disabled pending safe dispatch, so controller
input and game audio do not work yet.

The native host and Metal pushbuffer backend are under development; rendering,
video, saves and both editions still need game validation. The toolkit's
OpenGL renderer is not the selected game renderer. See
[docs/BUILDING.md](docs/BUILDING.md#macos-on-apple-silicon-experimental) for
native build instructions and limitations, and
[docs/UPSTREAM_ISSUES.md](docs/UPSTREAM_ISSUES.md) for portability findings.
The [Apple Silicon handoff](docs/APPLE_SILICON_HANDOFF.md) records the current
boot blocker and next steps.

## Prepare your copy

You need to own a retail North American Blood Wake disc and make your own disc
image or folder of its files, as required by [LEGAL.md](LEGAL.md). You also need
Python 3.13, Git, and enough free space for the disc and generated files. The
process uses the upstream retail and generated code fingerprints. The
repository does not include or download the game itself.

From the repository root, on macOS:

```sh
git config core.hooksPath tools/git-hooks
python3.13 -m venv build/venv
build/venv/bin/python -m pip install capstone==5.0.9
git clone https://github.com/sp00nznet/xboxrecomp.git build/xboxrecomp
git -C build/xboxrecomp checkout --detach f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b

build/venv/bin/python tools/build_from_disc.py \
  --disc "/path/to/your/disc.iso" \
  --install-dir game/prepare \
  --toolkit build/xboxrecomp \
  --prepare-only
```

`--disc` accepts an XISO, a full disc image, or a folder of disc files, not a
7z archive. Extract the image locally first. `--check-only` checks the disc
without extracting it or generating code. `--prepare-only` unpacks the disc,
translates its program, and verifies the generated output; it does **not**
produce an executable. On macOS, verification compares generated text using
the Windows line endings of the published fingerprints without changing the
files. A rerun replaces `game/prepare/work/gen`. Use a separate work directory
for anything you want to keep.

Keep disc images, unpacked files, generated C, binaries, logs and other
game-derived material private. The example uses ignored `game/` and `build/`
directories and enables the upstream commit guard; do not commit or share the
game or material generated from it. See [LEGAL.md](LEGAL.md) and
[CONTRIBUTING.md](CONTRIBUTING.md) for the upstream project's conditions.

## Native foundation checks

The standalone Metal backend test sends synthetic NV2A methods through the
carried pushbuffer executor into an off-screen Metal target. It covers 32-bit
colour clears, coloured triangles, a basic nearest-filtered and clamp-to-edge
stage-0 texture, blend factors and equations, colour-write masks, alpha
comparisons, face culling, and flip readback into synthetic guest memory. Unsupported
states encountered by this backend are reported as errors; surface and
texture dimensions are limited to 16384 pixels per axis. The backend is wired
into the experimental native executable, but in-game rendering has not been
verified and much of the renderer remains unfinished. The new render states
include a limited one-stage combiner and BC1 texture path in synthetic tests;
other combiner and texture states may still fail explicitly.

The separate Metal smoke test renders and reads back a synthetic triangle.
The audio test checks the SDL dummy device and the carried stereo output
adapter; it does not exercise the complete APU. The input integration test
checks SDL keyboard and virtual-controller events through the USB gamepad
report, including controller connection and disconnection. It does not test
the game or a physical controller. On macOS, with CMake, SDL2, and the pinned
toolkit checkout installed, the host MMIO tests also exercise test-only device
dispatch experiments; they do not enable guest USB or audio:

```sh
cmake -S tests/metal_backend -B build/test-metal-backend
cmake --build build/test-metal-backend
ctest --test-dir build/test-metal-backend --output-on-failure

cmake -S tests/metal_smoke -B build/test-metal-smoke
cmake --build build/test-metal-smoke
ctest --test-dir build/test-metal-smoke --output-on-failure

cmake -S tests/apu_output -B build/test-apu-output
cmake --build build/test-apu-output
ctest --test-dir build/test-apu-output --output-on-failure

cmake -S tests/input_integration -B build/test-input-integration
cmake --build build/test-input-integration
ctest --test-dir build/test-input-integration --output-on-failure

cmake -S tests/macos_presenter -B build/test-macos-presenter
cmake --build build/test-macos-presenter
ctest --test-dir build/test-macos-presenter --output-on-failure

cmake -S tests/macos_host -B build/test-macos-host
cmake --build build/test-macos-host
ctest --test-dir build/test-macos-host --output-on-failure

cmake -S tests/mmio_host -B build/test-mmio-host
cmake --build build/test-mmio-host
ctest --test-dir build/test-mmio-host --output-on-failure

build/venv/bin/python -m unittest discover -s tests -p 'test_*.py'
```

For a toolkit checkout outside `build/xboxrecomp`, pass
`-DXBOXRECOMP_DIR=/path/to/xboxrecomp` when configuring the Metal backend,
audio, input, presenter, host, and MMIO tests. The input tests use isolated virtual
controllers and report a skip when a physical controller is connected. The
Metal backend test builds only the necessary CPU-side toolkit sources and does
not require the toolkit's OpenGL backend.
The [local toolchain note](docs/UPSTREAM_ISSUES.md#local-toolchain-note)
describes an SDK selection needed on the development machine.

## Windows upstream

The existing Windows build remains available through
[`tools/build_from_disc.py`](tools/build_from_disc.py) without `--prepare-only`;
see [docs/BUILDING.md](docs/BUILDING.md) for its Windows prerequisites and
instructions. The original project and its documentation remain available
[upstream](https://github.com/minimalice92/bloodwake-recomp). Two newer
upstream commits add Windows shader-cache warm-up to the installer; they are
not yet merged into this fork and do not implement Metal rendering.

## Credits and licence

Blood Wake is by Stormfront Studios and was published by Microsoft Game
Studios. This fork builds on Blood Wake Recomp, xboxrecomp and xemu. See
[CREDITS.md](CREDITS.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE), and
[LICENSES/](LICENSES/) for attribution and licence details.
