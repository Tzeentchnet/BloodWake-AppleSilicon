/* bw_wait.c - a short, precise, non-spinning wait for guest busy-loops.
 *
 * recomp_manual.c cannot include windows.h (the generated code's register
 * macros), so its hooks call this. 200 us on a high-resolution waitable timer
 * (Windows 10 1803+); without one, a yield. POSIX uses the toolkit's
 * nanosecond sleep and wakeable condition variables. Used by the KickOff
 * replacement (sub_0012C9F0), where Sleep(1) overshot the vblank. */
#ifdef _WIN32
#include <windows.h>
#else
#include "platform/win32_compat.h"
#include "platform/host_time.h"
#include <pthread.h>
#include <sched.h>
#include <time.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "bw_wait.h"

extern volatile LONG g_bw_kickoffs;   /* KickOffs, for BW_KICKOFF_STATS */

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void bw_short_wait(void)
{
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    LARGE_INTEGER due;

    if (!timer && !tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (!timer) {
        SwitchToThread();
        return;
    }
    due.QuadPart = -2000;                    /* 200 us, relative, in 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, 5);
    else
        SwitchToThread();
}

/* The NV2A thread's idle wait. The toolkit's loop ends each pass with
 * Sleep(0), so it held a whole core even between frames. Here: spin as before
 * while work is recent (BW_ACK_IDLE_US, default 300 us since the last
 * segment), then nap on a high-resolution timer (BW_ACK_NAP_US, default
 * 200 us) or until the title's next KickOff sets the wake event, whichever
 * comes first. BW_ACK_NAP=0 keeps the spin. */
static HANDLE s_ack_wake;

void bw_ack_kick(void)
{
    InterlockedIncrement(&g_bw_kickoffs);
    if (s_ack_wake)
        SetEvent(s_ack_wake);
}

void bw_ack_idle(int worked)
{
    static int mode = -1;
    static HANDLE timer;
    static LONGLONG idle_ticks, nap_100ns, last_work;
    LARGE_INTEGER now, due;
    HANDLE h[2];

    if (mode < 0) {
        const char *e = getenv("BW_ACK_NAP");
        LARGE_INTEGER f;
        long idle_us = 300, nap_us = 200;
        mode = !(e && e[0] == '0');
        if ((e = getenv("BW_ACK_IDLE_US")) != NULL) idle_us = atol(e);
        if ((e = getenv("BW_ACK_NAP_US")) != NULL) nap_us = atol(e);
        QueryPerformanceFrequency(&f);
        idle_ticks = f.QuadPart * idle_us / 1000000;
        nap_100ns = nap_us * 10;
        if (mode) {
            s_ack_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
            timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
            if (!s_ack_wake || !timer)
                mode = 0;
        }
    }
    if (!mode) {
        Sleep(0);
        return;
    }
    QueryPerformanceCounter(&now);
    if (worked || !last_work || now.QuadPart - last_work < idle_ticks) {
        if (worked || !last_work)
            last_work = now.QuadPart;
        Sleep(0);
        return;
    }
    due.QuadPart = -nap_100ns;
    if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        Sleep(0);
        return;
    }
    h[0] = s_ack_wake;
    h[1] = timer;
    WaitForMultipleObjects(2, h, FALSE, 5);
}

/* The KickOff hand-off (D7, 2026-10-27). The KickOff replacement in
 * recomp_manual.c waits for the NV2A thread to acknowledge its PFB flush.
 * It used to poll with bw_short_wait, so every KickOff lost up to a whole
 * timer period after the flush was already acknowledged: in Remastered's
 * high frame rate the game thread sat 49% in that wait while the NV2A thread
 * idled 56% (profile with callers, run 20261027-023300-r6-gpu-d7hfprof).
 * Now the ack thread signals an event as it clears the flush bit
 * (bw_flush_acked, carried xbox_memory_layout.c), and the wait wakes on that
 * event or on the same ~200 us timer as before, whichever comes first, so a
 * posted interrupt still gets a safe point at least that often. Same
 * register protocol, same waits; only the wake-up is prompt.
 * BW_KICKOFF_EVENT=0 restores the timer-only poll. */
static HANDLE s_flush_ack;
static int s_flush_mode = -1;

static int flush_mode(void)
{
    if (s_flush_mode < 0) {
        const char *e = getenv("BW_KICKOFF_EVENT");
        int m = !(e && e[0] == '0');
        if (m && !s_flush_ack)
            s_flush_ack = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
        s_flush_mode = m && s_flush_ack;
    }
    return s_flush_mode;
}

void bw_flush_acked(void)
{
    if (s_flush_mode > 0)
        SetEvent(s_flush_ack);
}

void bw_flush_wait(void)
{
    static __declspec(thread) HANDLE timer;
    static __declspec(thread) int tried;
    LARGE_INTEGER due;
    HANDLE h[2];

    if (!flush_mode()) {
        bw_short_wait();
        return;
    }
    if (!timer && !tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    due.QuadPart = -2000;                    /* 200 us, as bw_short_wait */
    if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
        WaitForSingleObject(s_flush_ack, 1);
        return;
    }
    h[0] = s_flush_ack;
    h[1] = timer;
    {
        /* BW_KICKOFF_STATS=1: once a second, the KickOffs, how many waited,
         * and the time spent waiting (game thread). */
        static int stats = -1;
        static LARGE_INTEGER f, t_last;
        static double waited_ms;
        static unsigned waits, kicks_last;
        LARGE_INTEGER t0, t1;
        if (stats < 0) {
            const char *e = getenv("BW_KICKOFF_STATS");
            stats = e && e[0] == '1';
            QueryPerformanceFrequency(&f);
        }
        if (stats)
            QueryPerformanceCounter(&t0);
        WaitForMultipleObjects(2, h, FALSE, 5);
        if (stats) {
            QueryPerformanceCounter(&t1);
            waited_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
            waits++;
            if (!t_last.QuadPart)
                t_last = t1;
            if (t1.QuadPart - t_last.QuadPart >= f.QuadPart) {
                unsigned k = (unsigned)g_bw_kickoffs;
                fprintf(stderr, "[BW] kickoff: %u kickoffs, %u wait turns, %.1f ms waiting\n",
                        k - kicks_last, waits, waited_ms);
                kicks_last = k;
                waits = 0;
                waited_ms = 0.0;
                t_last = t1;
            }
        }
    }
}
#else
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int pending;
} BwWake;

