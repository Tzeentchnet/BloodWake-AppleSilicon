# Building by hand

The installer does all of this for you. This page is for people who'd rather run the steps themselves, or want to know what the installer does.

Everything happens on your own PC. Your disc's files, the C code generated from them and the executable you build never leave it.

## macOS on Apple Silicon (experimental)

The native build is a work in progress. The current input-enabled Original
edition build is user-confirmed for menu navigation and in-game play at the
title's 30 FPS cap. It uses Apple Clang and the project's Metal backend rather
than the toolkit's OpenGL renderer. Install Xcode Command Line Tools, CMake
3.20 or newer, SDL2 2.0.14 or newer, OpenSSL, and the Python requirements
described in the [README](../README.md). CMake must be able to find the SDL2
and OpenSSL packages; with Homebrew, for example:

```sh
export CMAKE_PREFIX_PATH="$(brew --prefix sdl2):$(brew --prefix openssl@3)"
```

The native host currently refuses to start if `RECOMP_USB` or
`RECOMP_AC97_READY` is present, even if its value is `0`: safe APU and OHCI
MMIO dispatch is not implemented. Leave both unset for diagnostics; this does
not provide working guest audio or a USB gamepad. Guest-created workers cannot
all be joined yet, so after a guest return the host retains its resources and
exits the boot process rather than attempting unsafe in-process teardown.

When unset, the native host defaults `RECOMP_PB_EXEC=1` and
`RECOMP_VBLANK=paced`. Existing values are preserved. Both switches are
presence-based: even an empty value or `0` enables the executor or vblank.
`RECOMP_PB_WALK=dma` remains an explicit opt-in.

For supported Metal colour surfaces, the SDL presenter uses the surface's
frozen pixel format, geometry and sampling dimensions. Supersampled frames
use a rounded RGB box-average fallback before the gamma ramp; the
title-selected NV2A reconstruction filter has not yet been implemented.
Unsupported surface formats or sampling modes fail explicitly.

For diagnostics with a freshly rebuilt native executable,
`BW_METAL_QUINCUNX_APPROX=1` opts into a narrowly scoped legacy Gaussian
approximation for quincunx-filtered pitch-linear XRGB textures. This is not
verified NV2A filtering. The renderer logs a warning at startup and on first
use; values other than `1`, including an unset or empty value, leave strict
rejection enabled. Other unsupported texture states still fail explicitly.
Synthetic tests cover the approximation, and the measured retail gameplay run
exercised it. It has not been validated against NV2A hardware.

For a complete build from your disc, use a **new** install directory:

```sh
build/venv/bin/python tools/build_from_disc.py \
  --disc "/path/to/your/disc.iso" \
  --install-dir "/path/to/new/install" \
  --toolkit "$PWD/build/xboxrecomp"
```

An optional `--work-dir` may be on another volume; free space is checked
separately for the work and install volumes.

The script stages `build/Release/bloodwake` under the install directory.
There is no native launcher yet; run the executable with the install directory
as its working directory. The native install record lists Original only until
the other edition and renderer features are validated.

To compile an already verified preparation without replacing it, point CMake
directly at its generated files and use a new private build directory:

```sh
umask 077
mkdir -m 700 build/bloodwake-macos &&
cmake -S . -B build/bloodwake-macos \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DXBOXRECOMP_DIR="$PWD/build/xboxrecomp" \
  -DBW_GEN_DIR="$PWD/game/prepare/work/gen" \
  -DPython3_EXECUTABLE="$PWD/build/venv/bin/python" \
  -DBW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON &&
cmake --build build/bloodwake-macos --target bloodwake --parallel
```

