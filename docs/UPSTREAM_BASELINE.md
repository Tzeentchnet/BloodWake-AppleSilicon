# Upstream gameplay-fix baseline

The Apple Silicon branch records its Blood Wake upstream baseline in
`fingerprints/upstream_baseline.json`. The baseline is intentionally separate
from the pinned `xboxrecomp` toolkit revision documented in
`UPSTREAM_ISSUES.md`.

The recorded Blood Wake upstream commit is
`e631edddfe56d22344e9f696923aeb6d78a9e7ba`. Its `src/bw_fixes.def` is
byte-identical to this branch and contains 42 registered changes. Original
keeps every registered change disabled. Remastered initializes every applicable
entry from the registry's explicit default and then applies strict `BW_FIX`
overrides.

Run the offline report from the repository root:

```sh
python3 tools/check_upstream_parity.py
```

The check needs no network access. It always verifies the recorded registry
hash and count, every settings-to-fix reference, and the native CMake source
wiring. If the recorded upstream commit is present in the local Git object
database, it also compares the upstream registry bytes and reports branch
divergence:

```sh
python3 tools/check_upstream_parity.py --require-upstream
```

## Classified upstream-only commits

Two commits after the branch's merge base are not merged:

- `6a4f115efa32286b9b73b681939e60754185daf4` adds a one-time Windows/DX11
  shader warm-up during installation.
- `e631edddfe56d22344e9f696923aeb6d78a9e7ba` corrects that warm-up's
  fullscreen, advanced-setting, scene, cache-counting, and install-record
  behavior.

Neither commit adds a gameplay fix, edition registry entry, or Metal renderer
feature. The DX11 shader-cache workflow must not be run against the native
Metal build. A future native pipeline warm-up requires its own launch-trace
evidence and Metal pipeline-cache design. The generic rule that an optional
cache/install-record rewrite must not invalidate a successful installation
should be retained by any future native installer.

When upstream changes, update the manifest only after reviewing the commit
range, matching every registry/settings/build-wiring change, running the
parity and support-port tests, and documenting any deliberately unported
platform-specific work.
