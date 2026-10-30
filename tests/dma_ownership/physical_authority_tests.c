#include "physical_authority_model.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

#define EXPECT(expression, expected) do { \
    PmResult actual_ = (expression); \
    PmResult expected_ = (expected); \
    if (actual_ != expected_) { \
        fprintf(stderr, "%s:%d: %s returned %d, expected %d\n", \
                __FILE__, __LINE__, #expression, actual_, expected_); \
        abort(); \
    } \
} while (0)

static void mutex_lock(pthread_mutex_t *mutex) { CHECK(!pthread_mutex_lock(mutex)); }
static void mutex_unlock(pthread_mutex_t *mutex) { CHECK(!pthread_mutex_unlock(mutex)); }

static PmModel *mapped_model(void)
{
    PmModel *m = pm_create();
    CHECK(m);
    for (unsigned page = 0; page < PM_PHYSICAL_PAGES; page++)
        EXPECT(pm_commit_page(m, page), PM_OK);
    for (unsigned view = 0; view < PM_VIEWS; view++)
        for (unsigned page = 0; page < PM_VIEW_PAGES; page++)
            EXPECT(pm_map_page(m, view, page, page), PM_OK);
    return m;
}

static void finish(PmModel *m)
{
    EXPECT(pm_begin_shutdown(m), PM_OK);
    EXPECT(pm_shutdown_ready(m), PM_OK);
    EXPECT(pm_destroy(m), PM_OK);
}

static void test_shared_aliases(void)
{
    PmModel *m = mapped_model();
    PmModel *other = mapped_model();
    PmOwner owner, foreign, zero, conflict = { UINT64_MAX, UINT64_MAX };
    PmOwner untouched = conflict;
    PmLease lease, page_zero, rejected, rejected_before;
    PmRequest request;
    uint8_t source[32], received[32], value = 0x6a;

    EXPECT(pm_claim(m, PM_PAGE_SIZE + 4, 48, &owner), PM_OK);
    EXPECT(pm_claim(other, PM_PAGE_SIZE + 4, 48, &foreign), PM_OK);
    CHECK(owner.generation == foreign.generation && owner.authority != foreign.authority);
    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i ^ 0xa5);
    EXPECT(pm_guest_write(m, owner, 0, PM_PAGE_SIZE + 8, source, sizeof(source)), PM_OK);
    EXPECT(pm_guest_read(m, owner, 1, PM_PAGE_SIZE + 8, received, sizeof(received)), PM_OK);
    CHECK(!memcmp(source, received, sizeof(source)));

    request = (PmRequest){ owner, 2, PM_PAGE_SIZE + 8, sizeof(source), PM_READ | PM_WRITE };
    EXPECT(pm_submit(m, &request, 1, &lease), PM_OK);
    memset(received, 0, sizeof(received));
    EXPECT(pm_device_read(m, lease, 0, received, sizeof(received)), PM_OK);
    CHECK(!memcmp(source, received, sizeof(source)));
    memset(source, 0x39, sizeof(source));
    EXPECT(pm_device_write(m, lease, 0, source, sizeof(source)), PM_OK);
    for (unsigned view = 0; view < PM_VIEWS; view++) {
        EXPECT(pm_guest_read(m, owner, view, PM_PAGE_SIZE + 8, received, sizeof(received)),
               PM_OK);
        CHECK(!memcmp(source, received, sizeof(source)));
    }

    EXPECT(pm_claim(m, PM_PAGE_SIZE, 20, &conflict), PM_CONFLICT);
    CHECK(conflict.authority == untouched.authority &&
          conflict.generation == untouched.generation);
    EXPECT(pm_guest_read(m, owner, 0, PM_PAGE_SIZE + 8, received, sizeof(received)), PM_OK);
    CHECK(!memcmp(source, received, sizeof(source)));
    request.owner = foreign;
    memset(&rejected, 0xa5, sizeof(rejected));
    memcpy(&rejected_before, &rejected, sizeof(rejected));
    EXPECT(pm_submit(m, &request, 1, &rejected), PM_STALE);
    CHECK(!memcmp(&rejected, &rejected_before, sizeof(rejected)));
    EXPECT(pm_device_read(other, lease, 0, received, 1), PM_STALE);

    EXPECT(pm_claim(m, 0, 8, &zero), PM_OK);
    EXPECT(pm_guest_write(m, zero, 0, 0, &value, 1), PM_OK);
    request = (PmRequest){ zero, 2, 0, 1, PM_READ };
    EXPECT(pm_submit(m, &request, 1, &page_zero), PM_OK);
    value = 0;
    EXPECT(pm_device_read(m, page_zero, 0, &value, 1), PM_OK);
    CHECK(value == 0x6a);
    EXPECT(pm_release(m, page_zero), PM_OK);
    EXPECT(pm_release(m, lease), PM_OK);
    finish(other);
    finish(m);
}

