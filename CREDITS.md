# Credits

## Blood Wake
**Blood Wake** (Xbox, 2001) was developed by **Stormfront Studios** and published by **Microsoft Game Studios**. All of its code, art, sound, music, voice acting and writing is their work and belongs to its rights holders. This project exists because their game is worth keeping playable. Thank you to everyone who made it.

If Microsoft ever re-releases Blood Wake, play that version and support it.

## The tools this port stands on
- **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nz (MIT): the static recompiler that translates an Xbox game's program into C, and the runtime it runs on (the Xbox system calls, memory, graphics, sound and controllers). This port uses it at a pinned commit, plus a few carried fixes in `src/toolkit_patches/`. xboxrecomp in turn includes code from xemu (see its own `NOTICE`).
- **[xemu](https://xemu.app)** ([source](https://github.com/xemu-project/xemu)), the Xbox emulator: its MCPX DSP56300 interpreter runs the Xbox sound chip's own programs (echo and 3D sound) in `src/xemu_dsp/` (GPL-2.0-or-later), and its MCPX APU code reaches this port by way of xboxrecomp (LGPL-2.1-or-later). Copyright espes, Jannik Vogel, Matt Borgerson and the xemu contributors.
- **Hatari** and the **ARAnyM** developer team (Thomas Huth among them), whose DSP56001 emulation xemu's DSP interpreter was adapted from.
- **[Capstone](https://github.com/capstone-engine/capstone)** (the x86 disassembler) and **[pefile](https://github.com/erocarrera/pefile)**, used by the toolkit's pipeline.
- **Python**, **CMake**, and Microsoft's **Visual Studio Build Tools**, **Windows SDK**, **Direct3D 11**, **XInput** and **XAudio2**, which the port is built with and runs on.

## This project
Made by the bloodwake-recomp contributors and maintained by [@minimalice92](https://github.com/minimalice92). Much of the engineering was done with the help of Claude, Anthropic's AI assistant, working from captured data and checked against the original game.

Thanks also to the people who keep the Xbox's history documented and its games playable.
