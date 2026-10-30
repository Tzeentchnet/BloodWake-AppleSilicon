/* Manual counterexample, not a shutdown-safety test: success means the
 * current unsafe behavior was reproduced. The producer and guest ISR are
 * synthetic; the carried OHCI stop and pinned IRQ gate are real. No MMIO
 * pages are protected or enabled. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int probe_join(pthread_t thread, void **result);
#define pthread_join probe_join
#include "../../src/toolkit_patches/usb/ohci.c"
#undef pthread_join

RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
RECOMP_TLS uint32_t g_ebp, g_seh_ebp, g_eflags, g_fs_base;
RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_stack_base = 0x10000;
uint32_t g_xbox_stack_size = XBOX_WORKER_STACK_SIZE;

ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
int xbox_worker_stack_alloc(void) { return 0; }
void xbox_worker_stack_free(int slot) { (void)slot; }
int xbox_DpcPending(void) { return 0; }
int xbox_DpcDrainHere(void) { return 0; }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int holder_ready;
    int holder_release;
    int posted;
    int post_done;
    int post_count;
    int callback_entered;
    int callback_release;
    int callback_count;
    int stop_returned;
    int callback_after_stop;
    int block_callback;
} Probe;

static Probe *s_probe;
static int s_mock_join;
static pthread_mutex_t s_join_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_join_cv = PTHREAD_COND_INITIALIZER;
static int s_join_calls;
static int s_first_stop_returned;
static _Thread_local int s_join_rank;

static int probe_join(pthread_t thread, void **result)
{
    if (!s_mock_join)
        return pthread_join(thread, result);
    /* Control the two join calls without invoking undefined behavior by
     * actually joining the same pthread concurrently. */
    pthread_mutex_lock(&s_join_lock);
    s_join_rank = ++s_join_calls;
    pthread_cond_broadcast(&s_join_cv);
    if (s_join_rank == 1) {
        while (s_join_calls < 2)
            pthread_cond_wait(&s_join_cv, &s_join_lock);
    } else {
        while (!s_first_stop_returned)
            pthread_cond_wait(&s_join_cv, &s_join_lock);
    }
    pthread_mutex_unlock(&s_join_lock);
    return 0;
}

static void callback(void)
{
    Probe *p = s_probe;
    pthread_mutex_lock(&p->lock);
    ++p->callback_count;
    p->callback_entered = 1;
    pthread_cond_broadcast(&p->cv);
    while (p->block_callback && !p->callback_release)
        pthread_cond_wait(&p->cv, &p->lock);
    if (p->stop_returned)
        ++p->callback_after_stop;
    pthread_mutex_unlock(&p->lock);
}

static void *holder_main(void *opaque)
{
    Probe *p = opaque;
    xbox_DispatchGateEnter();
    pthread_mutex_lock(&p->lock);
    p->holder_ready = 1;
    pthread_cond_broadcast(&p->cv);
    while (!p->holder_release)
        pthread_cond_wait(&p->cv, &p->lock);
    pthread_mutex_unlock(&p->lock);
    xbox_DispatchGateLeave();
    return NULL;
}

static void *producer_main(void *opaque)
{
    Probe *p = opaque;
    struct timespec delay = {0, 1000000};
    int posted = 1;
    for (int i = 0; i < p->post_count; ++i)
        posted &= xbox_IrqPost(XBOX_IRQ_OHCI) == XBOX_IRQ_POSTED;
    pthread_mutex_lock(&p->lock);
    p->posted = posted;
    p->post_done = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    while (!InterlockedCompareExchange(&s_ohci_stop, 0, 0))
        nanosleep(&delay, NULL);
    return NULL;
}