static void test_subpage_reuse(void)
{
    PmModel *m = mapped_model();
    PmOwner first, second, replacement, after_commit, rejected;
    PmLease leases[2], stale;
    PmRequest requests[2];
    uint8_t a[24], b[24], readback[24];

    memset(a, 0x31, sizeof(a));
    memset(b, 0x72, sizeof(b));
    EXPECT(pm_claim(m, 0, sizeof(a), &first), PM_OK);
    EXPECT(pm_claim(m, sizeof(a), sizeof(b), &second), PM_OK);
    EXPECT(pm_guest_write(m, first, 0, 0, a, sizeof(a)), PM_OK);
    EXPECT(pm_guest_write(m, second, 0, sizeof(a), b, sizeof(b)), PM_OK);
    requests[0] = (PmRequest){ first, 0, 8, 12, PM_READ | PM_WRITE };
    requests[1] = (PmRequest){ first, 1, 16, 8, PM_READ };
    EXPECT(pm_submit(m, requests, 2, leases), PM_OK);
    stale = leases[0];
    EXPECT(pm_retire(m, first), PM_OK);
    EXPECT(pm_retire(m, first), PM_STALE);
    EXPECT(pm_submit(m, requests, 1, &stale), PM_STALE);
    EXPECT(pm_claim(m, 0, sizeof(a), &rejected), PM_CONFLICT);
    EXPECT(pm_protect_page(m, 0, PM_READ), PM_BUSY);
    EXPECT(pm_decommit_page(m, 0), PM_BUSY);
    EXPECT(pm_map_page(m, 1, 0, PM_NO_PAGE), PM_BUSY);
    EXPECT(pm_map_page(m, 2, 0, 1), PM_BUSY);
    EXPECT(pm_device_read(m, leases[0], 0, readback, 12), PM_OK);
    CHECK(!memcmp(readback, a + 8, 12));
    memset(readback, 0x44, 12);
    EXPECT(pm_device_write(m, leases[0], 0, readback, 12), PM_OK);
    EXPECT(pm_guest_read(m, second, 2, sizeof(a), readback, sizeof(b)), PM_OK);
    CHECK(!memcmp(readback, b, sizeof(b)));
    EXPECT(pm_release(m, leases[0]), PM_OK);
    EXPECT(pm_claim(m, 0, sizeof(a), &rejected), PM_CONFLICT);
    EXPECT(pm_release(m, leases[1]), PM_OK);
    EXPECT(pm_claim(m, 0, sizeof(a), &replacement), PM_OK);
    CHECK(replacement.generation != first.generation);
    EXPECT(pm_guest_read(m, replacement, 1, 0, readback, sizeof(a)), PM_OK);
    for (unsigned i = 0; i < sizeof(a); i++) CHECK(readback[i] == 0);
    EXPECT(pm_guest_read(m, second, 0, sizeof(a), readback, sizeof(b)), PM_OK);
    CHECK(!memcmp(readback, b, sizeof(b)));
    EXPECT(pm_submit(m, requests, 1, &stale), PM_STALE);
    EXPECT(pm_guest_read(m, first, 0, 0, readback, 1), PM_STALE);
    EXPECT(pm_device_read(m, leases[0], 0, readback, 1), PM_STALE);
    EXPECT(pm_release(m, leases[0]), PM_STALE);
    EXPECT(pm_decommit_page(m, 0), PM_BUSY);
    EXPECT(pm_retire(m, replacement), PM_OK);
    EXPECT(pm_retire(m, second), PM_OK);
    EXPECT(pm_decommit_page(m, 0), PM_OK);
    EXPECT(pm_claim(m, 0, 1, &rejected), PM_UNMAPPED);
    EXPECT(pm_commit_page(m, 0), PM_OK);
    EXPECT(pm_claim(m, 0, 1, &after_commit), PM_OK);
    readback[0] = 0xff;
    EXPECT(pm_guest_read(m, after_commit, 2, 0, readback, 1), PM_OK);
    CHECK(readback[0] == 0);
    finish(m);
}

