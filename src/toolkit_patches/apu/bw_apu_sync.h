/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef BW_APU_SYNC_H
#define BW_APU_SYNC_H

#include <stdlib.h>

typedef void *(*BwApuThreadFunc)(void *);

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef SRWLOCK BwApuMutex;
typedef CONDITION_VARIABLE BwApuCond;
typedef struct { HANDLE handle; } BwApuThread;
#define BW_APU_MUTEX_INITIALIZER SRWLOCK_INIT
#define BW_APU_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline void bw_apu_mutex_lock(BwApuMutex *m) { AcquireSRWLockExclusive(m); }
static inline void bw_apu_mutex_unlock(BwApuMutex *m) { ReleaseSRWLockExclusive(m); }
static inline void bw_apu_cond_wait(BwApuCond *c, BwApuMutex *m)
{
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
static inline void bw_apu_cond_broadcast(BwApuCond *c) { WakeAllConditionVariable(c); }

typedef struct {
    BwApuThreadFunc func;
    void *arg;
} BwApuThreadStart;

static DWORD WINAPI bw_apu_thread_entry(LPVOID opaque)
{
    BwApuThreadStart *start = (BwApuThreadStart *)opaque;
    BwApuThreadFunc func = start->func;
    void *arg = start->arg;
    free(start);
    func(arg);
    return 0;
}

static inline int bw_apu_thread_create(BwApuThread *thread, BwApuThreadFunc func, void *arg)
{
    BwApuThreadStart *start;
    if (!thread || thread->handle || !func)
        return 0;
    start = (BwApuThreadStart *)malloc(sizeof(*start));
    if (!start)
        return 0;
    start->func = func;
    start->arg = arg;
    thread->handle = CreateThread(NULL, 0, bw_apu_thread_entry, start, 0, NULL);
    if (!thread->handle) {
        free(start);
        return 0;
    }
    return 1;
}

static inline void bw_apu_thread_join(BwApuThread *thread)
{
    if (thread->handle) {
        WaitForSingleObject(thread->handle, INFINITE);
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }
}

/* 0 normal, 1 above normal, 2 highest, 3 time critical. */
static inline int bw_apu_set_priority(int priority)
{
    static const int levels[] = {
        THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL,
        THREAD_PRIORITY_HIGHEST, THREAD_PRIORITY_TIME_CRITICAL
    };
    return SetThreadPriority(GetCurrentThread(), levels[priority]) != 0;
}

#else

#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

typedef pthread_mutex_t BwApuMutex;
typedef pthread_cond_t BwApuCond;
typedef struct { pthread_t handle; int started; } BwApuThread;
#define BW_APU_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER
#define BW_APU_COND_INITIALIZER PTHREAD_COND_INITIALIZER

static inline void bw_apu_mutex_lock(BwApuMutex *m) { pthread_mutex_lock(m); }
static inline void bw_apu_mutex_unlock(BwApuMutex *m) { pthread_mutex_unlock(m); }
static inline void bw_apu_cond_wait(BwApuCond *c, BwApuMutex *m)
{
    pthread_cond_wait(c, m);
}
static inline void bw_apu_cond_broadcast(BwApuCond *c) { pthread_cond_broadcast(c); }

static inline int bw_apu_thread_create(BwApuThread *thread, BwApuThreadFunc func, void *arg)
{
    if (!thread || thread->started || !func)
        return 0;
    if (pthread_create(&thread->handle, NULL, func, arg) != 0)
        return 0;
    thread->started = 1;
    return 1;
}

static inline void bw_apu_thread_join(BwApuThread *thread)
{
    if (thread->started) {
        pthread_join(thread->handle, NULL);
        thread->started = 0;
    }
}

static inline int bw_apu_set_priority(int priority)
{
    if (!priority)
        return 1;
#if defined(__APPLE__)
    return pthread_set_qos_class_self_np(priority == 1 ? QOS_CLASS_USER_INITIATED
                                                        : QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
    (void)priority;
    return 0;
#endif
}

#endif
#endif
