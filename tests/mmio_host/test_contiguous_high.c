#include "../../src/toolkit_patches/kernel/xbox_memory_layout.c"

#include <sched.h>

enum { TEST_PAGES = 2000 };
static int s_start, s_done, s_reader_observed_progress;

static void *allocate_pages(void *unused)
{
    uint32_t page;
    (void)unused;
    while (!__atomic_load_n(&s_start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (page = 1; page <= TEST_PAGES; page++) {
        /* There is only one writer in this test, so no allocator lock is
         * needed; the reader deliberately uses the public lock-free getter. */
        contig_take(page, 1);
        if (page == TEST_PAGES / 2)
            while (!__atomic_load_n(&s_reader_observed_progress, __ATOMIC_ACQUIRE))
                sched_yield();
        if (!(page & 15))
            sched_yield();
    }
    __atomic_store_n(&s_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    pthread_t writer;
    uint32_t last = xbox_ContiguousAllocatedBytes();
    int failed = last != CONTIG_PAGE;

    if (pthread_create(&writer, NULL, allocate_pages, NULL) != 0)
        return 1;
    __atomic_store_n(&s_start, 1, __ATOMIC_RELEASE);
    do {
        uint32_t current = xbox_ContiguousAllocatedBytes();
        uint32_t page = current / CONTIG_PAGE - 1;
        if (current < last || current > (TEST_PAGES + 1) * CONTIG_PAGE ||
                (page && bw_shared_u8_load(&g_contig_used[page]) != 1))
            failed = 1;
        if (current >= (TEST_PAGES / 2 + 1) * CONTIG_PAGE)
            __atomic_store_n(&s_reader_observed_progress, 1, __ATOMIC_RELEASE);
        last = current;
    } while (!__atomic_load_n(&s_done, __ATOMIC_ACQUIRE));
    if (pthread_join(writer, NULL) != 0)
        return 1;
    if (failed || xbox_ContiguousAllocatedBytes() != (TEST_PAGES + 1) * CONTIG_PAGE) {
        fprintf(stderr, "contiguous high-water publication failed\n");
        return 1;
    }
    return 0;
}