static void test_batch_and_spans(void)
{
    PmModel *m = mapped_model();
    PmOwner descriptor, payload, rejected;
    PmLease leases[2], unchanged[2];
    PmRequest requests[2];
    uint8_t left[2] = { 0x17, 0x28 }, right[2] = { 0x39, 0x4a }, bytes[2];

    EXPECT(pm_claim(m, PM_PAGE_SIZE, 3 * PM_PAGE_SIZE, &descriptor), PM_OK);
    EXPECT(pm_claim(m, 4 * PM_PAGE_SIZE, PM_PAGE_SIZE, &payload), PM_OK);
    requests[0] = (PmRequest){ descriptor, 0, PM_PAGE_SIZE, 3 * PM_PAGE_SIZE,
                               PM_READ | PM_WRITE };
    requests[1] = (PmRequest){ payload, 1, 4 * PM_PAGE_SIZE, 8, PM_WRITE };
    memset(leases, 0xa5, sizeof(leases));
    memcpy(unchanged, leases, sizeof(leases));
    EXPECT(pm_protect_page(m, 4, PM_READ), PM_OK);
    EXPECT(pm_submit(m, requests, 2, leases), PM_PROTECTION);
    CHECK(!memcmp(leases, unchanged, sizeof(leases)));
    EXPECT(pm_protect_page(m, 1, PM_READ), PM_OK);
    EXPECT(pm_protect_page(m, 1, PM_READ | PM_WRITE), PM_OK);
    EXPECT(pm_map_page(m, 0, 1, PM_NO_PAGE), PM_OK);
    EXPECT(pm_map_page(m, 0, 1, 1), PM_OK);
    EXPECT(pm_protect_page(m, 4, PM_READ | PM_WRITE), PM_OK);

    EXPECT(pm_map_page(m, 0, 2, PM_NO_PAGE), PM_OK);
    requests[0] = (PmRequest){ descriptor, 0, 2 * PM_PAGE_SIZE - 1, 2, PM_READ };
    EXPECT(pm_submit(m, requests, 1, leases), PM_UNMAPPED);
    CHECK(!memcmp(leases, unchanged, sizeof(leases)));
    EXPECT(pm_protect_page(m, 1, PM_READ), PM_OK);
    EXPECT(pm_protect_page(m, 1, PM_READ | PM_WRITE), PM_OK);
    EXPECT(pm_map_page(m, 0, 2, 2), PM_OK);
    requests[0] = (PmRequest){ descriptor, 0, 4 * PM_PAGE_SIZE - 1, 2, PM_READ };
    EXPECT(pm_submit(m, requests, 1, leases), PM_CONFLICT);
    CHECK(!memcmp(leases, unchanged, sizeof(leases)));
    EXPECT(pm_map_page(m, 2, 2, 3), PM_OK);
    requests[0] = (PmRequest){ descriptor, 2, 2 * PM_PAGE_SIZE - 2, 4, PM_WRITE };
    EXPECT(pm_submit(m, requests, 1, leases), PM_NONLINEAR);
    CHECK(!memcmp(leases, unchanged, sizeof(leases)));
    requests[0].length = 2;
    requests[1] = (PmRequest){ descriptor, 2, 2 * PM_PAGE_SIZE, 2, PM_WRITE };
    EXPECT(pm_submit(m, requests, 2, leases), PM_OK);
    EXPECT(pm_device_write(m, leases[0], 0, left, sizeof(left)), PM_OK);
    EXPECT(pm_device_write(m, leases[1], 0, right, sizeof(right)), PM_OK);
    EXPECT(pm_guest_read(m, descriptor, 0, 2 * PM_PAGE_SIZE - 2, bytes, 2), PM_OK);
    CHECK(!memcmp(bytes, left, 2));
    EXPECT(pm_guest_read(m, descriptor, 0, 3 * PM_PAGE_SIZE, bytes, 2), PM_OK);
    CHECK(!memcmp(bytes, right, 2));
    EXPECT(pm_map_page(m, 2, 2, 2), PM_BUSY);
    EXPECT(pm_release(m, leases[0]), PM_OK);
    EXPECT(pm_release(m, leases[1]), PM_OK);
    EXPECT(pm_map_page(m, 2, 2, 2), PM_OK);

    requests[0] = (PmRequest){ descriptor, 1, PM_PAGE_SIZE, 8, PM_READ };
    requests[1] = (PmRequest){ payload, 0, 4 * PM_PAGE_SIZE + 2, 12, PM_WRITE };
    EXPECT(pm_submit(m, requests, 2, leases), PM_OK);
    EXPECT(pm_device_write(m, leases[0], 0, left, 2), PM_PROTECTION);
    EXPECT(pm_device_read(m, leases[1], 0, bytes, 2), PM_PROTECTION);
    EXPECT(pm_device_write(m, leases[1], 11, left, 2), PM_INVALID);
    EXPECT(pm_retire(m, descriptor), PM_OK);
    EXPECT(pm_retire(m, payload), PM_OK);
    EXPECT(pm_claim(m, PM_PAGE_SIZE, 1, &rejected), PM_CONFLICT);
    EXPECT(pm_claim(m, 4 * PM_PAGE_SIZE, 1, &rejected), PM_CONFLICT);
    EXPECT(pm_release(m, leases[1]), PM_OK);
    EXPECT(pm_release(m, leases[0]), PM_OK);
    EXPECT(pm_claim(m, PM_PAGE_SIZE, 1, &rejected), PM_OK);
    finish(m);
}

