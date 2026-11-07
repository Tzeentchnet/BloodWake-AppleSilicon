# Metal pipeline-artifact contract

Status: PERF-029 compatibility contract, PERF-031 trace-key payload, and
PERF-032 rare-miss lifecycle

This contract defines how Metal pipeline warm-up identifies and rejects local
artifacts. PERF-031 records bounded canonical keys and preloads compatible
states. PERF-032 uses the validated preload as the opt-in boundary for bounded
asynchronous compilation of untraced specializations. No binary archive is
created or loaded, and the 128-entry draw-cache policy is unchanged.

## Artifact classes

Project-owned offline shader output is a normal build product. It is not a
`.bwmpa` file and must not contain game-derived inputs.

The project-owned stable source is `src/gpu/bw_metal_shaders.metal`. The build
produces default-math and safe-math `.metallib` variants when the selected
Xcode toolchain provides the Metal compiler, then embeds them in the
executable. Their source identity is SHA-256
`3fda260b7e0584eeea7d6a6d55229854dd67b310a447f9c1b45e21f617a2dec7`.
Builds without those tools embed the identical source bytes and retain
explicit runtime compilation; they do not create a success-shaped offline
artifact.

Trace-derived key sets and compiled binary archives are private, device-local
`.bwmpa` files. They must live under an owner-controlled host cache directory,
outside the source tree, prepared game tree, saves, and diagnostic output
intended for publication. The repository ignores `.bwmpa` files and its
pre-commit guard rejects them, but those safeguards do not replace correct
private storage.

The version-1 artifact kinds are:

- `1`, trace-derived pipeline keys; and
- `2`, a device-built Metal binary archive.

PERF-029 defines and validates the common envelope. PERF-031 adds the
production writer, kind-1 trace-key payload, and bounded preload consumer.
Kind 2 remains contract-defined but is not produced or loaded.

## Trace-key payload

Kind 1 uses pipeline-key schema version 1. Its payload is a sequence of
fixed 228-byte records. Every record contains 57 little-endian 32-bit words:

| Words | Field |
| ---: | --- |
| 0 | Shader classification |
| 1 | Canonical color-write mask |
| 2 | Fixed-alpha-zero classification |
| 3-6 | Blend enable, source, destination, and operation |
| 7 | Depth-attachment classification |
| 8 | Specialized-function classification |
| 9-12 | Four canonical texture-stage words |
| 13-16 | Four canonical stage-routing words |
| 17-20 | Four canonical dot-product words |
| 21-52 | Eight groups of four canonical combiner words |
| 53-54 | Two final-combiner words |
| 55-56 | Two alpha-test words |

Records are sorted by their encoded bytes and must be strictly increasing, so
duplicates and noncanonical ordering are rejected. The payload size must equal
the envelope entry count times 228. PERF-031 further caps kind-1 artifacts at
128 entries (29,184 payload bytes), matching the unchanged draw-pipeline cache
limit even though the common envelope permits 4,096 entries for future kinds.

These words are retail-derived state. They are never emitted in logs,
diagnostic JSON, source files, or committed artifacts. They exist only in the
explicitly requested private device-local `.bwmpa` file.

## Version-1 envelope

The envelope is exactly 176 bytes, uses little-endian integers, and precedes
the payload. The total file is limited to 64 MiB and the declared entry count
is limited to 4,096.

| Offset | Bytes | Field | Version-1 rule |
| ---: | ---: | --- | --- |
| 0 | 8 | Magic | `BWMPA01` followed by NUL |
| 8 | 4 | Format version | `1` |
| 12 | 4 | Header bytes | `176` |
| 16 | 4 | Artifact kind | Trace keys or binary archive |
| 20 | 4 | Flags | Device-local bit only (`1`) |
| 24 | 4 | Backend ABI | Exact match |
| 28 | 4 | Pipeline-key schema | Exact match |
| 32 | 32 | Stable shader SHA-256 | Exact source/build identity |
| 64 | 32 | Executable SHA-256 | Exact executable identity |
| 96 | 12 | OS major/minor/patch | Exact match |
| 108 | 4 | SDK version | Exact build-SDK identity |
| 112 | 4 | Metal feature level | Exact contract-defined capability level |
| 116 | 4 | GPU family | Exact selected Metal GPU family |
| 120 | 8 | Payload bytes | Nonzero and within the total-size limit |
| 128 | 4 | Entry count | 1 through 4,096 |
| 132 | 4 | Reserved | Zero |
| 136 | 32 | Payload SHA-256 | Nonzero and verified before selection |
| 168 | 8 | Reserved | Zero |

