#include "../../src/toolkit_patches/kernel/xbox_memory_layout.c"

#include <sched.h>

enum { WORKERS = 8 };
static int s_ready, s_start, s_failures, s_expected;

static void *read_reclaim_setting(void *unused)
{
    (void)unused;
    __atomic_add_fetch(&s_ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&s_start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (unsigned i = 0; i < 1000; i++)
        if (xbox_HeapReclaimEnabled() != s_expected)
            __atomic_add_fetch(&s_failures, 1, __ATOMIC_RELAXED);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t workers[WORKERS];
    if (argc != 2)
        return 1;
    if (strcmp(argv[1], "unset") == 0) {
        s_expected = 0;
        if (unsetenv("RECOMP_HEAP_RECLAIM") != 0)
            return 1;
    } else {
        s_expected = 1;
        if (setenv("RECOMP_HEAP_RECLAIM", argv[1], 1) != 0)
            return 1;
    }
    for (unsigned i = 0; i < WORKERS; i++)
        if (pthread_create(&workers[i], NULL, read_reclaim_setting, NULL) != 0)
            return 1;
    while (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) != WORKERS)
        sched_yield();
    __atomic_store_n(&s_start, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < WORKERS; i++)
        if (pthread_join(workers[i], NULL) != 0)
            return 1;
    return __atomic_load_n(&s_failures, __ATOMIC_RELAXED) != 0;
}
