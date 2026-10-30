# Synthetic callback and DPC retirement experiment

`retirement_model.c` is a bounded **test-only** state machine, not a runtime
implementation. It models one global retirement domain, one IRQ line, and at
most 16 outstanding work items. It does not run guest code, access devices,
enable MMIO, or prove that a real device can be retired.

## Contract exercised

One mutex serializes external admission, queue publication, cancellation,
transitions, closure, and the final readiness check. External IRQs, guest
DPCs and device accesses must acquire a token *before* becoming observable or
touching resources. Repeated posts to the pending modeled IRQ coalesce;
claiming the IRQ moves its token to a dispatcher-local state before clearing
the pending state. A later post can create a second pending token. DPC pop
likewise changes the token from queued to executing, so an empty queue does
not imply quiescence. Canceling a queued token does not cancel an executing
one, and a DPC can requeue the same identity after pop.

Closure rejects new external admission, while an already executing callback
or admitted access can publish a continuation under the same mutex, even
after closure. The parent stays outstanding until publication succeeds or the
caller handles failure. Duplicate queued DPC identities share one already
tracked queue entry in this single global domain. Full capacity rejects an
admission without changing existing work; there is no implicit dropping or
completion. Domain and slot generations reject stale, duplicate and
cross-domain token operations, including reuse of the model's storage.
Successful calls returning a token require a valid output pointer; failure
does not publish a token. Tokens are cooperative bookkeeping, not unforgeable
capabilities. Only the actual owner may complete work, and it must call
`rm_finish` after its **last** resource access. Similarly, cancellation may
release a queued token only after the actual work has been prevented from
starting. The synthetic transitions do not enforce those external obligations.

`rm_try_retire` is nonblocking and idempotent. It succeeds only after closure,
a caller assertion that the admission producers have been stopped and joined,
and completion or cancellation of **every** modeled item, including claimed
IRQs and executing callbacks. It never waits while holding the model mutex.
Callback writes sequenced before `rm_finish` are ordered before a successful
readiness check through that mutex; independent concurrent accesses still
need their own synchronization. Retirement is not destruction: before
`rm_destroy`, the caller must also prevent and join *all* threads that may
enter any model API, including shutdown callers.

The tests cover pending-to-local IRQ handoff, coalescing, late continuations,
queued and executing DPCs, requeue and cancellation, full capacity, stale
tokens, concurrent admission/closure, continuation/closure, pop/cancel, and
two shutdown callers. A blocked DPC is released by a separate service worker
after closure; the experiment leaves that worker available rather than
waiting for a callback while holding its gate or stopping its dependency.
CTest's timeout bounds scheduling failures. The tests do not establish
fairness or guarantee that arbitrary continuations eventually terminate.

For example, with the pinned toolkit and a compatible local macOS SDK:

```sh
mkdir -p build/retirement-scratch
export TMPDIR="$PWD/build/retirement-scratch"
cmake -S tests/mmio_host -B build/retirement-check \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
cmake --build build/retirement-check --target \
  bw_retirement_model_test bw_irq_shutdown_dpc_probe
ctest --test-dir build/retirement-check --output-on-failure --no-tests=error \
  -R '^(retirement_reference_model|irq_shutdown_dpc_)'
```

The three `irq_shutdown_dpc_*` cases are real-bridge **counterexamples**:
passing means unsafe behavior was observed, not that retirement works.
Select an installed SDK compatible with the local compiler and linker.

## What remains unproved

The model assumes **all** relevant external entry points, independent guest
device accesses, IRQ deliveries (including gated, ungated, local and coalesced
paths), and asynchronous continuations participate before their first access.
The carried runtime has no such global admission boundary. Guest DPC entries
contain guest addresses and arguments, not a trustworthy device/domain token;
guest-created or shared DPCs, timers, callbacks, cached pointers and work
created indirectly cannot simply be attributed to an APU. A per-device
counter, a pending bit, or `KeRemoveQueueDpc` is therefore not a substitute.

The producer-joined assertion is external, not independently verified. In the
real runtime a frame worker can both produce interrupts and provide service
needed by a DPC; the model does not show how to close its admission while
keeping the necessary service alive or how to break real gate/worker cycles.
Actual teardown also needs guest threads, timer and diagnostic workers,
interrupt registrations, device MMIO and DMA mappings, memory aliases,
in-flight device accesses, and shutdown ownership/join semantics coordinated
without lock-order deadlocks. No real cancellation, draining, data ownership,
guest-memory lifetime, or eventual progress is established here. On failure
or inability to prove quiescence, the safe production policy remains to retain
resources through process exit after guest entry. USB and APU MMIO stay
disabled.