static int device_case(int posts, int block_callback)
{
    Probe p = {.lock = PTHREAD_MUTEX_INITIALIZER,
               .cv = PTHREAD_COND_INITIALIZER,
               .post_count = posts,
               .block_callback = block_callback};
    pthread_t holder;
    XboxIrqStats stats;
    int ok;

    s_probe = &p;
    s_ohci_trapped = 0;
    s_ohci_stop = 0;
    s_enabled = 1;
    s_init_done = 1;
    xbox_IrqStatsGet(&stats, 1);
    xbox_IrqSetHandler(XBOX_IRQ_OHCI, callback);
    if (pthread_create(&holder, NULL, holder_main, &p) != 0) {
        pthread_cond_destroy(&p.cv);
        pthread_mutex_destroy(&p.lock);
        s_probe = NULL;
        return 0;
    }
    pthread_mutex_lock(&p.lock);
    while (!p.holder_ready)
        pthread_cond_wait(&p.cv, &p.lock);
    pthread_mutex_unlock(&p.lock);
    if (pthread_create(&s_ohci_thread, NULL, producer_main, &p) != 0) {
        pthread_mutex_lock(&p.lock);
        p.holder_release = 1;
        pthread_cond_broadcast(&p.cv);
        pthread_mutex_unlock(&p.lock);
        pthread_join(holder, NULL);
        pthread_cond_destroy(&p.cv);
        pthread_mutex_destroy(&p.lock);
        s_probe = NULL;
        return 0;
    }
    s_ohci_thread_started = 1;
    pthread_mutex_lock(&p.lock);
    while (!p.post_done)
        pthread_cond_wait(&p.cv, &p.lock);
    if (block_callback && p.posted) {
        p.holder_release = 1;
        pthread_cond_broadcast(&p.cv);
        while (!p.callback_entered)
            pthread_cond_wait(&p.cv, &p.lock);
    }
    pthread_mutex_unlock(&p.lock);

    ok = bw_xbox_ohci_stop() == 0;
    pthread_mutex_lock(&p.lock);
    p.stop_returned = 1;
    p.holder_release = 1;
    p.callback_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    ok &= pthread_join(holder, NULL) == 0;
    xbox_IrqStatsGet(&stats, 0);
    ok &= p.posted && p.callback_count == 1 && p.callback_after_stop == 1;
    ok &= stats.posted[XBOX_IRQ_OHCI] == 1;
    pthread_cond_destroy(&p.cv);
    pthread_mutex_destroy(&p.lock);
    s_probe = NULL;
    return ok;
}

static void *concurrent_stop(void *unused)
{
    (void)unused;
    int rc = bw_xbox_ohci_stop();
    if (s_join_rank == 1) {
        pthread_mutex_lock(&s_join_lock);
        s_first_stop_returned = 1;
        pthread_cond_broadcast(&s_join_cv);
        pthread_mutex_unlock(&s_join_lock);
    }
    return (void *)(uintptr_t)(rc != 0);
}

static int concurrent_case(void)
{
    pthread_t a, b;
    void *ar, *br;
    s_mock_join = 1;
    s_join_calls = s_first_stop_returned = 0;
    s_ohci_thread = pthread_self();
    s_ohci_thread_started = 1;
    s_ohci_stop = 0;
    s_ohci_trapped = 0;
    s_enabled = s_init_done = 1;
    if (pthread_create(&a, NULL, concurrent_stop, NULL) != 0) {
        s_mock_join = 0;
        return 0;
    }
    if (pthread_create(&b, NULL, concurrent_stop, NULL) != 0) {
        pthread_mutex_lock(&s_join_lock);
        while (s_join_calls < 1)
            pthread_cond_wait(&s_join_cv, &s_join_lock);
        s_join_calls = 2;
        pthread_cond_broadcast(&s_join_cv);
        pthread_mutex_unlock(&s_join_lock);
        pthread_join(a, NULL);
        s_mock_join = 0;
        return 0;
    }
    if (pthread_join(a, &ar) != 0 || pthread_join(b, &br) != 0)
        return 0;
    s_mock_join = 0;
    return !ar && !br && s_join_calls == 2;
}

int main(void)
{
    void *memory = calloc(1, XBOX_WORKER_STACK_SIZE + 32);
    int queued, executing, coalesced, concurrent;
    if (!memory)
        return 1;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory - g_xbox_stack_base;
    setenv("RECOMP_DISPATCH_GATE", "1", 1);
    setenv("RECOMP_IRQ_SAFE_POINTS", "1", 1);
    unsetenv("RECOMP_IRQ_SAFE_FORCE");
    alarm(20);
    queued = device_case(1, 0);
    executing = device_case(1, 1);
    coalesced = device_case(3, 0);
    concurrent = concurrent_case();
    alarm(0);
    puts("Known-unsafe counterexamples (reproduced does not mean safe):");
    printf("queued post-stop callback: %s\n", queued ? "reproduced" : "not reproduced");
    printf("executing callback survives stop: %s\n", executing ? "reproduced" : "not reproduced");
    printf("coalesced posts survive stop: %s\n", coalesced ? "reproduced" : "not reproduced");
    printf("concurrent double join attempt: %s\n", concurrent ? "reproduced" : "not reproduced");
    free(memory);
    return queued && executing && coalesced && concurrent ? 0 : 1;
}
