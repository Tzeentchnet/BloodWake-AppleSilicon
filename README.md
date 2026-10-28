# Blood Wake Recomp

A native Windows version of **Blood Wake** (Xbox, 2001), made by static recompilation: the game's own program is translated into C and compiled for your PC, on your PC, from **your own copy of the game**. It is not an emulator, and this repository contains nothing from the game.

> **Unofficial fan project.** Not affiliated with, endorsed by or connected to Microsoft, Xbox Game Studios or Stormfront Studios. Blood Wake and everything in it belongs to its rights holders.

## Good faith
- **You must own the game.** The build accepts only an unmodified retail copy, made from your own disc, and checks it against published fingerprints (SHA-256 hashes) before it does anything else.
- **Nothing from the game is here:** no game code, no generated C, no executables, no art, sound, music, video, text, screenshots or extracted files. Your PC translates and compiles the game itself, with Microsoft's own build tools; we publish fingerprints of the generated code (never the code) so your build can prove it matches ours.
- **Strictly non-commercial.** Free, no donations, no ads.
- **No links to game images, and no help with ripping or getting around any protection.** The project doesn't need any of that and never touches console security, disc authentication or online services. <!-- audit:allow off-limits -->
- **Credit to the original team.** Blood Wake was made by Stormfront Studios and published by Microsoft Game Studios. See [CREDITS.md](CREDITS.md).
- **Taken down at once if Microsoft asks.** See [Contact and takedown](#contact-and-takedown).
- **If Microsoft re-releases Blood Wake**, this page will point there first. Buy it.

## What you get
One program with two editions, picked in the launcher:

- **Original:** the game exactly as it shipped in 2001, its quirks and bugs included. Only problems caused by the port itself are fixed.
- **Remastered:** the same game, with the original art and sound, looking, running and sounding its best on a modern PC:
  - widescreen (16:9 and 21:9 ultrawide, more of the world at the sides, nothing stretched) with the HUD in the corners;
  - native resolution up to 4K, and a higher frame rate (60, 120, 144 or 240 fps, or your display's refresh rate) with the game still running at its own speed;
  - anti-aliasing, sharper textures at an angle, longer detail distance, smoother gradients, sharp text and HUD;
  - optional extras that are off by default because the original didn't have them: ambient occlusion, bloom, sharpening, edge smoothing, HDR, and an OLED care mode;
  - cleaner sound: higher-quality resampling, a slightly louder mix with a limiter, and the game's surround mix on 5.1/7.1 speakers;
  - the original game's own bugs fixed, each with its own switch (a mission that could never be won, sounds that never played, missing textures and more).

  Every Remastered change can be switched off one by one. The art, models, sound and music are never replaced or modified. Details: [docs/EDITIONS.md](docs/EDITIONS.md) and [docs/SETTINGS.md](docs/SETTINGS.md).

A third edition, **Expanded**, is planned: the developers' cut and half-finished content, completed. It isn't in this repository yet. <!-- audit:allow expanded -->

Story Mode, Battle Mode and couch split screen for 2 to 4 players work in both editions. Each edition keeps its own saves, under `%LOCALAPPDATA%\xboxrecomp\` (Remastered's in `editions\remastered` there); the launcher keeps its settings in `%LOCALAPPDATA%\BloodWake\settings.ini`.

## What you need
- **Windows 10 or 11, 64-bit**, and a graphics card with DirectX 11.
- **Your own Blood Wake disc**, copied to your PC as one of: a folder of the disc's files, an Xbox ISO (XISO), or a full disc image. Only the retail release listed in [`fingerprints/retail.json`](fingerprints/retail.json) is accepted.
- **Disk space:** several GB during the build (the game's files alone are about 2 GB).
- An Xbox-style controller (XInput), or the keyboard.

The build also needs these free tools. The installer fetches any that are missing, from their official sites only:
- **Python 3.13** (64-bit, with tkinter) and the packages pinned in [`tools/requirements.txt`](tools/requirements.txt), notably `capstone==5.0.9` and `pefile==2024.8.26`;
- **Visual Studio 2022 Build Tools** (version 17) with the C++ workload, and **CMake** 3.20 or later. Visual Studio 2026 is not supported yet;
- the **xboxrecomp** toolkit at a pinned commit, downloaded by the build script from its GitHub page and checked against a pinned hash.

## Install
### With the installer (recommended)
> **Windows may warn you:** the installer isn't code-signed, so SmartScreen can show "Windows protected your PC". Choose **More info**, then **Run anyway**. Each release lists the installer's SHA-256, so you can check you have the real file. The build takes about 15 minutes and can't be cancelled once it starts.

1. Download `BloodWakeRecomp-Setup-<version>.exe` from this repository's [Releases](https://github.com/minimalice92/bloodwake-recomp/releases) page.
2. Run it. It installs for your Windows user only and doesn't need administrator rights, except for the one step that installs Microsoft's build tools, if you don't have them.
3. Read and accept the disclaimer, confirming you own the game and made the copy from your own disc.
4. Point it at your copy of the disc (a folder, an XISO or a full image). It tells you in plain words whether the copy is recognised.
5. Pick where to install (by default `%LOCALAPPDATA%\Programs\Blood Wake Recomp`).
6. Let it build. Translating and compiling the game takes a while; a progress bar shows each step.
7. Play from the Start menu shortcut, which opens the launcher.

The uninstaller (in Windows' **Apps** settings) removes the build and its shortcuts and asks about the rest; your saves and settings are kept unless you choose otherwise.

### By hand
```
git clone https://github.com/minimalice92/bloodwake-recomp.git
cd bloodwake-recomp
python -m pip install -r tools/requirements.txt
python tools/build_from_disc.py --disc "<your disc folder or image>" --install-dir "<where to install>"
```
Then start the launcher from the install folder. Every option, step and exit code is in [docs/BUILDING.md](docs/BUILDING.md).

## Playing
The launcher has a tab per area (Edition, Display, Graphics, Audio, Fixes, Controls) and a Play button. Alt+Enter or F11 switches between a window and borderless fullscreen while you play.

## Troubleshooting
| What you see | What to do |
|---|---|
| "not a supported unmodified retail copy" (exit code 2) | Only an unmodified retail copy of the release in `fingerprints/retail.json` is accepted. A copy that was patched, trimmed or re-packed won't match. Copy your disc again. |
| A missing dependency (exit code 3) | The message names it. Install it (see What you need) or run the installer, which fetches it. |
| The translation step failed (exit code 4) | Check the Python packages are exactly the pinned versions (`python -m pip install -r tools/requirements.txt`). |
| "generated code doesn't match" (exit code 5) | Your translation came out different from ours: usually a different version of a Python package or of the toolkit. Reinstall the pinned versions and try again. |
| The build failed (exit code 6) | Make sure Visual Studio 2022 (17.x) Build Tools with the C++ workload is installed. |
| Disk space or a file error (exit code 7) | Free some space, or pick an install folder you can write to. |
| The game is slow | In the launcher, lower the rendering resolution or the frame rate cap, or turn off anti-aliasing. |
| Crackling or stuttering sound on a slow PC | Audio tab: Sound processor, Simple (it skips the sound chip's echo and 3D effects). |

If you're stuck, open an issue: say what you did, what you saw, and your Windows and graphics card. Please don't attach game files, screenshots of the game's text, or your disc image.

## Contact and takedown
The maintainer is [@minimalice92](https://github.com/minimalice92) on GitHub. Questions and bug reports: open an issue.

**Rights holders:** if you are Microsoft or act for the rights holders and want this project taken down, open an issue titled **Takedown** (or contact the maintainer through GitHub). It will be taken down at once, no questions asked.

## Credits and licence
Blood Wake is by **Stormfront Studios**, published by **Microsoft Game Studios**. This project stands on [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and on [xemu](https://xemu.app)'s work on the Xbox's sound chip. Full credits: [CREDITS.md](CREDITS.md).

Our own code is under the MIT licence ([LICENSE](LICENSE)); some parts are under the GPL or LGPL ([NOTICE.md](NOTICE.md), [LICENSES/](LICENSES/)). Please read [LEGAL.md](LEGAL.md) before you use it, and [CONTRIBUTING.md](CONTRIBUTING.md) before you open an issue.
