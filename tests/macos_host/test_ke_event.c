#include "bw_posix_ke_event.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

typedef struct Wait {
    const BwPosixGuestMem *mem;
    uint32_t objects[2];
    size_t count;
    int wait_all;
    int64_t timeout;
    BwKeResult result;
    uint32_t selected;
} Wait;

typedef struct Stress {
    const BwPosixGuestMem *mem;
    uint32_t va;
    int failed;
} Stress;

typedef struct AlertBlock {
    const BwPosixGuestMem *mem;
    uint32_t va;
    atomic_int entered;
} AlertBlock;

static void pause_ms(long ms)
{
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

static int wait_for_count(uint32_t va, unsigned expected)
{
    for (unsigned i = 0; i < 4000; i++) {
        if (bw_posix_ke_test_waiters(va) == expected)
            return 1;
        pause_ms(1);
    }
    return 0;
}

static void *run_wait(void *context)
{
    Wait *w = context;
    w->result = bw_posix_ke_wait(w->mem, w->objects, w->count, w->wait_all,
                                 &w->timeout, NULL, NULL, &w->selected);
    return NULL;
}

static void *run_stress(void *context)
{
    Stress *s = context;
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t previous;
        BwKeOperation op = (i & 1u) ? BW_KE_SET : BW_KE_RESET;
        if (bw_posix_ke_modify(s->mem, s->va, op, &previous) != BW_KE_OK ||
            previous > 1) {
            s->failed = 1;
            break;
        }
    }
    return NULL;
}

static int alert_after_three(void *context)
{
    unsigned *count = context;
    return ++*count == 3;
}

static int signal_during_alert_poll(void *context)
{
    Stress *s = context;
    if (bw_posix_ke_modify(s->mem, s->va, BW_KE_SET, NULL) != BW_KE_OK)
        s->failed = 1;
    return 0;
}

static int blocked_alert_poll(void *context)
{
    AlertBlock *block = context;
    atomic_store(&block->entered, 1);
    for (;;)
        pause_ms(100);
}

static void *run_cancellable_wait(void *context)
{
    AlertBlock *block = context;
    uint32_t selected;
    (void)bw_posix_ke_wait(block->mem, &block->va, 1, 0, NULL,
                           blocked_alert_poll, block, &selected);
    return NULL;
}

static void *run_infinite_wait(void *context)
{
    AlertBlock *block = context;
    uint32_t selected;
    (void)bw_posix_ke_wait(block->mem, &block->va, 1, 0, NULL,
                           NULL, NULL, &selected);
    return NULL;
}

static uint32_t state(const BwPosixGuestMem *mem, uint32_t va)
{
    uint32_t value = UINT32_MAX;
    (void)bw_posix_guest_read(mem, va + 4, &value, sizeof value);
    return value;
}

