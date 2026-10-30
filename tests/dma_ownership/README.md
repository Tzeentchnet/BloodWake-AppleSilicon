# DMA lifetime investigation

These are synthetic host-only tests. They do not boot the guest, change the
toolkit checkout, or enable device MMIO. The production runtime is **not**
wired to the test model.

## What the real-source counterexamples establish

`existing_counterexamples.c` compiles the project-owned allocator and
translation sources into a test fixture. It deliberately orders competing
threads with a condition variable so that the counterexamples are
reproducible without introducing a C data race in the fixture itself. A
passing counterexample means the unsafe behavior was observed, **not** that
the production path is safe:

* With `RECOMP_HEAP_RECLAIM` unset, a two-page heap block can be freed and
  reused at the same address while another thread retains its pointer. A
  reader also observes the first page's old name and the second page's new
  name, a combination that was never a complete map snapshot.
* With `RECOMP_HEAP_RECLAIM` present, the contiguous allocator frees and
  reallocates the same two pages while a pointer remains outstanding. The
  page bytes and high-water mark cannot distinguish the two generations.
* A live heap page and a claimed window page can have the same physical
  number. A subsequent translation changes the last-translation byte even
  while both names remain live. A physical number and that byte do not
  uniquely identify a retained mapping.

These tests do not invoke the device implementations. OHCI's separate
validation and later resolution, APU raw and cached pointers, and the NV2A
resolver have no lifetime relationship with these allocation operations.
The contiguous high-water mark can still select a freed page; the default
heap allocator also reuses whole freed blocks without the reclaim option.
`MmAllocateContiguousMemoryEx`'s exact-address bridge can zero an already
claimed range, and heap decommit and `guest_vmem_free` can zero storage
independently of allocator reuse.

## Candidate contract

`reference_model.c` exercises one conservative design, restricted to
page-aligned synthetic allocations. A single mutex serializes publication,
whole-span validation/pinning, retirement and reclamation. A lease identifies
the backing and allocation generation and pins every page in its range.
Retirement rejects new leases immediately, but the entire allocation cannot
be recycled until all its leases are released. Acquisition is all-or-nothing,
rejects overflow and mixed generations, and returns a linear pointer only
when the entire range belongs to one generation in one backing. Aliases with
two live backings require an explicit identity; an unqualified lookup is
ambiguous, including when a retired alias is still pinned. The model's
unqualified lookup is only a diagnostic snapshot, not an acquisition API;
its result must never substitute for atomic resolve-and-pin. Readers can use
a lease without holding the metadata mutex.
Threaded tests cover multi-page spans, overlapping leases, free/reuse, stale
tokens, distinct aliases, failed acquisitions and concurrent churn.

## Shared-backing physical authority experiment

`physical_authority_model.c` and `physical_authority_tests.c` exercise a
different, still synthetic candidate: every modeled physical page has exactly
one byte backing, and RAM, window and mirror views map to that same backing.
Physical claims own byte ranges, so disjoint allocations can share a page.
Exact claims validate the entire range before initializing anything; a
conflicting claim cannot zero a live or retired-but-pinned allocation.

The claim returns an authority-scoped, monotonically generated owner identity.
Each submission must supply that **original** identity and a virtual view;
there is no API that manufactures an identity from a naked physical number.
Submission validates and pins every requested range under one mutex, including
all pages, permissions and owners. A batch either acquires every lease or
changes nothing. A physically non-linear span is rejected, although the caller
can explicitly submit separate valid spans. Retirement closes admission and
defers reuse of the whole claimed range until its final lease is released;
page protection, decommit and remapping are also restricted while pinned.
Lease serials reject duplicate or stale releases, including after slot reuse.

The model returns no backing pointers. Modeled guest and device reads/writes
copy under the same mutex, so one copy is a race-free snapshot relative to
other modeled copies. A pin does **not** make data immutable across copies.
The tests cover aliases and page zero, cross-authority and stale identities,
shared-page reuse, conflicting claims, failed batches, non-linear spans,
permissions, bounds and capacity, concurrent conflicting claims, handoff and
allocation churn, duplicate release, and deferred shutdown. Shutdown stops
new admissions and tracks leases held by queued work. A separate
counterexample demonstrates that a lease-only "ready" result can coexist
with an **untracked** callback; the model's destructor also requires its
caller to join all API-entry threads.

These are bounded executable design checks, not a production implementation
or a proof of end-to-end safety. The shared-backing experiment assumes all
participants use its APIs, all physical byte owners are registered, each
caller preserves the correct identity from its claim, and copy buffers have
their own appropriate synchronization. A producer must also publish its
identity and initialized data according to a shared handoff protocol; a pin
alone does not signal that a payload or descriptor is ready. Identities are
process-local test tokens, not unforgeable capabilities or a solution for a
guest descriptor that contains only a physical number. The physical array,
64-byte model pages, global lock, uniform page permissions and lack of raw
pointers are deliberate simplifications, not representations of the runtime's
OS mappings or performance. In particular, the real RAM and contiguous
window currently use different storage for the same physical numbers, and
real guest heap blocks can share pages. The heap block table can overflow,
image/stack/reserved pages and extended virtual memory have other owners, and
mappings and protection can change outside the modeled allocator. A usable
implementation must:

1. Establish an authoritative physical-to-backing identity for every page,
   including simultaneous RAM/window aliases and mirror views. Either use
   genuinely shared physical backing with unique physical ownership or carry
   a valid backing/generation identity from submission through completion.
   The last-translation byte or a raw physical number alone is insufficient.
2. Coordinate acquisition with **all** allocation, naming, claim, release,
   decommit, protection and teardown paths. Account for shared heap pages,
   exact-address claims, page zero, image/stack, extended VM and every alias.
   Publish initialized data and metadata together, and define a lock order;
   never wait for a lease while holding a lock its release needs.
3. Pin or copy every complete DMA range before use, including descriptors,
   SGE tables and cached pointers. Reject or deliberately split non-linear
   multi-page spans. Hold the lease across asynchronous use, callbacks and
   cancellation, then release exactly once. A generation must originate at a
   point that actually knows the intended guest allocation: creating one
   later from a naked address does not resolve an ABA or alias ambiguity.
4. Quiesce all guest/device producers, queued and executing IRQs and DPCs,
   and outstanding leases before unmapping guest memory. Coordinate this with
   device shutdown; thread joins alone do not prove callback quiescence.
5. Specify guest/device data handoff and memory ordering as well as lifetime.
   Pinning prevents reuse but does not make overlapping plain C reads and
   writes race-free or make a multi-byte payload an atomic snapshot. Every
   CPU/device access involved must obey the same supported synchronization
   protocol, including accesses emitted by recompiled guest code.

Until those requirements are met, keep guest USB and APU MMIO disabled; do
not infer that the existing default heap mapping is safe for asynchronous DMA.

## Running

From the repository root, using an installed SDK compatible with the linker:

```sh
cmake -S tests/dma_ownership -B build/test-dma-ownership \
  -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
cmake --build build/test-dma-ownership
ctest --test-dir build/test-dma-ownership --output-on-failure
```

For separate ThreadSanitizer or ASan/UBSan builds, use respectively
`-DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'
-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'` or
`-DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
-DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'` and a different
build directory. On this macOS ASan runtime, leak detection is unsupported;
run its tests with `ASAN_OPTIONS=detect_leaks=0` rather than claiming leak
coverage. The focused 13-test suite passed normally, under TSan, and under
ASan/UBSan with leak detection disabled on the SDK 26.5 build.
