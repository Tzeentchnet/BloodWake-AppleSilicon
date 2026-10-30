#ifndef BW_SHARED_U8_H
#define BW_SHARED_U8_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Legacy map getters expose pointers; all POSIX accesses still use these
 * helpers. Atomic bytes do not pin the mapping they describe. */
#if !defined(_WIN32)
_Static_assert(__atomic_always_lock_free(sizeof(uint8_t), 0),
               "shared page-map byte accesses must be lock-free");
#endif

static inline uint8_t bw_shared_u8_load(const volatile uint8_t *p)
{
#if defined(_WIN32)
    return *p;
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}

static inline void bw_shared_u8_store(volatile uint8_t *p, uint8_t value)
{
#if defined(_WIN32)
    *p = value;
#else
    __atomic_store_n(p, value, __ATOMIC_RELEASE);
#endif
}

static inline void bw_shared_u8_fill(uint8_t *p, uint8_t value, size_t count)
{
#if defined(_WIN32)
    memset(p, value, count);
#else
    for (size_t i = 0; i < count; i++)
        bw_shared_u8_store(&p[i], value);
#endif
}

#endif
