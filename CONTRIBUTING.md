# Contributing

Thanks for wanting to help. Bug reports, test results and fixes to our own code are all welcome.

## Issues
- **Welcome:** build problems, crashes, things that look or sound different from the original game, settings that don't do what they say, and documentation that's unclear. Say what you did, what you expected, what you saw, and your Windows version, graphics card and the build script's exit code if there was one. The build log is in `<work-dir>\logs\`.
- **Not welcome:** requests for the game, its files, disc images, builds or the generated C, and links to any of them. Such issues are closed and links removed. Please don't ask how to rip a disc or get around any protection, either: the project doesn't help with that. <!-- audit:allow off-limits -->
- **Screenshots:** please don't post screenshots or recordings of the game. Describe what you see instead (for example "the water in mission 3 flickers at the horizon").

## Pull requests
- Change only this project's own files, and keep each change small and focused.
- **Never commit anything from the game**, or anything made from it: no game files, extracted assets, generated C (`src/recomp/gen/` or your work folder), executables, captures or screenshots. Turn the commit guard on in your clone first: `git config core.hooksPath tools/git-hooks`.
- **Original stays exactly as shipped.** Any change to what the game does goes behind the edition switch (Remastered) with its own on/off setting. Fixes for problems the port itself causes are the exception.
- By contributing you agree your work is released under this project's MIT licence (or, for files in `src/xemu_dsp/`, the GPL-2.0-or-later).

## Takedown
If you represent the rights holders, see the [README](README.md#contact-and-takedown).