static void test_limits_and_stale_handles(void)
{
    PmModel *m = mapped_model();
    PmOwner owner, rejected = { UINT64_MAX, UINT64_MAX }, owners[PM_MAX_OWNERS];
    PmLease leases[PM_MAX_LEASES], replacement, unchanged, old, pair[2], pair_before[2];
    PmRequest requests[PM_MAX_BATCH];
    uint8_t byte = 0;

    EXPECT(pm_claim(m, SIZE_MAX, 1, &rejected), PM_INVALID);
    EXPECT(pm_claim(m, 0, SIZE_MAX, &rejected), PM_INVALID);
    EXPECT(pm_claim(m, 0, 0, &rejected), PM_INVALID);
    CHECK(rejected.authority == UINT64_MAX && rejected.generation == UINT64_MAX);
    EXPECT(pm_claim(m, PM_PHYSICAL_PAGES * PM_PAGE_SIZE - 1, 1, &owner), PM_OK);
    requests[0] = (PmRequest){ owner, 0, PM_VIEW_PAGES * PM_PAGE_SIZE - 1, 1, PM_READ };
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_OK);
    EXPECT(pm_device_read(m, replacement, 0, &byte, 1), PM_OK);
    EXPECT(pm_device_read(m, replacement, 1, &byte, 1), PM_INVALID);
    EXPECT(pm_device_read(m, replacement, SIZE_MAX, &byte, 1), PM_INVALID);
    EXPECT(pm_device_read(m, replacement, 0, &byte, 0), PM_INVALID);
    EXPECT(pm_release(m, replacement), PM_OK);
    requests[0].offset = SIZE_MAX;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].offset = PM_VIEW_PAGES * PM_PAGE_SIZE - 1;
    requests[0].length = SIZE_MAX;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].length = 0;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].length = 1;
    requests[0].access = 0;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].access = 4;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].access = PM_READ;
    requests[0].view = PM_VIEWS;
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_INVALID);
    requests[0].view = 0;
    EXPECT(pm_submit(m, requests, 0, &replacement), PM_INVALID);
    EXPECT(pm_submit(m, requests, PM_MAX_BATCH + 1, &replacement), PM_INVALID);

    for (unsigned i = 1; i < PM_MAX_BATCH; i++) requests[i] = requests[0];
    for (unsigned start = 0; start < PM_MAX_LEASES; start += PM_MAX_BATCH)
        EXPECT(pm_submit(m, requests, PM_MAX_BATCH, &leases[start]), PM_OK);
    memset(&replacement, 0x5a, sizeof(replacement));
    memcpy(&unchanged, &replacement, sizeof(replacement));
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_CAPACITY);
    CHECK(!memcmp(&replacement, &unchanged, sizeof(replacement)));
    old = leases[7];
    EXPECT(pm_release(m, old), PM_OK);
    memset(pair, 0x6b, sizeof(pair));
    memcpy(pair_before, pair, sizeof(pair));
    EXPECT(pm_submit(m, requests, 2, pair), PM_CAPACITY);
    CHECK(!memcmp(pair, pair_before, sizeof(pair)));
    EXPECT(pm_submit(m, requests, 1, &replacement), PM_OK);
    CHECK(old.slot == replacement.slot && old.serial != replacement.serial);
    EXPECT(pm_release(m, old), PM_STALE);
    EXPECT(pm_device_read(m, replacement, 0, &byte, 1), PM_OK);
    for (unsigned i = 0; i < PM_MAX_LEASES; i++)
        if (i != 7) EXPECT(pm_release(m, leases[i]), PM_OK);
    EXPECT(pm_release(m, replacement), PM_OK);
    finish(m);

    m = mapped_model();
    for (unsigned i = 0; i < PM_MAX_OWNERS; i++)
        EXPECT(pm_claim(m, i, 1, &owners[i]), PM_OK);
    EXPECT(pm_claim(m, PM_MAX_OWNERS, 1, &rejected), PM_CAPACITY);
    EXPECT(pm_retire(m, owners[0]), PM_OK);
    EXPECT(pm_claim(m, PM_MAX_OWNERS, 1, &owner), PM_OK);
    CHECK(owner.generation != owners[0].generation);
    finish(m);
}

