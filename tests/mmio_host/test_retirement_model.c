#include "retirement_model.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static int transitions(void)
{
    RetirementModel m, other;
    RmToken irq, another, dpc, requeued, unused, foreign;

    CHECK(rm_init(&m) && rm_init(&other));
    CHECK(rm_admit_access(&other, &foreign) == RM_OK);
    CHECK(rm_producers_joined(&m) == RM_INVALID);
    CHECK(rm_admit_irq(&m, &irq) == RM_OK);
    CHECK(rm_finish(&m, foreign) == RM_INVALID);
    CHECK(rm_admit_irq(&m, &unused) == RM_DUPLICATE);
    CHECK(rm_claim_irq(&m, irq) == RM_OK);
    CHECK(rm_cancel_queued(&m, irq) == RM_INVALID);
    CHECK(rm_spawn_dpc(&m, irq, 1, &unused) == RM_INVALID);
    /* A line may be posted again after its previous bit was taken locally. */
    CHECK(rm_admit_irq(&m, &another) == RM_OK);
    CHECK(rm_admit_irq(&m, &unused) == RM_DUPLICATE);
    rm_close(&m);
    CHECK(rm_admit_irq(&m, &unused) == RM_CLOSED);
    CHECK(rm_admit_dpc(&m, 1, &unused) == RM_CLOSED);
    CHECK(rm_admit_access(&m, &unused) == RM_CLOSED);
    CHECK(rm_producers_joined(&m) == RM_OK);
    CHECK(!rm_try_retire(&m));
    CHECK(rm_cancel_queued(&m, another) == RM_OK);
    CHECK(rm_cancel_queued(&m, another) == RM_INVALID);
    CHECK(rm_count(&m, RM_IRQ_PENDING) == 0);
    CHECK(!rm_try_retire(&m)); /* claimed but not yet entered */
    CHECK(rm_start_irq(&m, irq) == RM_OK);
    CHECK(rm_spawn_dpc(&m, irq, 1, &dpc) == RM_OK);
    CHECK(rm_spawn_dpc(&m, irq, 1, &unused) == RM_DUPLICATE);
    CHECK(rm_finish(&m, irq) == RM_OK);
    CHECK(rm_spawn_dpc(&m, irq, 2, &unused) == RM_INVALID);
    CHECK(!rm_try_retire(&m));
    CHECK(rm_start_dpc(&m, dpc) == RM_OK);
    CHECK(rm_count(&m, RM_DPC_QUEUED) == 0);
    CHECK(rm_spawn_dpc(&m, dpc, 1, &requeued) == RM_OK);
    CHECK(rm_spawn_dpc(&m, dpc, 1, &unused) == RM_DUPLICATE);
    CHECK(rm_cancel_queued(&m, dpc) == RM_INVALID);
    CHECK(rm_cancel_queued(&m, requeued) == RM_OK);
    CHECK(rm_count(&m, RM_DPC_QUEUED) == 0);
    CHECK(!rm_try_retire(&m)); /* cancellation did not finish the running DPC */
    CHECK(rm_finish(&m, dpc) == RM_OK);
    CHECK(rm_finish(&m, dpc) == RM_INVALID);
    CHECK(rm_try_retire(&m));
    rm_close(&m);
    CHECK(rm_try_retire(&m));
    CHECK(rm_admit_access(&m, &unused) == RM_CLOSED);
    CHECK(rm_finish(&other, foreign) == RM_OK);
    rm_close(&other);
    CHECK(!rm_try_retire(&other));
    CHECK(rm_producers_joined(&other) == RM_OK);
    CHECK(rm_try_retire(&other));
    rm_destroy(&other);
    rm_destroy(&m);
    return 1;
}

