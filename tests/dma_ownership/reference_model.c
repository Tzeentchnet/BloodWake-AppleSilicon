/* Test-only page-granular ownership model, not a production DMA mapper. */
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 64, PAGES = 8, BACKINGS = 2, READERS = 4, ROUNDS = 4000 };
enum { FREE, LIVE, RETIRED };
typedef struct {
    uint64_t owner;
    unsigned state, pins;
} Page;
typedef struct {
    pthread_mutex_t lock;
    Page pages[BACKINGS][PAGES];
    uint8_t bytes[BACKINGS][PAGES * PAGE];
    uint64_t next_owner;
} Model;
typedef struct {
    Model *model;
    unsigned backing, first, last;
    uint8_t *ptr;
    int active;
} Lease;

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "reference contract check failed at line %d\n", __LINE__); \
    abort(); \
} } while (0)

static void lock(Model *m) { CHECK(!pthread_mutex_lock(&m->lock)); }
static void unlock(Model *m) { CHECK(!pthread_mutex_unlock(&m->lock)); }

static void collect(Model *m, unsigned backing)
{
    for (unsigned i = 0; i < PAGES; i++) {
        uint64_t owner = m->pages[backing][i].owner;
        int pinned = 0;
        if (m->pages[backing][i].state != RETIRED) continue;
        for (unsigned j = 0; j < PAGES; j++)
            if (m->pages[backing][j].owner == owner &&
                m->pages[backing][j].pins) pinned = 1;
        if (pinned) continue;
        for (unsigned j = 0; j < PAGES; j++)
            if (m->pages[backing][j].owner == owner)
                memset(&m->pages[backing][j], 0, sizeof(Page));
    }
}

static uint64_t allocate(Model *m, unsigned backing, unsigned first, unsigned count)
{
    uint64_t owner = 0;
    if (backing >= BACKINGS || first >= PAGES || !count || count > PAGES - first)
        return 0;
    lock(m);
    for (unsigned i = first; i < first + count; i++)
        if (m->pages[backing][i].state != FREE) goto out;
    CHECK(m->next_owner != UINT64_MAX);
    owner = ++m->next_owner;
    for (unsigned i = first; i < first + count; i++) {
        m->pages[backing][i] = (Page){ owner, LIVE, 0 };
        memset(&m->bytes[backing][i * PAGE], (uint8_t)owner, PAGE);
    }
out:
    unlock(m);
    return owner;
}

static int retire(Model *m, unsigned backing, uint64_t owner)
{
    int found = 0;
    if (backing >= BACKINGS || !owner) return 0;
    lock(m);
    for (unsigned i = 0; i < PAGES; i++)
        if (m->pages[backing][i].owner == owner &&
            m->pages[backing][i].state == LIVE) {
            m->pages[backing][i].state = RETIRED;
            found = 1;
        }
    collect(m, backing);
    unlock(m);
    return found;
}

/* A token identifies both a backing and a particular allocation generation.
 * The complete byte span is checked and pinned as one transaction. */
static int pin(Model *m, unsigned backing, uint64_t owner,
               size_t offset, size_t length, Lease *lease)
{
    unsigned first, last;
    int ok = 0;
    if (!lease || lease->active || backing >= BACKINGS || !owner ||
        !length || offset >= PAGES * PAGE || length > PAGES * PAGE - offset)
        return 0;
    first = (unsigned)(offset / PAGE);
    last = (unsigned)((offset + length - 1) / PAGE);
    lock(m);
    for (unsigned i = first; i <= last; i++) {
        Page *p = &m->pages[backing][i];
        if (p->state != LIVE || p->owner != owner || p->pins == UINT32_MAX)
            goto out;
    }
    for (unsigned i = first; i <= last; i++) m->pages[backing][i].pins++;
    *lease = (Lease){ m, backing, first, last, &m->bytes[backing][offset], 1 };
    ok = 1;
out:
    unlock(m);
    return ok;
}

static void release(Lease *lease)
{
    Model *m = lease->model;
    CHECK(lease->active);
    lock(m);
    for (unsigned i = lease->first; i <= lease->last; i++) {
        CHECK(m->pages[lease->backing][i].pins);
        m->pages[lease->backing][i].pins--;
    }
    collect(m, lease->backing);
    lease->active = 0;
    lease->ptr = NULL;
    unlock(m);
}

static uint64_t current_owner(Model *m, unsigned backing, unsigned page)
{
    uint64_t owner;
    lock(m);
    owner = m->pages[backing][page].state == LIVE
          ? m->pages[backing][page].owner : 0;
    unlock(m);
    return owner;
}

static int unique_backing(Model *m, unsigned page)
{
    int result = -1;
    int live = 0;
    lock(m);
    for (int b = 0; b < BACKINGS; b++) {
        if (m->pages[b][page].state == FREE) continue;
        if (result != -1) { result = -1; live = 0; break; }
        result = b;
        live = m->pages[b][page].state == LIVE;
    }
    unlock(m);
    return live ? result : -1;
}

typedef struct {
    Model *model;
    uint64_t owner;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int retired;
} RetireWork;

static void *retire_worker(void *arg)
{
    RetireWork *work = arg;
    CHECK(retire(work->model, 0, work->owner));
    CHECK(!pthread_mutex_lock(&work->lock));
    work->retired = 1;
    CHECK(!pthread_cond_signal(&work->cond));
    CHECK(!pthread_mutex_unlock(&work->lock));
    return NULL;
}