This keeps `game/prepare/work/gen` read-only: the ocean translation byproduct
is written into the CMake build directory. Choose a different new directory
if that one exists, and check ACLs and backups as appropriate. Keep both
generated inputs and build outputs private. If the default macOS SDK fails a
compiler link test, select a compatible installed SDK before running either
build command. For
example, on the machine described in the
[local toolchain note](UPSTREAM_ISSUES.md#local-toolchain-note):

```sh
export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

For an input-enabled build, `-DBW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK=ON`
enables compilation of a fingerprint-checked, game-specific keyboard bridge.
Its derived generated source stays in the build directory.
The hook is still disabled at runtime unless `BW_MACOS_KEYBOARD_HOOK=1` is
explicitly set; enabling it also requires `RECOMP_KEYBOARD` to be enabled.
Other values for `BW_MACOS_KEYBOARD_HOOK` except `0` are rejected, and a
normal build rejects `1` rather than silently ignoring it. The bridge
supports one keyboard on port 0, advertises no feedback motors and reports
feedback as unsupported. It bypasses
guest device initialization and does not enable guest USB MMIO. The current
input-enabled build has now been user-confirmed for menu navigation and
in-game play; detailed reconnect, focus-loss and lifecycle behavior remain
unverified.

An isolated retail run has verified guest initialization, enumeration,
controller open, device-change and sustained state-poll calls with this hook
active while the scripted 3D scene ran at its 30 FPS cap. A separate synthetic
run verified that a guest state poll received Start, Down and A. Physical
interaction was subsequently confirmed by the user with the current build.
The build and runtime opt-ins remain required while the broader input contract
is still experimental. Run an input-enabled build with:

```sh
BW_GAME_DIR="$PWD/game/prepare/game/disc" \
BW_MACOS_KEYBOARD_HOOK=1 \
BW_METAL_QUINCUNX_APPROX=1 \
build/bloodwake-macos/bloodwake
```

For a synthetic-input diagnostic only, additionally build with
`-DBW_EXPERIMENTAL_SYNTHETIC_INPUT=ON`. This requires the experimental keyboard
hook build option and is off by default. A separate runtime opt-in,
`BW_SYNTHETIC_INPUT_DIAGNOSTIC=1`, requires the hook and a visible SDL window.
It queues and holds synthetic Return, Down and Z key presses inside this
process before starting the guest; it does not generate physical or OS-level
keystrokes. The hook logs when a guest state query sees Start, Down and A.
This tests neither physical keyboard input nor menu navigation.

### Isolated native diagnostic

By default, the diagnostic runner performs one boot, not a playability test.
It verifies the prepared disc and generated-code fingerprints, requires the
pinned toolkit checkout to be clean, prints the executable's SHA-256, and
compares the prepared tree, toolkit and executable before and after the run.
The hash identifies the executable; it does not prove which sources were used
to build it. Tree comparison checks contents, paths, modes and symlink targets,
not ACLs or extended attributes. Symlinks must resolve within the tree.

On macOS, with a separately built executable, choose a **new** private output
directory outside the repository, preparation and toolkit:

```sh
mkdir -p "$HOME/bw-private-diagnostics"
build/venv/bin/python tools/macos_diagnostic.py \
  --exe "$PWD/build/bloodwake-macos/bloodwake" \
  --prepared-tree "$PWD/game/prepare" \
  --toolkit "$PWD/build/xboxrecomp" \
  --output-dir "$HOME/bw-private-diagnostics/boot-001" \
  --seconds 45 --gpu-audit
```

The runner creates isolated home, configuration, temporary and save paths,
discards inherited game settings, and refuses USB and audio MMIO. It checks a
macOS sandbox before starting: writes are allowed only under the output
directory, network operations and process forks are denied, and TCP/UDP
denial is probed for both IPv4 and IPv6. This is not a system-wide network
firewall. The boot has a deadline of 1-300 seconds and an external deadline;
combined standard output and error are capped at 32 MiB. Other files in the
private directory are not subject to a total disk quota. By default, a
guest-requested relaunch is saved privately but not executed. Exit 124
indicates the guest's run limit, not a successful playability check.

To follow same-title relaunch requests, add `--follow-relaunches`. Each boot
uses a separate sandboxed child and numbered private logs, audits and relaunch
records; only validated BWRL requests for `default.xbe` are followed.
`--max-boots` bounds the number of boots (default 4, maximum 16), and
`--total-seconds` bounds total guest execution (default 300, maximum 3600).
The 32 MiB output limit applies across all boot logs. This mode has been
exercised through the retail title's validated same-title relaunch.

`--gpu-audit` enables the existing private usage summary; it is not a replay
capture and a short failing run may end before a summary is written. Add
`--quincunx-approx` only when intentionally testing the unverified approximation
described above. `--experimental-keyboard-hook` opts into the experimental
hook in a build containing it; inherited hook settings are otherwise discarded.
With a synthetic-input diagnostic build, add `--synthetic-input` along with
`--experimental-keyboard-hook` to opt into the held synthetic key presses.
Inherited synthetic-input settings are also discarded.
Keep the output directory, including logs, saves and any
guest data, private. A missing sandbox or failed isolation check aborts before
guest execution.

## Windows

## 1. Install the tools
| Tool | Version | Where from |
|---|---|---|
| Windows | 10 or 11, 64-bit | |
| Python | 3.13, 64-bit, with tkinter (the launcher uses it). 3.10 or later runs, but the fingerprints were made with 3.13. | [python.org](https://www.python.org/downloads/windows/) |
| Python packages | exactly the versions in `tools/requirements.txt` (`capstone==5.0.9`, `pefile==2024.8.26`, ...) | `python -m pip install -r tools/requirements.txt` |
| Visual Studio 2022 Build Tools | 17.x, with the **Desktop development with C++** workload (it includes the Windows SDK). Visual Studio 2022 Community/Professional works too. **Not** Visual Studio 2026. | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) |
| CMake | 3.20 or later. The copy that comes with Visual Studio is fine. | [cmake.org](https://cmake.org/download/) |
| Git | any recent version (only to clone this repository; you can also download it as a zip) | |

You don't need the `py` launcher, administrator rights, Ghidra, or a copy of the toolkit: the build script fetches the toolkit itself.

## 2. Get this repository
```
git clone https://github.com/minimalice92/bloodwake-recomp.git
cd bloodwake-recomp
python -m pip install -r tools/requirements.txt
```

## 3. Build from your disc
```
python tools/build_from_disc.py --disc "<your copy>" --install-dir "<install folder>"
```
`<your copy>` is one of: a folder with your disc's files, an Xbox ISO (XISO), or a full disc image. Paths with spaces are fine (keep the quotes).

### Options
| Option | What it does |
|---|---|
| `--disc <folder or image>` | Your copy of the game (required). |
| `--install-dir <folder>` | Where the game goes; created if needed (required). |
| `--toolkit <folder>` | Use a clean xboxrecomp checkout you already have. It must be at the pinned commit; the script checks. Without this option the script downloads GitHub's archive of that commit and checks its SHA-256. |
| `--work-dir <folder>` | Where the toolkit, the generated C and the build go (default: `<install-dir>\work`). Logs are in `<work-dir>\logs\`. |
| `--jobs N` | Parallel compile jobs (default: your number of CPU threads). |
| `--check-only` | Only identify your copy and check its fingerprint. Nothing is unpacked or copied. |
| `--prepare-only` | Unpack and fingerprint the disc, translate its program and verify the generated C, but don't build or install an executable. Available on macOS as well as Windows; incompatible with `--check-only`. A rerun replaces `<work-dir>/gen`. |
| `--progress json` | One JSON object per line on standard output, for front ends such as the installer: `{"step": "...", "pct": 0-100, "msg": "..."}`. A failure adds `"error": true` and `"exit": <code>`. The default is plain text. |

On macOS and Linux, generated text may use LF rather than the CRLF line endings
in the published Windows fingerprints. Verification first checks the raw files;
if that fails, it checks them again with Windows line endings for comparison
only. Other byte differences, missing files and extra files still fail.

### What it does, step by step
1. **identify:** works out whether `--disc` is a folder, an XISO or a full disc image.
2. **toolkit:** gets xboxrecomp at the pinned commit (from `--toolkit`, checked, or downloaded and hash-checked).
3. **unpack:** copies your disc's files into `<install-dir>\game\disc` (from an image, with the toolkit's own reader).
4. **fingerprint:** checks every file against `fingerprints/retail.json`. Only an unmodified retail copy passes.
5. **pipeline:** translates the game's code to C with the toolkit (`tools/run_pipeline.py`), into `<work-dir>\gen`.
6. **verify:** checks the generated C against `fingerprints/generated.json`. These are hashes of our own run's output, so a match proves your build is the same as ours. The code itself is never published.
7. **build:** CMake and Visual Studio 2022, 64-bit, Release.
8. **stage:** copies the executable, the launcher and the files it runs with into `<install-dir>`, with a small `install.json` that records the versions and your copy's fingerprint.

### Exit codes
| Code | Meaning |
|---|---|
| 0 | Done. |
| 2 | Not a supported, unmodified retail copy. |
| 3 | A dependency is missing; the message says which. |
| 4 | The translation (pipeline) step failed. |
| 5 | The generated code doesn't match ours (usually a different package or toolkit version). |
| 6 | The build failed. |
| 7 | Not enough disk space, or a file couldn't be read or written. |

## 4. Play
Run the launcher from the install folder (`Blood Wake.cmd`, or `python tools\launcher.pyw`). Pick an edition and your settings, then press Play. See [SETTINGS.md](SETTINGS.md) for every option.

## Building again, or after an update
Pull the new version of this repository and run the same command again. A rebuild doesn't touch your saves (`%LOCALAPPDATA%\xboxrecomp\`, Remastered's in `editions\remastered`) or the launcher's settings (`%LOCALAPPDATA%\BloodWake\settings.ini`). The compiled shader cache is in `%LOCALAPPDATA%\xboxrecomp\shadercache` and rebuilds itself if needed.

## For developers
- The generated C lives in the work folder and must never be committed or shared. The commit guard blocks it, along with game files, images and executables: enable it in your clone with `git config core.hooksPath tools/git-hooks`.
- `src/toolkit_patches/` holds patched copies of a few toolkit runtime files, compiled in place of the toolkit's own. Each has a `.diff` showing the exact change, and CMake checks the SHA-256 of the toolkit file it was made from, so a toolkit change stops the build until the patch has been checked again.
- The CMake option `BW_GEN_DIR` points the build at generated C outside the source tree; `build_from_disc.py` sets it for you.
- Regression commands and the latest recorded results are in [VALIDATION.md](VALIDATION.md).
