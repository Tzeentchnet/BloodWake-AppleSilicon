#ifndef BW_HOOK_PLATFORM_H
#define BW_HOOK_PLATFORM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Match the toolkit's RECOMP_TLS declaration without pulling generated macros
 * into hook files. */
#if defined(_MSC_VER)
#define BW_HOOK_TLS __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define BW_HOOK_TLS __thread
#else
#define BW_HOOK_TLS _Thread_local
#endif

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>

#define BW_HOOK_PATH_MAX MAX_PATH
#define BW_HOOK_PATH_SEP "\\"
#define BW_HOOK_GAME_DIR "game\\disc"
#define bw_hook_stricmp _stricmp
#define bw_hook_snprintf _snprintf

static inline uint64_t bw_hook_counter(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (uint64_t)n.QuadPart;
}

static inline uint64_t bw_hook_counter_frequency(void)
{
    LARGE_INTEGER n;
    QueryPerformanceFrequency(&n);
    return (uint64_t)n.QuadPart;
}

static inline uint64_t bw_hook_milliseconds(void) { return GetTickCount64(); }
static inline unsigned long bw_hook_process_id(void) { return (unsigned long)GetCurrentProcessId(); }
static inline void bw_hook_yield(void) { SwitchToThread(); }
static inline void bw_hook_pause(void) { _mm_pause(); }
static inline void bw_hook_store_fence(void) { _mm_sfence(); }

static inline long bw_hook_increment(volatile long *p) { return InterlockedIncrement(p); }
static inline long bw_hook_exchange(volatile long *p, long v) { return InterlockedExchange(p, v); }
static inline long bw_hook_exchange_add(volatile long *p, long v) { return InterlockedExchangeAdd(p, v); }
static inline long bw_hook_compare_exchange(volatile long *p, long v, long expected)
{
    return InterlockedCompareExchange(p, v, expected);
}

static inline int bw_hook_file_size(const char *path, uint64_t *size)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
        return 0;
    *size = info.nFileSizeLow;
    return 1;
}

#else
#include <limits.h>
#include <sched.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BW_HOOK_PATH_MAX PATH_MAX
#define BW_HOOK_PATH_SEP "/"
#define BW_HOOK_GAME_DIR "game/disc"
#define bw_hook_stricmp strcasecmp
#define bw_hook_snprintf snprintf

static inline uint64_t bw_hook_counter(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t bw_hook_counter_frequency(void) { return 1000000000ull; }
static inline uint64_t bw_hook_milliseconds(void) { return bw_hook_counter() / 1000000ull; }
static inline unsigned long bw_hook_process_id(void) { return (unsigned long)getpid(); }
static inline void bw_hook_yield(void) { (void)sched_yield(); }
static inline void bw_hook_pause(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("yield" ::: "memory");
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");
#else
    (void)sched_yield();
#endif
}
static inline void bw_hook_store_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

static inline long bw_hook_increment(volatile long *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline long bw_hook_exchange(volatile long *p, long v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline long bw_hook_exchange_add(volatile long *p, long v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
static inline long bw_hook_compare_exchange(volatile long *p, long v, long expected)
{
    __atomic_compare_exchange_n(p, &expected, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline int bw_hook_file_size(const char *path, uint64_t *size)
{
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 0)
        return 0;
    *size = (uint64_t)info.st_size;
    return 1;
}
#endif

/* Required generated functions must never silently become no-ops when a
 * build uses address-only names rather than the optional descriptive names. */
static inline void bw_hook_call_guest(uint32_t va)
{
    extern void (*recomp_lookup(uint32_t))(void);
    void (*fn)(void) = recomp_lookup(va);
    if (!fn) {
        fprintf(stderr, "[BW] required translated function 0x%08X is missing\n", va);
        abort();
    }
    fn();
}

#endif