static void test_pin_retirement(Model *m)
{
    uint64_t owner = allocate(m, 0, 1, 3), new_owner;
    Lease first = {0}, second = {0}, bad = {0};
    RetireWork work = { m, owner, PTHREAD_MUTEX_INITIALIZER,
                        PTHREAD_COND_INITIALIZER, 0 };
    pthread_t thread;

    CHECK(owner);
    CHECK(!pin(m, 0, owner, PAGE, 0, &bad));
    CHECK(!pin(m, 0, owner, SIZE_MAX, 4, &bad));
    CHECK(!pin(m, 0, owner, 3 * PAGE, PAGE + 1, &bad));
    CHECK(pin(m, 0, owner, PAGE + PAGE - 5, PAGE + 10, &first));
    CHECK(pin(m, 0, owner, 2 * PAGE, 1, &second));
    CHECK(!pthread_create(&thread, NULL, retire_worker, &work));
    CHECK(!pthread_mutex_lock(&work.lock));
    while (!work.retired) CHECK(!pthread_cond_wait(&work.cond, &work.lock));
    CHECK(!pthread_mutex_unlock(&work.lock));
    CHECK(!pthread_join(thread, NULL));
    CHECK(!pin(m, 0, owner, PAGE, 1, &bad));
    CHECK(!allocate(m, 0, 1, 3));
    CHECK(first.ptr[0] == (uint8_t)owner &&
          first.ptr[PAGE + 9] == (uint8_t)owner);
    release(&first);
    CHECK(!allocate(m, 0, 1, 3));
    release(&second);
    new_owner = allocate(m, 0, 1, 3);
    CHECK(new_owner && new_owner != owner);
    CHECK(!pin(m, 0, owner, PAGE, 2 * PAGE, &bad));
    CHECK(pin(m, 0, new_owner, PAGE, 2 * PAGE, &bad));
    release(&bad);
    CHECK(retire(m, 0, new_owner));
    CHECK(!pthread_cond_destroy(&work.cond));
    CHECK(!pthread_mutex_destroy(&work.lock));
}

static void test_aliases(Model *m)
{
    uint64_t ram = allocate(m, 0, 2, 2);
    uint64_t window = allocate(m, 1, 2, 2);
    Lease a = {0}, b = {0}, bad = {0};
    CHECK(ram && window && ram != window);
    CHECK(unique_backing(m, 2) == -1);
    CHECK(!pin(m, 0, window, 2 * PAGE, PAGE + 1, &bad));
    CHECK(pin(m, 0, ram, 2 * PAGE, PAGE + 1, &a));
    CHECK(pin(m, 1, window, 2 * PAGE, PAGE + 1, &b));
    CHECK(a.ptr != b.ptr && a.ptr[0] == (uint8_t)ram &&
          b.ptr[0] == (uint8_t)window);
    CHECK(retire(m, 0, ram));
    CHECK(!allocate(m, 0, 2, 2));
    CHECK(unique_backing(m, 2) == -1);
    release(&a);
    CHECK(unique_backing(m, 2) == 1);
    CHECK(retire(m, 1, window));
    CHECK(!allocate(m, 1, 2, 2));
    release(&b);
}

static void test_split_generation(Model *m)
{
    uint64_t first = allocate(m, 0, 0, 2);
    uint64_t second = allocate(m, 0, 2, 2);
    uint64_t whole;
    Lease bad = {0};
    CHECK(first && second && first != second);
    CHECK(!pin(m, 0, first, 2 * PAGE - 1, 2, &bad));
    CHECK(retire(m, 0, first));
    CHECK(retire(m, 0, second));
    whole = allocate(m, 0, 0, 4);
    CHECK(whole);
    CHECK(retire(m, 0, whole));
}

typedef struct {
    Model *model;
    int stop, failures, readers_started, successful_pins;
} Stress;

static void *stress_reader(void *arg)
{
    Stress *s = arg;
    int started = 0;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE)) {
        Lease lease = {0};
        uint64_t owner = current_owner(s->model, 0, 0);
        if (pin(s->model, 0, owner, PAGE - 3, 2 * PAGE + 6, &lease)) {
            if (!started) {
                started = 1;
                __atomic_add_fetch(&s->readers_started, 1, __ATOMIC_RELEASE);
            }
            __atomic_add_fetch(&s->successful_pins, 1, __ATOMIC_RELAXED);
            for (unsigned i = 0; i < 2 * PAGE + 6; i++)
                if (lease.ptr[i] != (uint8_t)owner)
                    __atomic_add_fetch(&s->failures, 1, __ATOMIC_RELAXED);
            release(&lease);
        }
    }
    return NULL;
}

static void test_stress(Model *m)
{
    Stress s = { .model = m };
    pthread_t readers[READERS];
    uint64_t owner = allocate(m, 0, 0, 4);
    CHECK(owner);
    for (unsigned i = 0; i < READERS; i++)
        CHECK(!pthread_create(&readers[i], NULL, stress_reader, &s));
    while (__atomic_load_n(&s.readers_started, __ATOMIC_ACQUIRE) != READERS)
        sched_yield();
    for (unsigned i = 0; i < ROUNDS; i++) {
        CHECK(retire(m, 0, owner));
        do {
            owner = allocate(m, 0, 0, 4);
            if (!owner) sched_yield();
        } while (!owner);
    }
    __atomic_store_n(&s.stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < READERS; i++)
        CHECK(!pthread_join(readers[i], NULL));
    CHECK(!__atomic_load_n(&s.failures, __ATOMIC_RELAXED));
    CHECK(__atomic_load_n(&s.successful_pins, __ATOMIC_RELAXED) >= READERS);
    CHECK(retire(m, 0, owner));
}

int main(void)
{
    Model m = { .lock = PTHREAD_MUTEX_INITIALIZER };
    test_pin_retirement(&m);
    test_aliases(&m);
    test_split_generation(&m);
    test_stress(&m);
    CHECK(!pthread_mutex_destroy(&m.lock));
    return 0;
}
