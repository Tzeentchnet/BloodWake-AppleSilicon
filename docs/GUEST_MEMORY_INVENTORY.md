# Guest-memory access inventory

`tools/guest_memory_inventory.py` produces a reproducible **candidate** index
for planning explicit RAM/MMIO dispatch. It does not change the game, generated
code, toolkit, builds, or runtime behavior, and is not a dispatch implementation.
It reads sources, not guest data. Its report is private: paths, identifiers,
hashes, and locations can reveal information about generated game code.

## Run

From the repository root, with the verified prepared generated directory and
a **clean Git checkout** of the pinned toolkit:

```sh
umask 077
mkdir -p "$HOME/bw-private-diagnostics" &&
mkdir "$HOME/bw-private-diagnostics/memory-inventory-001" &&
python3 tools/guest_memory_inventory.py \
  --gen game/prepare/work/gen \
  --toolkit build/xboxrecomp \
  > "$HOME/bw-private-diagnostics/memory-inventory-001/inventory.json"
```

The report is written to stdout; errors go to stderr with nonzero exit status.
Choose a new private output directory outside the repository, prepared tree
and toolkit. The command above creates a fresh owner-only subdirectory and
owner-only report under the shell's umask. Check ACLs, parent-directory
permissions and backup settings as appropriate. Do not publish the report,
generated code, or the user's game assets. A shell redirection creates its
destination even when verification fails; do not reuse an existing report.

On macOS, the native-ocean translator normally creates a separate generated
include in the build tree. Supply the actual include used by the build when it
exists, for example:

```sh
python3 tools/guest_memory_inventory.py \
  --gen game/prepare/work/gen \
  --toolkit build/xboxrecomp \
  --derived build/bloodwake-native-integrated/bw_ocean/bw_ocean_native.inc \
  > "$HOME/bw-private-diagnostics/memory-inventory-001/with-ocean.json"
```

`--derived` is repeatable for separately generated C-family source files with
distinct filenames. These files are hashed and scanned but are **not verified
against the published generated fingerprint**; their presence, hashes and
verification status are recorded. The tool does not generate them or establish
which build uses them. If none are supplied (or present as the known Windows
include in `--gen`), `derived_artifacts` is `not_provided`. Other generated
build outputs may also be missing.

## Version checks and reproducibility

* The generated directory must exactly match the published
  `fingerprints/generated.json` file set and pinned total SHA-256, with either
  exact bytes or the existing pipeline's CRLF-normalized comparison. The
  optional `bw_ocean_native.inc` is an unverified, separately hashed exception;
  other extra files and directories cause failure. This does not re-run the
  lifter or prove the origin of any other input.
* The toolkit must be the checkout root, at pinned commit
  `f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b`, with a clean Git status.
  Every scanned toolkit source is additionally compared byte-for-byte to its
  pinned Git blob, and the complete source file set is checked, including
  ignored files. Archive-only toolkit installations are not supported.
* Project `src/` is deliberately allowed to be dirty. Its scanned files are
  individually SHA-256 hashed and combined as `project_source_digest`, a
  sorted `path + NUL + sha256 + newline` digest relative to `src/`. With no
  expected value this is a snapshot, **not** a baseline verification. To
  require a known earlier snapshot, use
  `--expect-project-sha256 <64-lowercase-hex-digits>`; a mismatch fails before
  output. The digest excludes build configuration, tests and Python tools.
* The JSON also includes raw file hashes, schema version, fingerprint-manifest
  SHA-256 and tool SHA-256. Source trees, the tool and manifest are checked for
  changes while scanning. Identical bytes and arguments produce byte-identical
  JSON; providing an expected project digest changes the verification metadata.
  This is a reproducibility check, not an atomic filesystem snapshot or a
  defense against adversarial concurrent modification.

The scan includes `.c`, `.h`, `.m`, `.mm`, and `.inc` files recursively in
project `src/` and toolkit `src/`, the fingerprinted generated directory, and
explicit derived files. It excludes any project `src/recomp/gen` copy in favor
of `--gen`, and includes both project-carried toolkit replacements and their
original toolkit counterparts. It does not select the active CMake target,
expand includes or preprocess conditional branches. Non-source files in the
generated fingerprint are verified and listed but not scanned.

## Reading the report

Each `sites` entry has a domain, relative path, one-based line and column,
category, symbol, syntactic operation, width (bytes, if inferred), span and
`certainty: candidate`. Source text and address expressions are not emitted.
`counts` aggregates these entries, **not instructions or runtime accesses**:
macro definitions, helper implementations, calls, repeated uses and both sides
of replacement files can all be counted. Functions declared like calls can
also be candidates. Review the referenced private source before acting.

The inventory recognizes the pinned generated scalar lvalue helpers (including
signed and floating forms), project/native-ocean variants, stack helpers,
XMM/MMX memory helpers and some file-local macro wrappers. It distinguishes
simple assignment, compound assignment, increment/decrement and address
escape. A macro passed inside another call is reported as
`unknown_lvalue_context`: that call could itself be a mutating macro. Even
`read_candidate` and the other operation labels are heuristic, not guarantees.
An x86 read-modify-write may be emitted as separate reads and writes.

Pointer-origin mentions, syntactically plausible dereferences and subscripts,
common bulk/string calls, atomics and vector intrinsics are also listed as
candidates, including many operations on host-only memory. Some simple typed
pointer dereferences receive width and read/write/RMW hints; unknown pointer
forms remain unknown. Bulk operations
retain copy/fill/compare/scan hints and literal byte lengths where recognized;
string lengths may be bounds rather than bytes actually touched. Most pointer
widths, address provenance, atomic semantics, and dynamic lengths remain
unknown. The generated `g_fp_stack` host-array subscript is intentionally
excluded. Guest-like helper names not understood by the scanner are listed as
`unrecognized`; this name-based warning cannot find every unknown construct.

Multi-byte scalar/vector and bulk records are marked as potentially spanning.
For a simple constant guest VA and known width, `constant_span` indicates
whether the mathematical range crosses a 4 KiB or 16 KiB boundary or wraps
32-bit guest address space. A few literal `XBOX_PTR`-style bulk operands get
the analogous `constant_operands`. Missing fields mean **unknown**, not safe.
These boundary hints do not resolve MMIO regions, alignment, actual transaction
granularity, zero-length behavior of every API, or whether the range executes.

The scanner is lexical, not a C parser, compiler, points-to analysis or
exhaustive audit. Indirect aliases, pointers passed across functions or stored
in structures, custom helpers, inline assembly, unusual macro expansions,
other APIs, code in excluded directories, and accesses from dependencies may
be missed. Comments and strings are masked, but malformed or unusual syntax
may cause further misses or false positives. Unknowns must be investigated
independently; a successful scan or zero candidates does **not** prove dispatch
coverage or make USB/APU MMIO safe to enable.

## Focused tests

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover \
  -s tests -p test_guest_memory_inventory.py -v
```

The tests use only synthetic sources and a synthetic local Git checkout under
`build/`. They cover lvalue writes, read-modify-write, escaped addresses,
multiline and nested expressions, derived includes, direct pointers, bulk and
string operations, vector/floating/atomic examples, boundary/wrap hints,
unknown forms, fingerprint and toolkit changes, and report repeatability.
