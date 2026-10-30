#include "../../src/toolkit_patches/kernel/xbox_memory_layout.c"

#include <sched.h>

enum { ITERATIONS = 100000 };
static int s_start, s_done;

LONG InterlockedCompareExchange(volatile LONG *target, LONG exchange, LONG compare)
{
    __atomic_compare_exchange_n(target, &compare, exchange, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return compare;
}

LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

VOID SwitchToThread(void) { sched_yield(); }
DWORD GetTickCount(void) { return 1; }

static void *update_names(void *unused)
{
    uint32_t va = XBOX_HEAP_BASE;
    (void)unused;
    while (!__atomic_load_n(&s_start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (unsigned i = 0; i < ITERATIONS; i++) {
        xbox_PhysNoteRamName(va);
        heap_forget_ram_names(0);
        contig_take(1, 1);
        if (!xbox_ContiguousFree(XBOX_CONTIG_BASE + CONTIG_PAGE))
            abort();
    }
    __atomic_store_n(&s_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    pthread_t writer;
    uint32_t va = XBOX_HEAP_BASE;
    const volatile uint8_t *names = xbox_PhysRamNameMap();
    const volatile uint8_t *contiguous = xbox_ContiguousPageMap();

    if (va >= XBOX_HEAP_TOP || va >= XBOX_CONTIG_SIZE || va % CONTIG_PAGE)
        return 1;
    g_heap_blocks[0].addr = va;
    g_heap_blocks[0].size = CONTIG_PAGE;
    g_heap_blocks[0].free = 1;
    g_heap_block_count = 1;
    if (setenv("RECOMP_HEAP_RECLAIM", "1", 1) != 0)
        return 1;
    if (pthread_create(&writer, NULL, update_names, NULL) != 0)
        return 1;
    __atomic_store_n(&s_start, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_done, __ATOMIC_ACQUIRE)) {
        if (bw_shared_u8_load(&names[va / CONTIG_PAGE]) > 1 ||
                bw_shared_u8_load(&contiguous[1]) > 1)
            return 1;
    }
    return pthread_join(writer, NULL) != 0;
}
