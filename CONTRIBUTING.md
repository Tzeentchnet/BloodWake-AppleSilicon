# Contributing

Thanks for wanting to help. Bug reports, test results and fixes to our own code are all welcome.

## Issues
- **Welcome:** build problems, crashes, incorrect rendering or audio, settings
  that do not behave as documented, and unclear documentation. Include the
  commit, Mac model, macOS version, title edition, exact runtime switches,
  reproduction steps, expected behavior, and observed behavior.
- **Not welcome:** requests for the game, its files, disc images, builds or the generated C, and links to any of them. Such issues are closed and links removed. Please don't ask how to rip a disc or get around any protection, either: the project doesn't help with that. <!-- audit:allow off-limits -->
- **Private evidence:** do not post screenshots, recordings, saves, captures,
  retail-derived traces, generated code, prepared game data, or logs that
  reveal private paths. Describe the behavior and provide aggregate
  diagnostics instead.

## Pull requests
- Change only this project's own files, and keep each change small and focused.
- **Never commit anything from the game**, or anything made from it: no game files, extracted assets, generated C (`src/recomp/gen/` or your work folder), executables, captures or screenshots. Turn the commit guard on in your clone first: `git config core.hooksPath tools/git-hooks`.
- **Original stays exactly as shipped.** Any change to what the game does goes behind the edition switch (Remastered) with its own on/off setting. Fixes for problems the port itself causes are the exception.
- Preserve explicit failure behavior, bounded diagnostics, and existing
  renderer/input ownership contracts. Add focused tests and run the smallest
  relevant suite before broader validation.
- By contributing you agree your work is released under this project's MIT licence (or, for files in `src/xemu_dsp/`, the GPL-2.0-or-later).

## Takedown
If you represent a rights holder and believe this repository contains
material that should be removed, use GitHub's
[DMCA takedown process](https://docs.github.com/en/site-policy/content-removal-policies/guide-to-submitting-a-dmca-takedown-notice).
Do not post personal, confidential or game-derived evidence in a public issue.