typedef struct {
    PmModel *model;
    PmLease lease;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int queued, go, completed, owns_lease;
    PmResult write_result, release_result;
} Deferred;

static void *deferred_worker(void *argument)
{
    Deferred *work = argument;
    uint8_t value = 0x71;
    mutex_lock(&work->lock);
    work->queued = 1;
    CHECK(!pthread_cond_broadcast(&work->cond));
    while (!work->go) CHECK(!pthread_cond_wait(&work->cond, &work->lock));
    mutex_unlock(&work->lock);
    if (work->owns_lease) {
        work->write_result = pm_device_write(work->model, work->lease, 0, &value, 1);
        work->release_result = pm_release(work->model, work->lease);
    }
    mutex_lock(&work->lock);
    work->completed = 1;
    CHECK(!pthread_cond_broadcast(&work->cond));
    mutex_unlock(&work->lock);
    return NULL;
}

static void wait_queued(Deferred *work)
{
    mutex_lock(&work->lock);
    while (!work->queued) CHECK(!pthread_cond_wait(&work->cond, &work->lock));
    CHECK(!work->completed);
    mutex_unlock(&work->lock);
}

static void start_deferred(Deferred *work)
{
    mutex_lock(&work->lock);
    work->go = 1;
    CHECK(!pthread_cond_broadcast(&work->cond));
    mutex_unlock(&work->lock);
}

static void test_shutdown(void)
{
    PmModel *m = mapped_model();
    PmOwner owner, rejected;
    PmLease lease, rejected_lease;
    PmRequest request;
    pthread_t thread;
    uint8_t byte = 0;
    Deferred work = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER,
                      .cond = PTHREAD_COND_INITIALIZER, .owns_lease = 1 };

    EXPECT(pm_destroy(m), PM_BUSY);
    EXPECT(pm_claim(m, 0, 16, &owner), PM_OK);
    request = (PmRequest){ owner, 0, 0, 16, PM_READ | PM_WRITE };
    EXPECT(pm_submit(m, &request, 1, &lease), PM_OK);
    work.lease = lease;
    CHECK(!pthread_create(&thread, NULL, deferred_worker, &work));
    wait_queued(&work);
    EXPECT(pm_begin_shutdown(m), PM_OK);
    EXPECT(pm_begin_shutdown(m), PM_OK);
    EXPECT(pm_shutdown_ready(m), PM_BUSY);
    EXPECT(pm_submit(m, &request, 1, &rejected_lease), PM_CLOSED);
    EXPECT(pm_claim(m, PM_PAGE_SIZE, 1, &rejected), PM_CLOSED);
    EXPECT(pm_guest_read(m, owner, 0, 0, &byte, 1), PM_CLOSED);
    EXPECT(pm_protect_page(m, 0, PM_READ), PM_CLOSED);
    EXPECT(pm_map_page(m, 0, 0, PM_NO_PAGE), PM_CLOSED);
    start_deferred(&work);
    CHECK(!pthread_join(thread, NULL));
    CHECK(work.completed && work.write_result == PM_OK && work.release_result == PM_OK);
    EXPECT(pm_shutdown_ready(m), PM_OK);
    EXPECT(pm_device_read(m, lease, 0, &byte, 1), PM_STALE);
    EXPECT(pm_destroy(m), PM_OK);
    CHECK(!pthread_cond_destroy(&work.cond));
    CHECK(!pthread_mutex_destroy(&work.lock));

    m = mapped_model();
    Deferred untracked = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER,
                           .cond = PTHREAD_COND_INITIALIZER };
    CHECK(!pthread_create(&thread, NULL, deferred_worker, &untracked));
    wait_queued(&untracked);
    EXPECT(pm_begin_shutdown(m), PM_OK);
    EXPECT(pm_shutdown_ready(m), PM_OK);
    mutex_lock(&untracked.lock);
    CHECK(!untracked.completed);
    mutex_unlock(&untracked.lock);
    start_deferred(&untracked);
    CHECK(!pthread_join(thread, NULL));
    CHECK(untracked.completed);
    EXPECT(pm_destroy(m), PM_OK);
    CHECK(!pthread_cond_destroy(&untracked.cond));
    CHECK(!pthread_mutex_destroy(&untracked.lock));
}

enum { HANDOFF_BYTES = 96, HANDOFF_THREADS = 4, HANDOFF_ROUNDS = 3000 };

typedef struct {
    PmModel *model;
    PmOwner owner;
    PmLease lease;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    int go;
    atomic_uint failures;
} Handoff;

typedef struct {
    Handoff *work;
    unsigned role;
} HandoffArg;