static int capacity_and_stale_tokens(void)
{
    RetirementModel m;
    RmToken access[RM_CAPACITY], queued, replacement, unused;

    CHECK(rm_init(&m));
    for (unsigned i = 0; i < RM_CAPACITY; ++i)
        CHECK(rm_admit_access(&m, &access[i]) == RM_OK);
    CHECK(rm_admit_irq(&m, &unused) == RM_FULL);
    CHECK(rm_spawn_dpc(&m, access[1], 3, &unused) == RM_FULL);
    CHECK(rm_count(&m, RM_ACCESS) == RM_CAPACITY);
    CHECK(rm_finish(&m, access[0]) == RM_OK);
    CHECK(rm_spawn_dpc(&m, access[1], 3, &queued) == RM_OK);
    CHECK(rm_admit_dpc(&m, 3, &unused) == RM_DUPLICATE);
    CHECK(rm_cancel_queued(&m, queued) == RM_OK);
    CHECK(rm_admit_access(&m, &replacement) == RM_OK);
    CHECK(replacement.slot == access[0].slot);
    CHECK(rm_finish(&m, access[0]) == RM_INVALID);
    CHECK(rm_cancel_queued(&m, queued) == RM_INVALID);
    rm_close(&m);
    CHECK(!rm_try_retire(&m));
    CHECK(rm_producers_joined(&m) == RM_OK);
    CHECK(!rm_try_retire(&m));
    CHECK(rm_spawn_dpc(&m, access[1], 4, &unused) == RM_FULL);
    CHECK(rm_finish(&m, replacement) == RM_OK);
    CHECK(rm_spawn_dpc(&m, access[1], 4, &queued) == RM_OK);
    CHECK(rm_cancel_queued(&m, queued) == RM_OK);
    for (unsigned i = 1; i < RM_CAPACITY; ++i)
        CHECK(rm_finish(&m, access[i]) == RM_OK);
    CHECK(rm_try_retire(&m));
    rm_destroy(&m);
    CHECK(rm_init(&m)); /* A new domain can reuse the same storage and slot. */
    CHECK(rm_admit_access(&m, &unused) == RM_OK);
    CHECK(rm_finish(&m, replacement) == RM_INVALID);
    CHECK(rm_finish(&m, unused) == RM_OK);
    rm_close(&m);
    CHECK(rm_producers_joined(&m) == RM_OK);
    CHECK(rm_try_retire(&m));
    rm_destroy(&m);
    return 1;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int ready, go;
    RetirementModel *model;
    RmToken token, child;
    RmResult first, second;
    int action;
} Race;

static void race_wait(Race *r)
{
    pthread_mutex_lock(&r->lock);
    ++r->ready;
    pthread_cond_broadcast(&r->cv);
    while (!r->go)
        pthread_cond_wait(&r->cv, &r->lock);
    pthread_mutex_unlock(&r->lock);
}

static void *race_first(void *arg)
{
    Race *r = arg;
    race_wait(r);
    if (r->action == 0)
        r->first = rm_admit_access(r->model, &r->token);
    else if (r->action == 1)
        r->first = rm_spawn_dpc(r->model, r->token, 7, &r->child);
    else
        r->first = rm_start_dpc(r->model, r->token);
    return NULL;
}

static void *race_second(void *arg)
{
    Race *r = arg;
    race_wait(r);
    if (r->action != 2)
        rm_close(r->model);
    else
        r->second = rm_cancel_queued(r->model, r->token);
    return NULL;
}

static int races(void)
{
    for (int action = 0; action < 3; ++action) {
        for (int n = 0; n < 200; ++n) {
            RetirementModel m;
            Race r = {.lock = PTHREAD_MUTEX_INITIALIZER,
                      .cv = PTHREAD_COND_INITIALIZER, .model = &m, .action = action};
            pthread_t a, b;

            CHECK(rm_init(&m));
            if (action == 1)
                CHECK(rm_admit_access(&m, &r.token) == RM_OK);
            if (action == 2)
                CHECK(rm_admit_dpc(&m, 7, &r.token) == RM_OK);
            CHECK(pthread_create(&a, NULL, race_first, &r) == 0);
            CHECK(pthread_create(&b, NULL, race_second, &r) == 0);
            pthread_mutex_lock(&r.lock);
            while (r.ready != 2)
                pthread_cond_wait(&r.cv, &r.lock);
            r.go = 1;
            pthread_cond_broadcast(&r.cv);
            pthread_mutex_unlock(&r.lock);
            CHECK(pthread_join(a, NULL) == 0);
            CHECK(pthread_join(b, NULL) == 0);
            rm_close(&m);
            CHECK(rm_producers_joined(&m) == RM_OK);
            if (action == 0) {
                CHECK(r.first == RM_OK || r.first == RM_CLOSED);
                if (r.first == RM_OK) {
                    CHECK(!rm_try_retire(&m));
                    CHECK(rm_finish(&m, r.token) == RM_OK);
                }
            } else if (action == 1) {
                CHECK(r.first == RM_OK);
                CHECK(!rm_try_retire(&m));
                CHECK(rm_finish(&m, r.token) == RM_OK);
                CHECK(!rm_try_retire(&m));
                CHECK(rm_cancel_queued(&m, r.child) == RM_OK);
            } else {
                CHECK((r.first == RM_OK && r.second == RM_INVALID) ||
                      (r.first == RM_INVALID && r.second == RM_OK));
                if (r.first == RM_OK) {
                    CHECK(!rm_try_retire(&m));
                    CHECK(rm_finish(&m, r.token) == RM_OK);
                }
            }
            CHECK(rm_try_retire(&m));
            rm_destroy(&m);
            CHECK(pthread_cond_destroy(&r.cv) == 0);
            CHECK(pthread_mutex_destroy(&r.lock) == 0);
        }
    }
    return 1;
}