#define BW_WAKE_INIT { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }
static BwWake s_ack_wake = BW_WAKE_INIT;
static BwWake s_flush_ack = BW_WAKE_INIT;
static pthread_once_t s_ack_once = PTHREAD_ONCE_INIT;
static pthread_once_t s_flush_once = PTHREAD_ONCE_INIT;
static int s_ack_mode, s_flush_mode;
static uint64_t s_ack_idle_ns, s_ack_nap_ns, s_last_work;

static void wake_signal(BwWake *wake)
{
    pthread_mutex_lock(&wake->mutex);
    wake->pending = 1;
    pthread_cond_signal(&wake->cond);
    pthread_mutex_unlock(&wake->mutex);
}

static void wake_wait(BwWake *wake, uint64_t ns)
{
    pthread_mutex_lock(&wake->mutex);
    if (!wake->pending && ns) {
#ifdef __APPLE__
        struct timespec relative = { (time_t)(ns / 1000000000u),
                                     (long)(ns % 1000000000u) };
        pthread_cond_timedwait_relative_np(&wake->cond, &wake->mutex, &relative);
#else
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += (time_t)(ns / 1000000000u);
        until.tv_nsec += (long)(ns % 1000000000u);
        if (until.tv_nsec >= 1000000000L) {
            until.tv_nsec -= 1000000000L;
            until.tv_sec++;
        }
        pthread_cond_timedwait(&wake->cond, &wake->mutex, &until);
#endif
    }
    wake->pending = 0;
    pthread_mutex_unlock(&wake->mutex);
}

void bw_short_wait(void)
{
    xbox_HostSleepNs(200000);
}

static void ack_init(void)
{
    const char *e = getenv("BW_ACK_NAP");
    long idle_us = 300, nap_us = 200;
    s_ack_mode = !(e && e[0] == '0');
    if ((e = getenv("BW_ACK_IDLE_US")) != NULL)
        idle_us = atol(e);
    if ((e = getenv("BW_ACK_NAP_US")) != NULL)
        nap_us = atol(e);
    if (idle_us < 0) idle_us = 0;
    if (nap_us < 0) nap_us = 0;
    if (idle_us > 10000000) idle_us = 10000000;
    if (nap_us > 10000000) nap_us = 10000000;
    s_ack_idle_ns = (uint64_t)idle_us * 1000u;
    s_ack_nap_ns = (uint64_t)nap_us * 1000u;
}

void bw_ack_kick(void)
{
    InterlockedIncrement(&g_bw_kickoffs);
    wake_signal(&s_ack_wake);
}

void bw_ack_idle(int worked)
{
    uint64_t now;
    pthread_once(&s_ack_once, ack_init);
    if (!s_ack_mode) {
        sched_yield();
        return;
    }
    now = xbox_HostNowNs();
    if (worked || !s_last_work || now - s_last_work < s_ack_idle_ns) {
        if (worked || !s_last_work)
            s_last_work = now;
        sched_yield();
        return;
    }
    wake_wait(&s_ack_wake, s_ack_nap_ns);
}

static void flush_init(void)
{
    const char *e = getenv("BW_KICKOFF_EVENT");
    s_flush_mode = !(e && e[0] == '0');
}

void bw_flush_acked(void)
{
    wake_signal(&s_flush_ack);
}