static void *handoff_worker(void *argument)
{
    HandoffArg *arg = argument;
    Handoff *work = arg->work;
    uint8_t bytes[HANDOFF_BYTES];
    mutex_lock(&work->lock);
    work->ready++;
    CHECK(!pthread_cond_broadcast(&work->cond));
    while (!work->go) CHECK(!pthread_cond_wait(&work->cond, &work->lock));
    mutex_unlock(&work->lock);
    for (unsigned round = 0; round < HANDOFF_ROUNDS; round++) {
        PmResult result;
        if (arg->role < 2) {
            memset(bytes, (uint8_t)((round + 43 * arg->role) % 251 + 1), sizeof(bytes));
            result = arg->role == 0
                   ? pm_guest_write(work->model, work->owner, 0, PM_PAGE_SIZE + 3,
                                    bytes, sizeof(bytes))
                   : pm_device_write(work->model, work->lease, 0, bytes, sizeof(bytes));
        } else {
            result = arg->role == 2
                   ? pm_guest_read(work->model, work->owner, 1, PM_PAGE_SIZE + 3,
                                   bytes, sizeof(bytes))
                   : pm_device_read(work->model, work->lease, 0, bytes, sizeof(bytes));
            if (result == PM_OK)
                for (unsigned i = 1; i < sizeof(bytes); i++)
                    if (bytes[i] != bytes[0]) {
                        atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
                        break;
                    }
        }
        if (result != PM_OK) {
            atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
            break;
        }
        if (!(round % 32)) sched_yield();
    }
    return NULL;
}

static void test_threaded_handoff(void)
{
    PmModel *m = mapped_model();
    PmOwner owner;
    PmLease lease;
    PmRequest request;
    pthread_t threads[HANDOFF_THREADS];
    HandoffArg args[HANDOFF_THREADS];
    Handoff work = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER,
                     .cond = PTHREAD_COND_INITIALIZER };
    uint8_t bytes[HANDOFF_BYTES];
    atomic_init(&work.failures, 0);
    EXPECT(pm_claim(m, PM_PAGE_SIZE + 3, HANDOFF_BYTES, &owner), PM_OK);
    request = (PmRequest){ owner, 2, PM_PAGE_SIZE + 3, HANDOFF_BYTES, PM_READ | PM_WRITE };
    EXPECT(pm_submit(m, &request, 1, &lease), PM_OK);
    work.owner = owner;
    work.lease = lease;
    memset(bytes, 0x31, sizeof(bytes));
    EXPECT(pm_guest_write(m, owner, 0, PM_PAGE_SIZE + 3, bytes, sizeof(bytes)), PM_OK);
    EXPECT(pm_device_read(m, lease, 0, bytes, sizeof(bytes)), PM_OK);
    CHECK(bytes[0] == 0x31);
    memset(bytes, 0x72, sizeof(bytes));
    EXPECT(pm_guest_write(m, owner, 1, PM_PAGE_SIZE + 3, bytes, sizeof(bytes)), PM_OK);
    memset(bytes, 0, sizeof(bytes));
    EXPECT(pm_device_read(m, lease, 0, bytes, sizeof(bytes)), PM_OK);
    CHECK(bytes[0] == 0x72);
    for (unsigned i = 0; i < HANDOFF_THREADS; i++) {
        args[i] = (HandoffArg){ &work, i };
        CHECK(!pthread_create(&threads[i], NULL, handoff_worker, &args[i]));
    }
    mutex_lock(&work.lock);
    while (work.ready != HANDOFF_THREADS) CHECK(!pthread_cond_wait(&work.cond, &work.lock));
    work.go = 1;
    CHECK(!pthread_cond_broadcast(&work.cond));
    mutex_unlock(&work.lock);
    for (unsigned i = 0; i < HANDOFF_THREADS; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!atomic_load_explicit(&work.failures, memory_order_relaxed));
    EXPECT(pm_release(m, lease), PM_OK);
    finish(m);
    CHECK(!pthread_cond_destroy(&work.cond));
    CHECK(!pthread_mutex_destroy(&work.lock));
}

enum { CHURN_READERS = 3, CHURN_ROUNDS = 1200, CHURN_BYTES = PM_PAGE_SIZE + 5 };

typedef struct {
    PmModel *model;
    pthread_mutex_t lock;
    PmOwner current;
    int stop;
    atomic_uint started, successes, failures;
} Churn;

