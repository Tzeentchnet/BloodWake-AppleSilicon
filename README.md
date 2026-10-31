# Blood Wake Recomp for Apple Silicon

An experimental native macOS/Apple Silicon fork of
[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp), built
with [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and a Metal
renderer. It runs a user-provided retail copy of Blood Wake through static
recompilation.

This project is not affiliated with the game's or hardware's rights holders.

## Current status

The current arm64 build boots into the game and has been user-confirmed for
menu navigation and interactive play. A measured 640x480 retail workload
sustains the original title's 30 FPS cap.

This is still a development build:

- The game-specific keyboard bridge and Metal quincunx approximation require
  explicit build or runtime opt-ins.
- The quincunx approximation is not verified against NV2A hardware.
- Guest audio, USB gamepads, physical-controller support, and broad mission,
  save, FMV, and edition coverage are incomplete.
- Measured gameplay averages 30 FPS, but p95 frame pacing remains about
  45-46 ms.

See [Apple Silicon status](docs/APPLE_SILICON_HANDOFF.md),
[work completed](docs/WORK_COMPLETED.md), and
[validation](docs/VALIDATION.md) for details.

## Requirements

- An Apple Silicon Mac
- Xcode Command Line Tools
- CMake 3.20 or newer
- SDL2 2.0.14 or newer
- OpenSSL 3
- Python 3.13 and Git
- Your own North American retail Blood Wake disc image or extracted disc

The repository does not include or download the game.

## Prepare and build

From the repository root:

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
BW_MACOS_KEYBOARD_HOOK=1 \
BW_METAL_QUINCUNX_APPROX=1 \
./build/bloodwake-macos/bloodwake
```

Detailed preparation, alternate install, diagnostic, and Windows instructions
are in [BUILDING.md](docs/BUILDING.md).

## Documentation

- [BUILDING.md](docs/BUILDING.md) - build, run, and diagnostic instructions
- [APPLE_SILICON_HANDOFF.md](docs/APPLE_SILICON_HANDOFF.md) - current status
  and remaining priorities
- [WORK_COMPLETED.md](docs/WORK_COMPLETED.md) - implementation history
- [VALIDATION.md](docs/VALIDATION.md) - performance evidence, tests, and
  integrity checks
- [SETTINGS.md](docs/SETTINGS.md) - runtime and edition settings
- [UPSTREAM_ISSUES.md](docs/UPSTREAM_ISSUES.md) - portability findings
- [GPU_CAPTURE.md](docs/GPU_CAPTURE.md) - synthetic capture fixture
- [GUEST_MEMORY_INVENTORY.md](docs/GUEST_MEMORY_INVENTORY.md) - memory-access
  inventory

## Privacy and legal

Keep disc images, unpacked files, generated C, binaries, saves, logs, and
captures private. Do not commit or share game-derived material. See
[LEGAL.md](LEGAL.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

Blood Wake is by Stormfront Studios and was published by Microsoft Game
Studios. This fork builds on Blood Wake Recomp, xboxrecomp, and xemu. See
[CREDITS.md](CREDITS.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE), and
[LICENSES/](LICENSES/) for attribution and licence details.
