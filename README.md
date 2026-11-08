# Blood Wake Recomp for Apple Silicon

A native macOS/Apple Silicon fork of
[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp), built
with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a Metal
renderer. It runs a user-provided retail copy of Blood Wake through static
recompilation.

This project is not affiliated with the game's or hardware's rights holders.

## Requirements

- An Apple Silicon Mac
- Xcode Command Line Tools
- CMake 3.20 or newer
- SDL2 2.0.14 or newer
- OpenSSL 3
- Python 3.13 and Git
- Your own North American retail Blood Wake disc image or extracted disc

The repository does not include or download the game.

## Quick start

From the repository root:

```sh
git config core.hooksPath tools/git-hooks
python3.13 -m venv build/venv
build/venv/bin/python -m pip install capstone==5.0.9

git clone https://github.com/sp00nznet/xboxrecomp.git build/xboxrecomp
git -C build/xboxrecomp checkout --detach 343b6d8901723b37ce568b2418c5df4e1fe1d223

build/venv/bin/python tools/build_from_disc.py \
  --disc "/path/to/your/disc.iso" \
  --install-dir game/prepare \
  --toolkit build/xboxrecomp \
  --prepare-only

cmake -S . -B build/bloodwake-macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DXBOXRECOMP_DIR="$PWD/build/xboxrecomp" \
  -DBW_GEN_DIR="$PWD/game/prepare/work/gen" \
  -DPython3_EXECUTABLE="$PWD/build/venv/bin/python" \
  -DBW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON

cmake --build build/bloodwake-macos --target bloodwake --parallel
```

If CMake cannot find Homebrew SDL2 or OpenSSL:

```sh
export CMAKE_PREFIX_PATH="$(brew --prefix sdl2):$(brew --prefix openssl@3)"
```

Run the prepared build:

```sh
BW_GAME_DIR="$PWD/game/prepare/game/disc" \
RECOMP_KEYBOARD=1 \
BW_MACOS_KEYBOARD_HOOK=1 \
BW_MOUSE=1 \
BW_METAL_QUINCUNX_APPROX=1 \
BW_METAL_RENDER_SCALE=auto \
./build/bloodwake-macos/bloodwake
```

Detailed preparation, alternate installation, diagnostic, and Windows
instructions are in [BUILDING.md](docs/BUILDING.md).

## Privacy and legal

Keep disc images, unpacked files, generated C, binaries, saves, logs, traces,
archives, and captures private. Do not commit or share game-derived material.
See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution and takedown guidance.

Blood Wake is by Stormfront Studios and was published by Microsoft Game
Studios. This fork builds on Blood Wake Recomp, xboxrecomp, and xemu. See
[credits and acknowledgements](docs/CREDITS.md), [NOTICE.md](NOTICE.md),
[LICENSE](LICENSE), and [LICENSES/](LICENSES/) for attribution and licence
details.