void bw_flush_wait(void)
{
    static int stats = -1;
    static double waited_ms, last_ms;
    static unsigned waits, kicks_last;
    double before = 0, after;
    pthread_once(&s_flush_once, flush_init);
    if (!s_flush_mode) {
        bw_short_wait();
        return;
    }
    if (stats < 0) {
        const char *e = getenv("BW_KICKOFF_STATS");
        stats = e && e[0] == '1';
    }
    if (stats)
        before = bw_now_ms();
    wake_wait(&s_flush_ack, 200000);
    if (!stats)
        return;
    after = bw_now_ms();
    waited_ms += after - before;
    waits++;
    if (!last_ms)
        last_ms = after;
    if (after - last_ms >= 1000.0) {
        unsigned k = (unsigned)InterlockedCompareExchange(&g_bw_kickoffs, 0, 0);
        fprintf(stderr, "[BW] kickoff: %u kickoffs, %u wait turns, %.1f ms waiting\n",
                k - kicks_last, waits, waited_ms);
        kicks_last = k;
        waits = 0;
        waited_ms = 0;
        last_ms = after;
    }
}
#endif

volatile LONG g_bw_kickoffs;

/* P1: the walk one segment at a time (bw_wait.h). The NV2A thread used to
 * walk everything between the last PUT it saw and the register's PUT in one
 * pass and only then move GET, so a KickOff waiting for GET (more than
 * g_bw_kickoff_async segments outstanding) waited for the whole batch, and
 * the NV2A thread then idled while the game produced the next segment: in
 * Remastered both threads waited about a third of the time (profiles
 * -130336-p1vfprof3, -130419-p1vfprof15). Walking each recorded segment on
 * its own, with GET moved after each, releases the game after one segment.
 * The same words are walked in the same order, split at the PUTs the title
 * itself published. A held flip now holds right after its own segment, as
 * the console's FLIP_STALL does, instead of after whatever followed it in
 * the batch; that changes Original's rhythm, so this is on only with
 * Remastered's high frame rate (bw_hifps_boot) unless BW_PB_SEGWALK=1/0
 * says otherwise. Single producer (the game thread's KickOff), single
 * consumer (the NV2A thread). */
#define BW_PQ 64
static unsigned s_pq[BW_PQ];
static volatile LONG s_pq_head, s_pq_tail;
int g_bw_segwalk = -1;

static int segwalk_on(void)
{
#ifdef _WIN32
    if (g_bw_segwalk < 0) {
        const char *e = getenv("BW_PB_SEGWALK");
        g_bw_segwalk = e && e[0] == '1';
    }
    return g_bw_segwalk;
#else
    int mode = __atomic_load_n(&g_bw_segwalk, __ATOMIC_ACQUIRE);
    if (mode < 0) {
        const char *e = getenv("BW_PB_SEGWALK");
        int expected = -1;
        int desired = e && e[0] == '1';
        __atomic_compare_exchange_n(&g_bw_segwalk, &expected, desired, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        mode = __atomic_load_n(&g_bw_segwalk, __ATOMIC_ACQUIRE);
    }
    return mode;
#endif
}

void bw_segwalk_default(int on)
{
    const char *e = getenv("BW_PB_SEGWALK");
    int mode = e && *e ? e[0] == '1' : on;
#ifdef _WIN32
    g_bw_segwalk = mode;
#else
    __atomic_store_n(&g_bw_segwalk, mode, __ATOMIC_RELEASE);
#endif
    fprintf(stderr, "[BW] NV2A walk: %s (P1, BW_PB_SEGWALK)\n",
            mode ? "one KickOff segment at a time, GET after each" : "to the newest PUT");
}

void bw_put_queue_push(unsigned put)
{
    LONG h;
    if (!segwalk_on())
        return;
    h = InterlockedCompareExchange(&s_pq_head, 0, 0);
    if ((uint32_t)h - (uint32_t)InterlockedCompareExchange(&s_pq_tail, 0, 0) >= BW_PQ)
        return;                              /* full: the walk takes the register */
    s_pq[(uint32_t)h & (BW_PQ - 1)] = put;
    InterlockedExchange(&s_pq_head, (LONG)((uint32_t)h + 1u)); /* publish after the slot */
}

unsigned bw_put_queue_take(unsigned last_put, unsigned put_reg)
{
    if (!segwalk_on())
        return put_reg;
    for (;;) {
        LONG t = InterlockedCompareExchange(&s_pq_tail, 0, 0);
        unsigned q;
        if (t == InterlockedCompareExchange(&s_pq_head, 0, 0))
            break;
        q = s_pq[(uint32_t)t & (BW_PQ - 1)];
        /* inside (last_put, put_reg] in ring order; anything else is a PUT
         * already walked (the register was read first) or from before a
         * ring restart, and is dropped */
        int inside = put_reg >= last_put ? (q > last_put && q <= put_reg)
                                         : (q > last_put || q <= put_reg);
        InterlockedExchange(&s_pq_tail, (LONG)((uint32_t)t + 1u));
        if (inside)
            return q;
    }
    return put_reg;
}

double bw_now_ms(void)
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
