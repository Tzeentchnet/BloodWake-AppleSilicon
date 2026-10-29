# Blood Wake Recomp for Apple Silicon

An in-progress native macOS/Apple Silicon fork of
[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp), using
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp). It aims to run your
own retail copy of Blood Wake using static recompilation and a Metal renderer.
It is not affiliated with the game or hardware's rights holders.

## Status

**Not playable on macOS yet.** The upstream host, renderer and launcher are
Windows-specific. This fork can check an eligible disc and prepare and verify
the translated C on macOS; it does not yet build or run a native game.

The planned native work includes a macOS host, Metal implementation of the
game's pushbuffer rendering backend, audio and input integration, and validation
of gameplay, video, saves and both editions. The toolkit's existing OpenGL
renderer is not the selected game renderer. Portability findings and potential
upstream contributions are recorded in [docs/UPSTREAM_ISSUES.md](docs/UPSTREAM_ISSUES.md).

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
stage-0 texture, and flip readback into synthetic guest memory. Unsupported
states encountered by this test backend are reported as errors; surface and
texture dimensions are limited to 16384 pixels per axis. It is not
registered with the game: register combiners, depth/stencil, blending, native
window presentation and much of the renderer remain unfinished.

The separate Metal smoke test renders and reads back a synthetic triangle.
The audio test checks the SDL dummy device and the carried stereo output
adapter; it does not exercise the complete APU. On macOS, with CMake, SDL2,
and the pinned toolkit checkout installed:

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

build/venv/bin/python -m unittest discover -s tests -p 'test_*.py'
```

For a toolkit checkout outside `build/xboxrecomp`, pass
`-DXBOXRECOMP_DIR=/path/to/xboxrecomp` when configuring the Metal backend and
audio tests. The Metal backend test builds only the necessary CPU-side toolkit
sources and does not require the toolkit's OpenGL backend.
The [local toolchain note](docs/UPSTREAM_ISSUES.md#local-toolchain-note)
describes an SDK selection needed on the development machine.

## Windows upstream

The existing Windows build remains available through
[`tools/build_from_disc.py`](tools/build_from_disc.py) without `--prepare-only`;
see [docs/BUILDING.md](docs/BUILDING.md) for its Windows prerequisites and
instructions. The original project and its documentation remain available
[upstream](https://github.com/minimalice92/bloodwake-recomp).

## Credits and licence

Blood Wake is by Stormfront Studios and was published by Microsoft Game
Studios. This fork builds on Blood Wake Recomp, xboxrecomp and xemu. See
[CREDITS.md](CREDITS.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE), and
[LICENSES/](LICENSES/) for attribution and licence details.