static void *churn_reader(void *argument)
{
    Churn *work = argument;
    int announced = 0;
    for (;;) {
        PmOwner owner;
        PmLease lease;
        PmResult result;
        uint8_t bytes[CHURN_BYTES];
        mutex_lock(&work->lock);
        if (work->stop) {
            mutex_unlock(&work->lock);
            break;
        }
        owner = work->current;
        mutex_unlock(&work->lock);
        PmRequest request = { owner, 1, PM_PAGE_SIZE - 3, CHURN_BYTES, PM_READ };
        result = pm_submit(work->model, &request, 1, &lease);
        if (result == PM_STALE) continue;
        if (result != PM_OK) {
            atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
            break;
        }
        if (!announced) {
            announced = 1;
            atomic_fetch_add_explicit(&work->started, 1, memory_order_release);
        }
        result = pm_device_read(work->model, lease, 0, bytes, sizeof(bytes));
        if (result != PM_OK) {
            atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
        } else {
            uint8_t expected = (uint8_t)(owner.generation % 251 + 1);
            for (unsigned i = 0; i < sizeof(bytes); i++)
                if (bytes[i] != expected) {
                    atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
                    break;
                }
        }
        if (pm_release(work->model, lease) != PM_OK)
            atomic_fetch_add_explicit(&work->failures, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&work->successes, 1, memory_order_relaxed);
    }
    return NULL;
}

static void initialize_churn_owner(PmModel *m, PmOwner owner)
{
    uint8_t bytes[CHURN_BYTES];
    memset(bytes, (uint8_t)(owner.generation % 251 + 1), sizeof(bytes));
    EXPECT(pm_guest_write(m, owner, 0, PM_PAGE_SIZE - 3, bytes, sizeof(bytes)), PM_OK);
}

static void test_threaded_churn(void)
{
    PmModel *m = mapped_model();
    PmOwner owner;
    pthread_t threads[CHURN_READERS];
    Churn work = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER };
    atomic_init(&work.started, 0);
    atomic_init(&work.successes, 0);
    atomic_init(&work.failures, 0);
    EXPECT(pm_claim(m, PM_PAGE_SIZE - 3, CHURN_BYTES, &owner), PM_OK);
    initialize_churn_owner(m, owner);
    work.current = owner;
    for (unsigned i = 0; i < CHURN_READERS; i++)
        CHECK(!pthread_create(&threads[i], NULL, churn_reader, &work));
    while (atomic_load_explicit(&work.started, memory_order_acquire) != CHURN_READERS &&
           !atomic_load_explicit(&work.failures, memory_order_relaxed))
        sched_yield();
    CHECK(atomic_load_explicit(&work.started, memory_order_acquire) == CHURN_READERS);
    for (unsigned round = 0; round < CHURN_ROUNDS; round++) {
        PmResult result;
        EXPECT(pm_retire(m, owner), PM_OK);
        do {
            result = pm_claim(m, PM_PAGE_SIZE - 3, CHURN_BYTES, &owner);
            if (result == PM_CONFLICT) sched_yield();
        } while (result == PM_CONFLICT);
        CHECK(result == PM_OK);
        initialize_churn_owner(m, owner);
        mutex_lock(&work.lock);
        work.current = owner;
        mutex_unlock(&work.lock);
    }
    mutex_lock(&work.lock);
    work.stop = 1;
    mutex_unlock(&work.lock);
    for (unsigned i = 0; i < CHURN_READERS; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!atomic_load_explicit(&work.failures, memory_order_relaxed));
    CHECK(atomic_load_explicit(&work.successes, memory_order_relaxed) >= CHURN_READERS);
    EXPECT(pm_retire(m, owner), PM_OK);
    finish(m);
    CHECK(!pthread_mutex_destroy(&work.lock));
}

typedef struct {
    PmModel *model;
    PmLease lease;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    int go;
    PmResult results[2];
} ReleaseRace;

typedef struct {
    ReleaseRace *race;
    unsigned index;
} ReleaseArg;

static void *release_worker(void *argument)
{
    ReleaseArg *arg = argument;
    ReleaseRace *race = arg->race;
    mutex_lock(&race->lock);
    race->ready++;
    CHECK(!pthread_cond_broadcast(&race->cond));
    while (!race->go) CHECK(!pthread_cond_wait(&race->cond, &race->lock));
    mutex_unlock(&race->lock);
    race->results[arg->index] = pm_release(race->model, race->lease);
    return NULL;
}