int main(void)
{
    long page_long = sysconf(_SC_PAGESIZE);
    size_t page;
    uint8_t *base;
    BwPosixGuestMem mem;
    BwKeHeader header;
    uint32_t first, second, third, protected_va, previous, selected;
    int64_t immediate = 0, short_wait = -500000;
    pthread_t threads[4];
    Wait waits[4];
    unsigned alerts = 0;

    CHECK(page_long > 0);
    page = (size_t)page_long;
    base = mmap(NULL, page * 24, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED);
    mem = (BwPosixGuestMem){(uintptr_t)base, page * 24, 0, 0, 0};
    first = (uint32_t)page + 0x100;
    second = first + 0x20;
    third = second + 0x20;
    protected_va = (uint32_t)page * 3;

    CHECK(bw_posix_ke_initialize(&mem, first, 0, 0) == BW_KE_OK);
    CHECK(base[first] == 0 && base[first + 2] == 4);
    uint32_t list_head;
    memcpy(&list_head, base + first + 8, sizeof list_head);
    CHECK(list_head == first + 8);
    memcpy(&list_head, base + first + 12, sizeof list_head);
    CHECK(list_head == first + 8);
    CHECK(bw_posix_ke_probe(&mem, first, &header) == BW_KE_OK);
    CHECK(header.type == 0 && header.size == 4 && header.state == 0);
    CHECK(bw_posix_ke_initialize(&mem, 0, 0, 0) == BW_KE_INVALID);
    CHECK(bw_posix_ke_initialize(&mem, first, 2, 0) == BW_KE_INVALID);
    CHECK(bw_posix_ke_initialize(&mem, first, 0, 2) == BW_KE_INVALID);
    CHECK(bw_posix_ke_initialize(&mem, first + 1, 0, 0) == BW_KE_INVALID);

    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_SET, &previous) == BW_KE_OK);
    CHECK(previous == 0 && state(&mem, first) == 1);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_SET, &previous) == BW_KE_OK);
    CHECK(previous == 1);
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_OK);
    CHECK(selected == 0 && state(&mem, first) == 1);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_RESET, &previous) == BW_KE_OK);
    CHECK(previous == 1 && state(&mem, first) == 0);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_RESET, &previous) == BW_KE_OK);
    CHECK(previous == 0);
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &short_wait, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);
    int64_t expired_absolute = 1;
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &expired_absolute, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);
    struct timespec realtime;
    CHECK(clock_gettime(CLOCK_REALTIME, &realtime) == 0);
    int64_t future_absolute =
        ((int64_t)realtime.tv_sec + INT64_C(11644473600)) * INT64_C(10000000) +
        realtime.tv_nsec / 100 + 100000;
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &future_absolute, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);

    CHECK(bw_posix_ke_initialize(&mem, second, 1, 1) == BW_KE_OK);
    CHECK(bw_posix_ke_wait(&mem, &second, 1, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_OK);
    CHECK(state(&mem, second) == 0);
    CHECK(bw_posix_ke_wait(&mem, &second, 1, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);
    CHECK(bw_posix_ke_initialize(&mem, third, 1, 0) == BW_KE_OK);
    uint32_t pair[2] = {second, third};
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_wait(&mem, pair, 2, 1, &immediate, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);
    CHECK(state(&mem, second) == 1);
    CHECK(bw_posix_ke_modify(&mem, third, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_wait(&mem, pair, 2, 1, &immediate, NULL, NULL,
                           &selected) == BW_KE_OK);
    CHECK(selected == 0 && state(&mem, second) == 0 && state(&mem, third) == 0);
    CHECK(bw_posix_ke_modify(&mem, third, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_wait(&mem, pair, 2, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_OK);
    CHECK(selected == 1 && state(&mem, third) == 0);
    uint32_t duplicate[2] = {first, first};
    CHECK(bw_posix_ke_wait(&mem, duplicate, 2, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_INVALID);
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, NULL, alert_after_three,
                           &alerts, &selected) == BW_KE_ALERTED);
    CHECK(alerts == 3);
    Stress alert_signal = {&mem, second, 0};
    CHECK(bw_posix_ke_wait(&mem, &second, 1, 0, NULL, signal_during_alert_poll,
                           &alert_signal, &selected) == BW_KE_OK);
    CHECK(!alert_signal.failed && state(&mem, second) == 0);

    AlertBlock cancel_block = {&mem, first, 0};
    void *cancel_result;
    CHECK(pthread_create(&threads[0], NULL, run_infinite_wait, &cancel_block) == 0);
    CHECK(wait_for_count(first, 1));
    CHECK(pthread_cancel(threads[0]) == 0);
    CHECK(pthread_join(threads[0], &cancel_result) == 0);
    CHECK(cancel_result == PTHREAD_CANCELED);
    CHECK(bw_posix_ke_test_waiters(first) == 0);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_RESET, NULL) == BW_KE_OK);
    CHECK(pthread_create(&threads[0], NULL, run_cancellable_wait, &cancel_block) == 0);
    for (unsigned i = 0; i < 4000 && !atomic_load(&cancel_block.entered); i++)
        pause_ms(1);
    CHECK(atomic_load(&cancel_block.entered));
    CHECK(wait_for_count(first, 1));
    CHECK(pthread_cancel(threads[0]) == 0);
    CHECK(pthread_join(threads[0], &cancel_result) == 0);
    CHECK(cancel_result == PTHREAD_CANCELED);
    CHECK(bw_posix_ke_test_waiters(first) == 0);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_RESET, NULL) == BW_KE_OK);

    for (unsigned i = 0; i < 3; i++) {
        waits[i] = (Wait){&mem, {first, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
        CHECK(pthread_create(&threads[i], NULL, run_wait, &waits[i]) == 0);
    }
    CHECK(wait_for_count(first, 3));
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_PULSE, &previous) == BW_KE_OK);
    CHECK(previous == 0);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(waits[i].result == BW_KE_OK);
    }
    CHECK(state(&mem, first) == 0);
    CHECK(bw_posix_ke_wait(&mem, &first, 1, 0, &immediate, NULL, NULL,
                           &selected) == BW_KE_TIMEOUT);

    for (unsigned i = 0; i < 3; i++) {
        waits[i] = (Wait){&mem, {second, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
        CHECK(pthread_create(&threads[i], NULL, run_wait, &waits[i]) == 0);
    }
    CHECK(wait_for_count(second, 3));
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_PULSE, NULL) == BW_KE_OK);
    CHECK(wait_for_count(second, 2));
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(wait_for_count(second, 1));
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_SET, NULL) == BW_KE_OK);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(waits[i].result == BW_KE_OK);
    }
    CHECK(state(&mem, second) == 0);

    for (unsigned iteration = 0; iteration < 20; iteration++) {
        for (unsigned i = 0; i < 2; i++) {
            waits[i] = (Wait){&mem, {second, 0}, 1, 0, -30000000,
                              BW_KE_INVALID, 99};
            CHECK(pthread_create(&threads[i], NULL, run_wait, &waits[i]) == 0);
        }
        CHECK(wait_for_count(second, 2));
        CHECK(bw_posix_ke_modify(&mem, second, BW_KE_PULSE, NULL) == BW_KE_OK);
        CHECK(bw_posix_ke_modify(&mem, second, BW_KE_PULSE, NULL) == BW_KE_OK);
        for (unsigned i = 0; i < 2; i++) {
            CHECK(pthread_join(threads[i], NULL) == 0);
            CHECK(waits[i].result == BW_KE_OK);
        }
    }

    waits[0] = (Wait){&mem, {second, third}, 2, 1, -30000000,
                      BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(second, 1));
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_PULSE, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, third, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_test_waiters(second) == 1);
    CHECK(state(&mem, third) == 1);
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_OK);
    CHECK(state(&mem, second) == 0 && state(&mem, third) == 0);

    CHECK(bw_posix_ke_modify(&mem, third, BW_KE_SET, NULL) == BW_KE_OK);
    waits[0] = (Wait){&mem, {second, third}, 2, 1, -30000000,
                      BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(second, 1));
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_PULSE, NULL) == BW_KE_OK);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_OK && state(&mem, third) == 0);

    waits[0] = (Wait){&mem, {first, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(first, 1));
    CHECK(bw_posix_ke_initialize(&mem, first, 0, 0) == BW_KE_OK);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_CHANGED);

    waits[0] = (Wait){&mem, {first, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(first, 1));
    bw_posix_ke_invalidate(first);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_CHANGED);

    CHECK(bw_posix_ke_initialize(&mem, protected_va, 1, 0) == BW_KE_OK);
    waits[0] = (Wait){&mem, {protected_va, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(protected_va, 1));
    CHECK(mprotect(base + page * 3, page, PROT_READ) == 0);
    CHECK(bw_posix_ke_initialize(&mem, protected_va, 0, 1) == BW_KE_INACCESSIBLE);
    CHECK(bw_posix_ke_modify(&mem, protected_va, BW_KE_SET, NULL) == BW_KE_INACCESSIBLE);
    CHECK(mprotect(base + page * 3, page, PROT_READ | PROT_WRITE) == 0);
    CHECK(bw_posix_ke_modify(&mem, protected_va, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, protected_va, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(mprotect(base + page * 3, page, PROT_NONE) == 0);
    CHECK(bw_posix_ke_probe(&mem, protected_va, NULL) == BW_KE_INACCESSIBLE);
    CHECK(bw_posix_ke_modify(&mem, protected_va, BW_KE_SET, NULL) == BW_KE_INACCESSIBLE);
    CHECK(bw_posix_ke_probe(&mem, (uint32_t)(page * 3 - 8), NULL) == BW_KE_INACCESSIBLE);
    CHECK(mprotect(base + page * 3, page, PROT_READ) == 0);
    uint32_t ro_pair[2] = {second, protected_va};
    CHECK(bw_posix_ke_wait(&mem, ro_pair, 2, 1, &immediate, NULL, NULL,
                           &selected) == BW_KE_INACCESSIBLE);
    CHECK(state(&mem, second) == 1 && state(&mem, protected_va) == 1);
    CHECK(mprotect(base + page * 3, page, PROT_READ | PROT_WRITE) == 0);
    CHECK(bw_posix_ke_modify(&mem, second, BW_KE_RESET, NULL) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, protected_va, BW_KE_RESET, NULL) == BW_KE_OK);

    base[third] = 2;
    CHECK(bw_posix_ke_probe(&mem, third, NULL) == BW_KE_NOT_EVENT);
    base[third] = 1;
    base[third + 2] = 16;
    CHECK(bw_posix_ke_probe(&mem, third, NULL) == BW_KE_NOT_EVENT);
    base[third + 2] = 4;
    uint32_t invalid_state = UINT32_MAX;
    memcpy(base + third + 4, &invalid_state, sizeof invalid_state);
    CHECK(bw_posix_ke_probe(&mem, third, NULL) == BW_KE_NOT_EVENT);
    CHECK(bw_posix_ke_probe(&mem, third + 1, NULL) == BW_KE_INACCESSIBLE);
    CHECK(bw_posix_ke_probe(&mem, UINT32_MAX - 3, NULL) == BW_KE_INACCESSIBLE);

    CHECK(bw_posix_ke_initialize(&mem, third, 1, 0) == BW_KE_OK);
    Stress stress[4];
    for (unsigned i = 0; i < 4; i++) {
        stress[i] = (Stress){&mem, third, 0};
        CHECK(pthread_create(&threads[i], NULL, run_stress, &stress[i]) == 0);
    }
    for (unsigned i = 0; i < 4; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(!stress[i].failed);
    }
    CHECK(state(&mem, third) <= 1);

    waits[0] = (Wait){&mem, {first, 0}, 1, 0, -30000000, BW_KE_INVALID, 99};
    CHECK(pthread_create(&threads[0], NULL, run_wait, &waits[0]) == 0);
    CHECK(wait_for_count(first, 1));
    for (unsigned i = 0; i < 4200; i++)
        CHECK(bw_posix_ke_initialize(&mem, (uint32_t)(page * 5 + i * 16),
                                     0, 0) == BW_KE_OK);
    CHECK(bw_posix_ke_modify(&mem, first, BW_KE_SET, NULL) == BW_KE_OK);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(waits[0].result == BW_KE_OK);
    CHECK(munmap(base, page * 24) == 0);
    return 0;
}
