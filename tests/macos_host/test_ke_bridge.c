#include "../../src/toolkit_patches/kernel/kernel_bridge.c"

#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
size_t g_xbox_map_size, g_xbox_total_ram;
static RECOMP_TLS atomic_int *test_block_apc;

int bw_xbox_contiguous_memory_region(void **host, uint32_t *va, size_t *bytes)
{
    (void)host;
    (void)va;
    (void)bytes;
    return 0;
}

static int bridge_run_io_apcs(uint32_t alertable, uint32_t wait_mode)
{
    (void)wait_mode;
    if (alertable && test_block_apc) {
        atomic_store(test_block_apc, 1);
        for (;;)
            usleep(10000);
    }
    return 0;
}

static void args(uint8_t *base, const uint32_t *values, size_t count)
{
    memcpy(base + g_esp, values, count * sizeof *values);
}

static uint32_t load32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void *shadow_writer(void *context)
{
    uint32_t va = *(uint32_t *)context;
    for (unsigned i = 0; i < 1000; i++) {
        HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!h)
            return (void *)(uintptr_t)1;
        ke_shadow_insert(va, h);
    }
    return NULL;
}

static void *shadow_reader(void *context)
{
    uint32_t va = *(uint32_t *)context;
    for (unsigned i = 0; i < 3000; i++) {
        HANDLE h = ke_shadow_acquire(va);
        if (h) {
            BOOL ok = SetEvent(h);
            CloseHandle(h);
            if (!ok)
                return (void *)(uintptr_t)1;
        }
    }
    return NULL;
}

static void *token_writer(void *context)
{
    (void)context;
    for (unsigned i = 0; i < 1000; i++) {
        HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!h)
            return (void *)(uintptr_t)1;
        uint32_t token = bridge_handle_token(h);
        if (!token || !CloseHandle(bridge_take_handle(token)))
            return (void *)(uintptr_t)1;
    }
    return NULL;
}

static void *token_reader(void *context)
{
    (void)context;
    for (unsigned i = 0; i < 3000; i++) {
        HANDLE h = bridge_tagged_acquire(BRIDGE_HANDLE_TAG | 1u);
        if (h) {
            BOOL ok = SetEvent(h);
            CloseHandle(h);
            if (!ok)
                return (void *)(uintptr_t)1;
        }
    }
    return NULL;
}

typedef struct CancelShadow {
    uint32_t va;
    atomic_int started;
} CancelShadow;

static void *cancel_shadow_writer(void *context)
{
    CancelShadow *s = context;
    for (;;) {
        HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!h)
            return (void *)(uintptr_t)1;
        ke_shadow_insert(s->va, h);
        atomic_store(&s->started, 1);
        pthread_testcancel();
    }
}

static void *cancel_shadow_reinitializer(void *context)
{
    CancelShadow *s = context;
    BwPosixGuestMem mem = bridge_ke_memory();
    for (;;) {
        if (ke_initialize_guest_event(&mem, s->va, 0, 0) != BW_KE_OK)
            return (void *)(uintptr_t)1;
        atomic_store(&s->started, 1);
        pthread_testcancel();
    }
}

typedef struct CancelNative {
    uint8_t *base;
    uint32_t stack_va;
    uint32_t object;
    uint32_t array_va;
    int multiple;
    int block_apc;
    int asynchronous;
    atomic_int started;
} CancelNative;

static void *cancel_native_wait(void *context)
{
    CancelNative *w = context;
    uint32_t a[8] = {0};
    g_esp = w->stack_va;
    if (w->asynchronous)
        pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    if (w->block_apc)
        test_block_apc = &w->started;
    if (w->multiple) {
        a[0] = 2;
        a[1] = w->array_va;
        a[2] = 1;
        a[5] = w->block_apc;
        args(w->base, a, 8);
    } else {
        a[0] = w->object;
        a[3] = w->block_apc;
        args(w->base, a, 5);
    }
    if (!w->block_apc)
        atomic_store(&w->started, 1);
    if (w->multiple)
        bridge_KeWaitForMultipleObjects();
    else
        bridge_KeWaitForSingleObject();
    return (void *)(uintptr_t)1;
}