static void test_concurrent_release(void)
{
    PmModel *m = mapped_model();
    PmOwner owner;
    PmLease lease;
    PmRequest request;
    pthread_t threads[2];
    ReleaseArg args[2];
    ReleaseRace race = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER,
                         .cond = PTHREAD_COND_INITIALIZER };
    EXPECT(pm_claim(m, 0, 8, &owner), PM_OK);
    request = (PmRequest){ owner, 0, 0, 8, PM_READ };
    EXPECT(pm_submit(m, &request, 1, &lease), PM_OK);
    race.lease = lease;
    for (unsigned i = 0; i < 2; i++) {
        args[i] = (ReleaseArg){ &race, i };
        CHECK(!pthread_create(&threads[i], NULL, release_worker, &args[i]));
    }
    mutex_lock(&race.lock);
    while (race.ready != 2) CHECK(!pthread_cond_wait(&race.cond, &race.lock));
    race.go = 1;
    CHECK(!pthread_cond_broadcast(&race.cond));
    mutex_unlock(&race.lock);
    for (unsigned i = 0; i < 2; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK((race.results[0] == PM_OK && race.results[1] == PM_STALE) ||
          (race.results[1] == PM_OK && race.results[0] == PM_STALE));
    EXPECT(pm_retire(m, owner), PM_OK);
    finish(m);
    CHECK(!pthread_cond_destroy(&race.cond));
    CHECK(!pthread_mutex_destroy(&race.lock));
}

typedef struct {
    PmModel *model;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    int go;
    PmOwner owners[2];
    PmResult results[2], writes[2];
} ClaimRace;

typedef struct {
    ClaimRace *race;
    unsigned index;
} ClaimArg;

static void *claim_worker(void *argument)
{
    ClaimArg *arg = argument;
    ClaimRace *race = arg->race;
    uint8_t bytes[32];
    mutex_lock(&race->lock);
    race->ready++;
    CHECK(!pthread_cond_broadcast(&race->cond));
    while (!race->go) CHECK(!pthread_cond_wait(&race->cond, &race->lock));
    mutex_unlock(&race->lock);
    race->results[arg->index] = pm_claim(race->model, arg->index * 16, 32,
                                         &race->owners[arg->index]);
    if (race->results[arg->index] == PM_OK) {
        memset(bytes, (int)arg->index + 1, sizeof(bytes));
        race->writes[arg->index] = pm_guest_write(race->model, race->owners[arg->index],
                                                  0, arg->index * 16,
                                                  bytes, sizeof(bytes));
    }
    return NULL;
}

static void test_concurrent_claim(void)
{
    PmModel *m = mapped_model();
    PmOwner remainder;
    pthread_t threads[2];
    ClaimArg args[2];
    uint8_t bytes[32];
    unsigned winner;
    ClaimRace race = { .model = m, .lock = PTHREAD_MUTEX_INITIALIZER,
                       .cond = PTHREAD_COND_INITIALIZER };
    for (unsigned i = 0; i < 2; i++) {
        args[i] = (ClaimArg){ &race, i };
        CHECK(!pthread_create(&threads[i], NULL, claim_worker, &args[i]));
    }
    mutex_lock(&race.lock);
    while (race.ready != 2) CHECK(!pthread_cond_wait(&race.cond, &race.lock));
    race.go = 1;
    CHECK(!pthread_cond_broadcast(&race.cond));
    mutex_unlock(&race.lock);
    for (unsigned i = 0; i < 2; i++) CHECK(!pthread_join(threads[i], NULL));
    CHECK((race.results[0] == PM_OK && race.results[1] == PM_CONFLICT) ||
          (race.results[1] == PM_OK && race.results[0] == PM_CONFLICT));
    winner = race.results[0] == PM_OK ? 0 : 1;
    CHECK(race.writes[winner] == PM_OK);
    EXPECT(pm_claim(m, winner == 0 ? 32 : 0, 16, &remainder), PM_OK);
    EXPECT(pm_guest_read(m, race.owners[winner], 1, winner * 16, bytes, sizeof(bytes)),
           PM_OK);
    for (unsigned i = 0; i < sizeof(bytes); i++) CHECK(bytes[i] == winner + 1);
    finish(m);
    CHECK(!pthread_cond_destroy(&race.cond));
    CHECK(!pthread_mutex_destroy(&race.lock));
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "aliases")) test_shared_aliases();
    else if (!strcmp(argv[1], "subpage")) test_subpage_reuse();
    else if (!strcmp(argv[1], "batch")) test_batch_and_spans();
    else if (!strcmp(argv[1], "limits")) test_limits_and_stale_handles();
    else if (!strcmp(argv[1], "shutdown")) test_shutdown();
    else if (!strcmp(argv[1], "handoff")) test_threaded_handoff();
    else if (!strcmp(argv[1], "churn")) test_threaded_churn();
    else if (!strcmp(argv[1], "release")) test_concurrent_release();
    else if (!strcmp(argv[1], "claim")) test_concurrent_claim();
    else return 2;
    return 0;
}