typedef struct {
    RetirementModel *model;
    RmToken dpc;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int entered, service;
    int payload;
} WorkerTest;

static void *blocked_dpc(void *arg)
{
    WorkerTest *w = arg;
    if (rm_start_dpc(w->model, w->dpc) != RM_OK)
        return NULL;
    pthread_mutex_lock(&w->lock);
    w->entered = 1;
    pthread_cond_broadcast(&w->cv);
    while (!w->service)
        pthread_cond_wait(&w->cv, &w->lock);
    pthread_mutex_unlock(&w->lock);
    w->payload = 42;
    (void)rm_finish(w->model, w->dpc);
    return NULL;
}

static void *service_worker(void *arg)
{
    WorkerTest *w = arg;
    pthread_mutex_lock(&w->lock);
    w->service = 1;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

typedef struct { RetirementModel *model; int ready; } Closer;
static void *close_and_try(void *arg)
{
    Closer *c = arg;
    rm_close(c->model);
    c->ready = rm_try_retire(c->model);
    return NULL;
}

static int retire_with_deadline(RetirementModel *m)
{
    struct timespec deadline, now, pause = {0, 1000000};
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return 0;
    deadline.tv_sec += 5;
    for (;;) {
        if (rm_try_retire(m))
            return 1;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            return 0;
        nanosleep(&pause, NULL);
    }
}

static int worker_and_concurrent_shutdown(void)
{
    RetirementModel m;
    WorkerTest w = {.model = &m, .lock = PTHREAD_MUTEX_INITIALIZER,
                    .cv = PTHREAD_COND_INITIALIZER};
    Closer a = {.model = &m}, b = {.model = &m};
    pthread_t callback, closer1, closer2, worker;
    RmToken unused;

    CHECK(rm_init(&m));
    CHECK(rm_admit_dpc(&m, 8, &w.dpc) == RM_OK);
    CHECK(pthread_create(&callback, NULL, blocked_dpc, &w) == 0);
    pthread_mutex_lock(&w.lock);
    while (!w.entered)
        pthread_cond_wait(&w.cv, &w.lock);
    pthread_mutex_unlock(&w.lock);
    rm_close(&m);
    CHECK(rm_producers_joined(&m) == RM_OK);
    CHECK(pthread_create(&closer1, NULL, close_and_try, &a) == 0);
    CHECK(pthread_create(&closer2, NULL, close_and_try, &b) == 0);
    CHECK(pthread_join(closer1, NULL) == 0);
    CHECK(pthread_join(closer2, NULL) == 0);
    CHECK(!a.ready && !b.ready);
    CHECK(rm_admit_access(&m, &unused) == RM_CLOSED);
    CHECK(!rm_try_retire(&m));
    /* A required service worker remains available during the drain. */
    CHECK(pthread_create(&worker, NULL, service_worker, &w) == 0);
    CHECK(retire_with_deadline(&m));
    /* No join or callback-completion condition variable publishes this. */
    CHECK(w.payload == 42);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(pthread_join(callback, NULL) == 0);
    rm_destroy(&m);
    CHECK(pthread_cond_destroy(&w.cv) == 0);
    CHECK(pthread_mutex_destroy(&w.lock) == 0);
    return 1;
}

int main(void)
{
    if (!transitions() || !capacity_and_stale_tokens() ||
        !races() || !worker_and_concurrent_shutdown())
        return 1;
    puts("Synthetic retirement model: all checks passed (not runtime safety)");
    return 0;
}
