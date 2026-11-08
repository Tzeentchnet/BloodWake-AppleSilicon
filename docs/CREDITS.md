# Credits and acknowledgements

## Blood Wake
**Blood Wake** (Xbox, 2001) was developed by **Stormfront Studios** and published by **Microsoft Game Studios**. All of its code, art, sound, music, voice acting and writing is their work and belongs to its rights holders. This project exists because their game is worth keeping playable. Thank you to everyone who made it.

If Microsoft ever re-releases Blood Wake, play that version and support it.

## The tools this port stands on
- **[Blood Wake Recomp](https://github.com/minimalice92/bloodwake-recomp)** by
  minimalice92 and its contributors is the upstream project this fork extends.
- **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nz (MIT): the static recompiler that translates an Xbox game's program into C, and the runtime it runs on (the Xbox system calls, memory, graphics, sound and controllers). This port uses it at a pinned commit, plus a few carried fixes in `src/toolkit_patches/`. xboxrecomp in turn includes code from xemu (see its own `NOTICE`).
- **[xemu](https://xemu.app)** ([source](https://github.com/xemu-project/xemu)), the Xbox emulator: its MCPX DSP56300 interpreter runs the Xbox sound chip's own programs (echo and 3D sound) in `src/xemu_dsp/` (GPL-2.0-or-later), and its MCPX APU code reaches this port by way of xboxrecomp (LGPL-2.1-or-later). Copyright espes, Jannik Vogel, Matt Borgerson and the xemu contributors.
- **Hatari** and the **ARAnyM** developer team (Thomas Huth among them), whose DSP56001 emulation xemu's DSP interpreter was adapted from.
- **[Capstone](https://github.com/capstone-engine/capstone)** (the x86 disassembler) and **[pefile](https://github.com/erocarrera/pefile)**, used by the toolkit's pipeline.
- **Xcode**, **Metal**, **AppKit**, **CoreAudio**, **SDL2**, **OpenSSL**,
  **Python**, and **CMake**, which support the Apple Silicon build. The
  inherited Direct3D and Windows paths remain credited through the upstream
  project.

## This fork

The original project was made by the bloodwake-recomp contributors and is
maintained upstream by
[@minimalice92](https://github.com/minimalice92). Its credits note substantial
engineering assistance from Claude, Anthropic's AI assistant. This fork
retains that work and attribution while adding the native macOS host, Metal
renderer, Apple Silicon diagnostics, and related validation.

Thanks also to the people who keep the Xbox's history documented and its games playable.