int main(void)
{
    long page_long = sysconf(_SC_PAGESIZE);
    size_t page;
    uint8_t *base;
    uint32_t event_va, second_va, shadow_va, array_va, timeout_va;
    uint32_t a[8] = {0};
    HANDLE h, retained;
    int64_t zero = 0;
    pthread_t workers[4];
    void *worker_result;

    CHECK(page_long > 0);
    page = (size_t)page_long;
    base = mmap(NULL, page * 6, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(base != MAP_FAILED);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)base;
    g_xbox_map_size = page * 6;
    g_xbox_total_ram = g_xbox_map_size;
    g_esp = (uint32_t)page;
    event_va = (uint32_t)page + 0x100;
    second_va = event_va + 0x20;
    shadow_va = second_va + 0x20;
    array_va = shadow_va + 0x20;
    timeout_va = array_va + 0x20;
    memcpy(base + timeout_va, &zero, sizeof zero);

    CHECK(pthread_create(&workers[0], NULL, shadow_writer, &shadow_va) == 0);
    CHECK(pthread_create(&workers[1], NULL, shadow_writer, &shadow_va) == 0);
    CHECK(pthread_create(&workers[2], NULL, shadow_reader, &shadow_va) == 0);
    CHECK(pthread_create(&workers[3], NULL, shadow_reader, &shadow_va) == 0);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(pthread_join(workers[i], &worker_result) == 0);
        CHECK(worker_result == NULL);
    }
    ke_shadow_remove(shadow_va);
    CancelShadow cancel_shadow = {shadow_va, 0};
    CHECK(pthread_create(&workers[0], NULL, cancel_shadow_writer,
                         &cancel_shadow) == 0);
    for (unsigned i = 0; i < 4000 && !atomic_load(&cancel_shadow.started); i++)
        usleep(1000);
    CHECK(atomic_load(&cancel_shadow.started));
    CHECK(pthread_cancel(workers[0]) == 0);
    CHECK(pthread_join(workers[0], &worker_result) == 0);
    CHECK(worker_result == PTHREAD_CANCELED);
    retained = ke_shadow_acquire(shadow_va);
    CHECK(retained != NULL && SetEvent(retained));
    CHECK(CloseHandle(retained));
    ke_shadow_remove(shadow_va);
    atomic_store(&cancel_shadow.started, 0);
    CHECK(pthread_create(&workers[0], NULL, cancel_shadow_reinitializer,
                         &cancel_shadow) == 0);
    for (unsigned i = 0; i < 4000 && !atomic_load(&cancel_shadow.started); i++)
        usleep(1000);
    CHECK(atomic_load(&cancel_shadow.started));
    CHECK(pthread_cancel(workers[0]) == 0);
    CHECK(pthread_join(workers[0], &worker_result) == 0);
    CHECK(worker_result == PTHREAD_CANCELED);
    BwPosixGuestMem mem = bridge_ke_memory();
    CHECK(ke_initialize_guest_event(&mem, shadow_va, 0, 0) == BW_KE_OK);
    CHECK(pthread_create(&workers[0], NULL, token_writer, NULL) == 0);
    CHECK(pthread_create(&workers[1], NULL, token_writer, NULL) == 0);
    CHECK(pthread_create(&workers[2], NULL, token_reader, NULL) == 0);
    CHECK(pthread_create(&workers[3], NULL, token_reader, NULL) == 0);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(pthread_join(workers[i], &worker_result) == 0);
        CHECK(worker_result == NULL);
    }

    /* A title-built synchronization event, without KeInitializeEvent. */
    base[event_va] = 1;
    base[event_va + 2] = 4;
    a[0] = event_va;
    args(base, a, 3);
    bridge_KeSetEvent();
    CHECK(g_eax == 0 && load32(base + event_va + 4) == 1);
    bridge_KeSetEvent();
    CHECK(g_eax == 1 && load32(base + event_va + 4) == 1);
    a[3] = 0;
    a[4] = timeout_va;
    args(base, a, 5);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == 0 && load32(base + event_va + 4) == 0);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == (uint32_t)STATUS_TIMEOUT);

    a[0] = second_va;
    a[1] = 0;
    a[2] = 1;
    args(base, a, 3);
    bridge_KeInitializeEvent();
    CHECK(g_eax == 0 && base[second_va] == 0 && base[second_va + 2] == 4);
    CHECK(load32(base + second_va + 4) == 1);
    bridge_KeResetEvent();
    CHECK(g_eax == 1 && load32(base + second_va + 4) == 0);
    bridge_KePulseEvent();
    CHECK(g_eax == 0 && load32(base + second_va + 4) == 0);
    bridge_KeSetEventBoostPriority();
    CHECK(g_eax == 0 && load32(base + second_va + 4) == 1);

    uint32_t pair[2] = {event_va, second_va};
    memcpy(base + array_va, pair, sizeof pair);
    memset(a, 0, sizeof a);
    a[0] = 2;
    a[1] = array_va;
    a[2] = 1;
    a[6] = timeout_va;
    args(base, a, 8);
    bridge_KeWaitForMultipleObjects();
    CHECK(g_eax == 1 && load32(base + second_va + 4) == 1);
    a[2] = 0;
    args(base, a, 8);
    bridge_KeWaitForMultipleObjects();
    CHECK(g_eax == (uint32_t)STATUS_TIMEOUT);
    CHECK(load32(base + second_va + 4) == 1);

    /* A known native shadow is retained even after replacement/removal. */
    h = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(h != NULL);
    ke_shadow_insert(shadow_va, h);
    retained = ke_shadow_acquire(shadow_va);
    CHECK(retained != NULL);
    ke_shadow_remove(shadow_va);
    CHECK(SetEvent(retained));
    CHECK(WaitForSingleObject(retained, 0) == WAIT_OBJECT_0);
    CHECK(CloseHandle(retained));

    h = CreateEventW(NULL, TRUE, TRUE, NULL);
    CHECK(h != NULL);
    ke_shadow_insert(shadow_va, h);
    memset(a, 0, sizeof a);
    a[0] = shadow_va;
    a[4] = timeout_va;
    args(base, a, 5);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == 0);
    pair[0] = event_va;
    pair[1] = shadow_va;
    memcpy(base + array_va, pair, sizeof pair);
    memset(a, 0, sizeof a);
    a[0] = 2;
    a[1] = array_va;
    a[2] = 1;
    a[6] = timeout_va;
    args(base, a, 8);
    bridge_KeWaitForMultipleObjects();
    CHECK(g_eax == 0xC00000BBu);

    /* Native waits remain real, bounded, and safe to cancel while retained. */
    h = CreateEventW(NULL, FALSE, FALSE, NULL);
    CHECK(h != NULL);
    ke_shadow_insert(shadow_va, h);
    memset(a, 0, sizeof a);
    a[0] = shadow_va;
    a[4] = timeout_va;
    int64_t relative = -200000;
    memcpy(base + timeout_va, &relative, sizeof relative);
    args(base, a, 5);
    uint64_t before = bw_posix_ke_monotonic_ns();
    bridge_KeWaitForSingleObject();
    uint64_t elapsed = bw_posix_ke_monotonic_ns() - before;
    CHECK(g_eax == (uint32_t)STATUS_TIMEOUT);
    CHECK(elapsed >= UINT64_C(10000000) && elapsed < UINT64_C(2000000000));
    memcpy(base + timeout_va, &zero, sizeof zero);

    uint32_t other_shadow_va = shadow_va + 0x100;
    HANDLE other = CreateEventW(NULL, FALSE, TRUE, NULL);
    CHECK(other != NULL);
    ke_shadow_insert(other_shadow_va, other);
    pair[0] = shadow_va;
    pair[1] = other_shadow_va;
    memcpy(base + array_va, pair, sizeof pair);
    memset(a, 0, sizeof a);
    a[0] = 2;
    a[1] = array_va;
    a[2] = 1;
    a[6] = timeout_va;
    args(base, a, 8);
    bridge_KeWaitForMultipleObjects();
    CHECK(g_eax == 1);
    CHECK(SetEvent(h));
    a[2] = 0;
    args(base, a, 8);
    bridge_KeWaitForMultipleObjects();
    CHECK(g_eax == 0xC00000BBu);
    CHECK(WaitForSingleObject(h, 0) == WAIT_OBJECT_0);

    retained = ke_shadow_acquire(shadow_va);
    CHECK(retained != NULL);
    for (unsigned i = 0; i < 3; i++) {
        CancelNative native = {
            base, (uint32_t)(page * 2 + 0x100), shadow_va, array_va,
            i == 1, i == 2, i == 1, 0
        };
        CHECK(pthread_create(&workers[0], NULL, cancel_native_wait, &native) == 0);
        for (unsigned j = 0; j < 4000 && !atomic_load(&native.started); j++)
            usleep(1000);
        CHECK(atomic_load(&native.started));
        if (!native.block_apc)
            usleep(30000);
        CHECK(pthread_cancel(workers[0]) == 0);
        CHECK(pthread_join(workers[0], &worker_result) == 0);
        CHECK(worker_result == PTHREAD_CANCELED);
        CHECK(SetEvent(retained));
        CHECK(WaitForSingleObject(retained, 0) == WAIT_OBJECT_0);
    }
    CHECK(CloseHandle(retained));
    ke_shadow_remove(other_shadow_va);
    CHECK(SetEvent(h));

    retained = ke_shadow_acquire(shadow_va);
    CHECK(retained != NULL);
    memset(a, 0, sizeof a);
    a[0] = shadow_va;
    a[1] = 1;
    args(base, a, 3);
    bridge_KeInitializeEvent();
    CHECK(g_eax == 0 && ke_shadow_acquire(shadow_va) == NULL);
    CHECK(WaitForSingleObject(retained, 0) == WAIT_OBJECT_0);
    CHECK(CloseHandle(retained));

    h = CreateEventW(NULL, TRUE, TRUE, NULL);
    CHECK(h != NULL);
    uint32_t token = bridge_handle_token(h);
    CHECK(token != 0);
    memset(a, 0, sizeof a);
    a[0] = token;
    a[4] = timeout_va;
    args(base, a, 5);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == 0);
    retained = bridge_tagged_acquire(token);
    CHECK(retained != NULL);
    CHECK(CloseHandle(bridge_take_handle(token)));
    CHECK(WaitForSingleObject(retained, 0) == WAIT_OBJECT_0);
    CHECK(CloseHandle(retained));
    CHECK(bridge_tagged_acquire(token) == NULL);

    /* Invalid guest objects fail closed rather than becoming native HANDLEs. */
    memset(a, 0, sizeof a);
    a[0] = (uint32_t)page + 0x300;
    base[a[0] + 2] = 16;
    args(base, a, 3);
    bridge_KeSetEvent();
    CHECK(g_eax == 0xC0000024u);
    a[0] = (uint32_t)(page * 8);
    args(base, a, 3);
    bridge_KeSetEvent();
    CHECK(g_eax == 0xC0000005u);
    a[0] = event_va;
    a[4] = (uint32_t)(page * 8);
    args(base, a, 5);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == 0xC0000005u);
    a[0] = 0;
    a[4] = timeout_va;
    args(base, a, 5);
    bridge_KeWaitForSingleObject();
    CHECK(g_eax == 0xC0000005u);
    CHECK(mprotect(base + page * 3, page, PROT_NONE) == 0);
    a[0] = (uint32_t)(page * 3);
    args(base, a, 3);
    bridge_KeSetEvent();
    CHECK(g_eax == 0xC0000005u);
    CHECK(mprotect(base + page * 3, page, PROT_READ | PROT_WRITE) == 0);

    h = CreateSemaphoreW(NULL, 0, 3, NULL);
    CHECK(h != NULL);
    ke_shadow_insert(shadow_va, h);
    memset(a, 0, sizeof a);
    a[0] = shadow_va;
    a[2] = 1;
    args(base, a, 4);
    bridge_KeReleaseSemaphore();
    CHECK(g_eax == 0);
    bridge_KeReleaseSemaphore();
    CHECK(g_eax == 1);
    ke_shadow_remove(shadow_va);

    CHECK(munmap(base, page * 6) == 0);
    return 0;
}