Backend ABI and pipeline-key schema both start at version 1. Changing command
ownership, artifact-facing backend semantics, or the canonical pipeline key
layout requires the corresponding version to advance. The stable shader hash
is SHA-256 over the exact project-owned stable shader bytes used by the build.
The executable hash is SHA-256 over the executable that will consume the
artifact. OS, SDK, feature-level, and GPU-family values are captured as
explicit integers by the future producer and consumer; display names and
device registry identifiers are not part of the key.

The fixed envelope contains no pipeline descriptor, combiner word,
function-constant value, texture identity, guest or host address, or raw
retail-derived state. A future trace payload remains private and must define
its own bounded encoding under the versioned pipeline-key schema. A future
binary-archive payload remains opaque Metal data bound to the complete
identity above.

## Validation and fallback

Validation requires an absolute `.bwmpa` path. The final path component must
not be a symlink. The opened object must be a regular, single-link file owned
by the current user with no group or other permissions. The validator uses
`O_NOFOLLOW`, bounds the file before reading, reads without allocation,
verifies the payload SHA-256, and confirms that file identity, size, mode,
link count, and modification state did not change during validation.

The stable machine-readable status values are:

- `compatible`;
- `invalid_argument`;
- `unsupported_platform`;
- `missing`;
- `symlink_rejected`;
- `policy_rejected`;
- `io_error`;
- `over_limit`;
- `truncated`;
- `trailing_data`;
- `invalid_format`;
- `stale`;
- `incompatible`; and
- `corrupt`.

Shader-hash or executable-hash mismatches are `stale`. Backend ABI,
pipeline-key schema, artifact kind, OS, SDK, Metal feature-level, or GPU-family
mismatches are `incompatible`. Structural errors, truncation, trailing bytes,
policy failures, and limits remain separately visible.

Only `compatible` selects the artifact. Every other status selects explicit
runtime compilation while retaining the rejection status for reporting.
Absence of the selected Metal API is likewise `unsupported_platform` plus
runtime compilation. A caller must never report artifact success after this
fallback, hide a compilation failure, or return a missing pipeline.

## Capture, preload, and reporting

`BW_METAL_PIPELINE_TRACE_OUTPUT` opts into kind-1 capture. It accepts only a
bounded absolute `.bwmpa` path. Successful draw keys are deduplicated in fixed
storage and persisted at frame boundaries. The parent directory must be owned
by the current user with no group or other permissions. Writes use a new
mode-0600, single-link regular temporary file, `fsync`, and atomic replacement;
symlinked or nonprivate targets are rejected. Capture never changes pipeline
creation or cache retention.

`BW_METAL_PIPELINE_WARMUP` opts into preload. The backend verifies the full
envelope, executable/device identity, digest, fixed payload shape, ordering,
uniqueness, and semantic key fields before creating any state. It then creates
all states before the first draw. Any load, decode, allocation, specialization,
or pipeline failure clears partial preload and uses the unchanged synchronous
runtime path.

Both switches are unset by default. They may name different files only.
Reporting is bounded to `bloodwake.metal-pipeline-warmup/v1` JSONL with
artifact status and aggregate entry/write/preload counts. It never includes a
path, key, descriptor, combiner word, function constant, texture identity, or
address.

A fully compatible preload enables asynchronous handling only for a later
specialized key that was not in the artifact. The first request creates or
reuses the existing exact generic extended pipeline, queues Metal function
specialization and render-pipeline creation, and renders through the generic
pipeline until the specialized state is ready. Duplicate requests coalesce by
the unchanged canonical key. Completion handlers never mutate the draw cache;
the serial renderer installs completed states at a draw or flip boundary.

The queue retains at most 32 pending, ready, or failed entries. A full queue
keeps rendering through the generic pipeline and counts the bounded-capacity
fallback. Successful entries leave the queue after installation. Failed
entries remain retained so they cannot silently retry; function, pipeline,
allocation, and timing failures are explicit. Backend stop waits for all Metal
callbacks and drains their state before releasing the library or device.
Missing or rejected artifacts never enable this path and retain synchronous
runtime creation.

`bloodwake.metal-pipeline-async/v1` JSONL is bounded to 16 aggregate summaries.
It reports queue occupancy/capacity, scheduled and coalesced misses, generic
fallback draws, installed states, stage-specific failures, capacity
fallbacks, and specialization/pipeline timing. It contains no paths, canonical
keys or hashes, descriptors, combiner words, function constants, texture
identities, addresses, or other retail-derived values.
