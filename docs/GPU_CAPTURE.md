# GPU capture/replay foundation

This is a **synthetic fixture facility**, not a live game capture. There is no
runtime capture switch or production replay executable. The fixture owns all
guest memory, runs the executor and the CPU renderer on one thread, and
explicitly records every input-memory write and synchronization point. The
executor observer is compiled only with `BW_GPU_CAPTURE_FIXTURE`; normal
builds, including those with `BW_GPU_AUDIT` disabled, do not enable it.

## What is replayed

The version-1 stream preserves the order of:

- executor `(subchannel, method, parameter)` calls, including state and bulk
  method data;
- explicit guest-memory byte snapshots to apply before subsequent methods;
- semaphore-target changes, executor/backend barrier requests, and expected
  guest-memory bytes.

The synthetic test attaches before the first executor method and replays in a
fresh process. It covers pitch-linear color clears, screen-space triangle
arrays, changed vertex colors between draws, two CPU-rendered color surfaces,
16-bit and 32-bit indices, synchronous semaphore releases, a flip, and full
surface/semaphore comparisons. Its strict replay adapter preflights its narrow
method and memory profile and rejects unknown methods, missing inputs, missing
barriers, unchecked writes and unsupported barrier kinds. It does not silently
skip an unsupported operation. The test also runs the existing `BW_GPU_AUDIT`
alongside capture and checks that the original and replay produce the same
audit report. The audit remains a **usage summary**, not a recording.

An observer refuses a nonempty recorder, attachment after the first method, or
attachment while a semaphore target is already set. Capture requires one
owner thread; while attached, a
method, target change or detach on another thread invalidates the entire
recording, and only the owner can detach it. While attached,
the executor's `ARRAY_ELEMENT16` bulk shortcut falls back to individual calls
so their order cannot be lost. Explicit memory events must be recorded at the
same point that the synthetic owner changes memory. A barrier event records a
synchronization the producer actually performed; merely inserting one does
not synchronize a backend.

## Bounds and privacy

Each recorder has caller-selected limits no larger than 16 MiB and 262,144
events; an individual memory event is at most 4 MiB. Limit, thread, argument,
or I/O errors are sticky and prevent writing a completed trace. Recorders
cannot be finalized or destroyed while attached. Memory buffers are cleared
before release.

The writer requires an explicit `.bwgpu` path, creates it exclusively with
owner-only permissions, does not follow a final-component symlink, and never
overwrites an existing file. The reader also requires a private,
single-link regular file owned by the current user. A partial write is
truncated on a detected write failure where possible. Captures contain raw
memory and method values: store them only in a private, trusted directory,
remove them when no longer needed, and never publish them. The extension is
ignored by git and blocked by the repository's commit guard, but those
protections are not a substitute for private storage. Parent-directory
permissions, ACLs, backups and other processes with the same user identity
remain the operator's responsibility.

The file has a version, source-profile marker, bounded lengths/counts and an
end marker and checksum. The checksum detects accidental corruption; it is
not authentication or proof of provenance. Before any replay effects, the
reader validates the entire file and asks the selected replay adapter to
preflight **every** event and its final state. A backend or comparison error
stops execution; already executed events are not rolled back, so discard that
replay context. A well-formed file alone does not establish that its producer
recorded all inputs or that a replay adapter models them correctly.

## Run the synthetic tests

With the pinned toolkit available at `build/xboxrecomp`:

```sh
mkdir -p build/gpu-capture-scratch
export TMPDIR="$PWD/build/gpu-capture-scratch"
cmake -S tests/gpu_capture -B build/test-gpu-capture
cmake --build build/test-gpu-capture
ctest --test-dir build/test-gpu-capture --output-on-failure
```

Pass `-DXBOXRECOMP_DIR=/path/to/pinned/xboxrecomp` if needed. The test creates
only synthetic capture files in its build directory and removes them.

## Coverage boundary and remaining work

This is not a capture of boot, menu, mission, or any real guest workload, and
the CPU evaluator is not a validated NV2A hardware reference. There is no
automatic observation of guest CPU writes, resource lifetime or aliases,
vertex programs, textures, depth/stencil, gamma, MMIO reports, asynchronous
backend ownership/readback, live fences, DMA mapping changes, or guest/host
thread synchronization. The generic stream can represent ordered events, but
its callbacks alone do not supply those missing semantics. The fixture
adapter rejects them rather than treating an incomplete stream as a replay.

Live capture needs a separately designed coherent, pinned memory and GPU
ownership protocol, actual backend synchronization/error propagation,
comprehensive input and state coverage, and a validated reference path. Do not
enable this fixture observer in the real guest while those contracts are
absent.
