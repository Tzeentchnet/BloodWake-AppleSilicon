#include "../../src/toolkit_patches/kernel/kernel_memory.c"

#include <pthread.h>
#include <sched.h>

enum { ITERATIONS = 100000 };
static int s_start, s_finished;

LONG InterlockedIncrement(volatile LONG *value)
{
    return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static void *update_translations(void *arg)
{
    uint8_t kind = (uint8_t)(uintptr_t)arg;
    while (!__atomic_load_n(&s_start, __ATOMIC_ACQUIRE))
        sched_yield();
    for (unsigned i = 0; i < ITERATIONS; i++)
        bw_note_translation(4096, kind, 4096);
    __atomic_add_fetch(&s_finished, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    pthread_t writers[2];
    const volatile uint8_t *last = bw_PhysLastTranslationMap();

    if (unsetenv("BW_PHYS_TRACE") != 0)
        return 1;
    if (pthread_create(&writers[0], NULL, update_translations, (void *)(uintptr_t)1) ||
            pthread_create(&writers[1], NULL, update_translations, (void *)(uintptr_t)2))
        return 1;
    __atomic_store_n(&s_start, 1, __ATOMIC_RELEASE);
    while (__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE) != 2) {
        if (bw_shared_u8_load(&last[1]) > 2)
            return 1;
    }
    return pthread_join(writers[0], NULL) || pthread_join(writers[1], NULL);
}
