/* Counterexample, not a shutdown-safety test: success means a real queued or
 * executing DPC outlived real APU shutdown. Guest code and RAM are synthetic;
 * no device MMIO trap is enabled. */
#include "apu.h"
#include "apu_state.h"
#include "bw_edition.h"
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IRQ_OBJECT 0x1000u
#define ISR_ADDRESS 0x1234u
#define ISR_CONTEXT 0x5678u
#define DPC_OBJECT 0x2000u
#define DPC_ADDRESS 0x2345u
#define DPC_CONTEXT 0x6789u
#define DPC_ARG1 0x13579BDFu
#define DPC_ARG2 0x2468ACE0u
#define GUEST_SIZE (UINT64_C(1) << 32)
#define CASE_TIMEOUT_SECONDS 20
#define WAIT_TIMEOUT_SECONDS 5

extern char **environ;

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
uint32_t g_xbox_stack_base = 0x10000, g_xbox_stack_size = XBOX_WORKER_STACK_SIZE;
uint32_t g_xbox_image_lo, g_xbox_image_hi;
size_t g_xbox_total_ram = 64u * 1024u * 1024u, g_xbox_map_size;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
static uint8_t s_page_map[(64u * 1024u * 1024u) / 4096u];

ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
const volatile uint8_t *xbox_ContiguousPageMap(void) { return s_page_map; }
const volatile uint8_t *xbox_PhysRamNameMap(void) { return s_page_map; }
const volatile uint8_t *bw_PhysLastTranslationMap(void) { return s_page_map; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
int xbox_worker_stack_alloc(void) { return 0; }
void xbox_worker_stack_free(int slot) { (void)slot; }
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return vector == 5 ? IRQ_OBJECT : 0;
}
uint32_t xbox_AllocThreadTib(void) { return 0; }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    uint8_t *guest;
    int holder_ready, holder_release;
    int isr_count, isr_queued, isr_args_ok;
    int block_dpc, dpc_entered, dpc_count, dpc_completed, dpc_release;
    int dpc_args_ok, dpc_started_after_stop, dpc_live_at_entry, dpc_null_at_entry;
    int dpc_completed_after_stop, dpc_null_at_finish;
    int stop_returned, drain_done, drained, drain_result;
} Probe;

typedef struct {
    int irq_posted, irq_posted_once, irq_on_holder_once, irq_no_fallback;
    int isr_count, isr_queued, isr_args_ok;
    int pending_after_isr, inserted_after_isr, entered_after_isr;
    int entered_before_stop, completed_before_stop, pending_before_stop, inserted_before_stop;
    int shutdown_returned, ram_cleared;
    int entered_after_stop, completed_after_stop, pending_after_stop, inserted_after_stop;
    int dpc_count, dpc_args_ok, dpc_started_after_stop, dpc_live_at_entry, dpc_null_at_entry;
    int dpc_completed_after_stop, dpc_null_at_finish;
    int drain_result, drained, pending_after_drain, inserted_after_drain, object_args_ok;
} Trace;

static Probe *s_probe;

static const char *trace_error(int executing, const Trace *t)
{
    if (!t->irq_posted || !t->irq_posted_once ||
        !t->irq_on_holder_once || !t->irq_no_fallback)
        return "IRQ was not posted once and delivered on the gate holder";
    if (t->isr_count != 1 || !t->isr_queued || !t->isr_args_ok)
        return "real APU ISR did not queue one DPC with the expected arguments";
    if (!t->pending_after_isr || !t->inserted_after_isr || t->entered_after_isr)
        return "DPC was not still queued and unentered after the ISR";
    if (t->entered_before_stop != executing || t->completed_before_stop ||
        t->pending_before_stop != !executing || t->inserted_before_stop != !executing)
        return "wrong queued/executing state before shutdown";
    if (!t->shutdown_returned || !t->ram_cleared)
        return "APU shutdown did not return and clear its RAM pointer";
    if (t->entered_after_stop != executing || t->completed_after_stop ||
        t->pending_after_stop != !executing || t->inserted_after_stop != !executing)
        return "wrong queued/executing state when shutdown returned";
    if (t->dpc_count != 1 || !t->dpc_args_ok ||
        t->dpc_started_after_stop != !executing ||
        t->dpc_live_at_entry != executing || t->dpc_null_at_entry != !executing ||
        !t->dpc_completed_after_stop || !t->dpc_null_at_finish)
        return "DPC did not have the expected entry and completion lifetime";
    if (t->drain_result != 0 || t->drained != 1 || t->pending_after_drain ||
        t->inserted_after_drain || !t->object_args_ok)
        return "DPC drain count, final queue state, or object arguments are wrong";
    return NULL;
}

static int oracle_self_test(void)
{
    for (int executing = 0; executing <= 1; ++executing) {
        Trace good = {
            .irq_posted = 1, .irq_posted_once = 1, .irq_on_holder_once = 1,
            .irq_no_fallback = 1, .isr_count = 1, .isr_queued = 1, .isr_args_ok = 1,
            .pending_after_isr = 1, .inserted_after_isr = 1,
            .entered_before_stop = executing, .pending_before_stop = !executing,
            .inserted_before_stop = !executing, .shutdown_returned = 1, .ram_cleared = 1,
            .entered_after_stop = executing, .pending_after_stop = !executing,
            .inserted_after_stop = !executing, .dpc_count = 1, .dpc_args_ok = 1,
            .dpc_started_after_stop = !executing, .dpc_live_at_entry = executing,
            .dpc_null_at_entry = !executing, .dpc_completed_after_stop = 1,
            .dpc_null_at_finish = 1, .drained = 1, .object_args_ok = 1
        };
        if (trace_error(executing, &good)) {
            fprintf(stderr, "DPC oracle rejected a valid %s trace\n",
                    executing ? "executing" : "queued");
            return 1;
        }
#define REJECT(field, value) do { \
            Trace bad = good; \
            bad.field = (value); \
            if (!trace_error(executing, &bad)) { \
                fprintf(stderr, "DPC oracle accepted invalid %s in %s trace\n", \
                        #field, executing ? "executing" : "queued"); \
                return 1; \
            } \
        } while (0)
        REJECT(irq_posted, 0);
        REJECT(irq_posted_once, 0);
        REJECT(irq_on_holder_once, 0);
        REJECT(irq_no_fallback, 0);
        REJECT(isr_count, 0);
        REJECT(isr_queued, 0);
        REJECT(isr_args_ok, 0);
        REJECT(pending_after_isr, 0);
        REJECT(inserted_after_isr, 0);
        REJECT(entered_after_isr, 1);
        REJECT(entered_before_stop, !executing);
        REJECT(completed_before_stop, 1);
        REJECT(pending_before_stop, executing);
        REJECT(inserted_before_stop, executing);
        REJECT(shutdown_returned, 0);
        REJECT(ram_cleared, 0);
        REJECT(entered_after_stop, !executing);
        REJECT(completed_after_stop, 1);
        REJECT(pending_after_stop, executing);
        REJECT(inserted_after_stop, executing);
        REJECT(dpc_count, 0);
        REJECT(dpc_args_ok, 0);
        REJECT(dpc_started_after_stop, executing);
        REJECT(dpc_live_at_entry, !executing);
        REJECT(dpc_null_at_entry, executing);
        REJECT(dpc_completed_after_stop, 0);
        REJECT(dpc_null_at_finish, 0);
        REJECT(drain_result, 1);
        REJECT(drained, 0);
        REJECT(pending_after_drain, 1);
        REJECT(inserted_after_drain, 1);
        REJECT(object_args_ok, 0);
#undef REJECT
    }
    puts("DPC counterexample oracle accepted both expected traces and rejected invalid traces");
    return 0;
}

static uint32_t guest_u32(const Probe *p, uint32_t address)
{
    return *(uint32_t *)(p->guest + address);
}

static void guest_isr(void)
{
    Probe *p = s_probe;
    int args_ok = guest_u32(p, g_esp) == 0xDEADBEEFu &&
                  guest_u32(p, g_esp + 4) == IRQ_OBJECT &&
                  guest_u32(p, g_esp + 8) == ISR_CONTEXT;
    int queued = xbox_DpcQueue(DPC_OBJECT, DPC_ARG1, DPC_ARG2);

    pthread_mutex_lock(&p->lock);
    ++p->isr_count;
    p->isr_queued = queued;
    p->isr_args_ok = args_ok;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    g_eax = 1;
    g_esp += 12;
}

static void guest_dpc(void)
{
    Probe *p = s_probe;

    pthread_mutex_lock(&p->lock);
    ++p->dpc_count;
    p->dpc_entered = 1;
    p->dpc_args_ok = guest_u32(p, g_esp) == 0 &&
                     guest_u32(p, g_esp + 4) == DPC_OBJECT &&
                     guest_u32(p, g_esp + 8) == DPC_CONTEXT &&
                     guest_u32(p, g_esp + 12) == DPC_ARG1 &&
                     guest_u32(p, g_esp + 16) == DPC_ARG2 &&
                     p->guest[DPC_OBJECT + 2] == 0;
    p->dpc_started_after_stop = p->stop_returned;
    p->dpc_live_at_entry = g_apu_ram_ptr == p->guest;
    p->dpc_null_at_entry = g_apu_ram_ptr == NULL;
    pthread_cond_broadcast(&p->cv);
    while (p->block_dpc && !p->dpc_release)
        pthread_cond_wait(&p->cv, &p->lock);
    p->dpc_completed_after_stop = p->stop_returned;
    p->dpc_null_at_finish = g_apu_ram_ptr == NULL;
    p->dpc_completed = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    g_esp += 20;
}

typedef void (*GuestFn)(void);
GuestFn recomp_lookup(uint32_t address)
{
    if (address == ISR_ADDRESS)
        return guest_isr;
    if (address == DPC_ADDRESS)
        return guest_dpc;
    return NULL;
}
GuestFn recomp_lookup_manual(uint32_t address) { (void)address; return NULL; }
void probe_register_real_apu_isr(void);

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

static void *drain_main(void *opaque)
{
    Probe *p = opaque;
    int ran = 0;
    int result;

    g_esp = XBOX_WORKER_STACK_TOP(0);
    result = xbox_DpcDrainTimer(&ran);
    pthread_mutex_lock(&p->lock);
    p->drained = ran;
    p->drain_result = result;
    p->drain_done = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static int wait_flag_locked(Probe *p, const int *flag, const int *finished,
                            const char *description)
{
    struct timespec deadline;
    int error;

    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        perror("clock_gettime");
        return 0;
    }
    deadline.tv_sec += WAIT_TIMEOUT_SECONDS;
    while (!*flag && (!finished || !*finished)) {
        error = pthread_cond_timedwait(&p->cv, &p->lock, &deadline);
        if (error != 0) {
            fprintf(stderr, "Waiting for %s failed: %s\n", description, strerror(error));
            return 0;
        }
    }
    if (!*flag)
        fprintf(stderr, "%s did not occur before the worker finished\n", description);
    return *flag;
}

static int child_case(int executing)
{
    Probe p = {.lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
               .block_dpc = executing};
    Trace t = {0};
    XboxIrqStats stats;
    uint8_t *guest = MAP_FAILED;
    MCPXAPUState *apu = NULL;
    pthread_t holder, drainer;
    int holder_started = 0, drainer_started = 0, workers_joined = 1;
    int waited, result = 1;
    const char *error;

#define CHECK(expr) do { \
        if (!(expr)) { \
            fprintf(stderr, "DPC %s: %s failed at line %d\n", \
                    executing ? "executing" : "queued", #expr, __LINE__); \
            goto cleanup; \
        } \
    } while (0)

    alarm(CASE_TIMEOUT_SECONDS);
    guest = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(guest != MAP_FAILED);
    CHECK(mprotect(guest, g_xbox_total_ram, PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(guest + 0x80000000u, g_xbox_total_ram,
                   PROT_READ | PROT_WRITE) == 0);
    CHECK(mprotect(guest + 0xFE830000u, 0x50000u, PROT_READ | PROT_WRITE) == 0);
    p.guest = guest;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    *(uint32_t *)(guest + IRQ_OBJECT) = ISR_ADDRESS;
    *(uint32_t *)(guest + IRQ_OBJECT + 4) = ISR_CONTEXT;
    *(uint32_t *)(guest + DPC_OBJECT + 12) = DPC_ADDRESS;
    *(uint32_t *)(guest + DPC_OBJECT + 16) = DPC_CONTEXT;
    apu = mcpx_apu_init_standalone(guest);
    CHECK(apu != NULL);
    s_probe = &p;
    probe_register_real_apu_isr();
    xbox_IrqStatsGet(&stats, 1);
    CHECK(pthread_create(&holder, NULL, holder_main, &p) == 0);
    holder_started = 1;
    pthread_mutex_lock(&p.lock);
    waited = wait_flag_locked(&p, &p.holder_ready, NULL, "the gate holder");
    pthread_mutex_unlock(&p.lock);
    CHECK(waited);
    t.irq_posted = xbox_IrqPost(XBOX_IRQ_APU) == XBOX_IRQ_POSTED;
    CHECK(t.irq_posted);
    pthread_mutex_lock(&p.lock);
    p.holder_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    CHECK(pthread_join(holder, NULL) == 0);
    holder_started = 0;
    xbox_IrqStatsGet(&stats, 0);
    t.irq_posted_once = stats.posted[XBOX_IRQ_APU] == 1;
    t.irq_on_holder_once = stats.on_holder[XBOX_IRQ_APU] == 1;
    t.irq_no_fallback = stats.second_try[XBOX_IRQ_APU] == 0 &&
                        stats.forced[XBOX_IRQ_APU] == 0 &&
                        stats.ungated[XBOX_IRQ_APU] == 0;
    t.isr_count = p.isr_count;
    t.isr_queued = p.isr_queued;
    t.isr_args_ok = p.isr_args_ok;
    t.pending_after_isr = !!xbox_DpcPending();
    t.inserted_after_isr = guest[DPC_OBJECT + 2] == 1;
    t.entered_after_isr = p.dpc_entered;
    CHECK(t.isr_count == 1 && t.isr_queued && t.isr_args_ok);
    CHECK(t.pending_after_isr && t.inserted_after_isr && !t.entered_after_isr);

    if (executing) {
        CHECK(pthread_create(&drainer, NULL, drain_main, &p) == 0);
        drainer_started = 1;
        pthread_mutex_lock(&p.lock);
        waited = wait_flag_locked(&p, &p.dpc_entered, &p.drain_done, "DPC entry");
        pthread_mutex_unlock(&p.lock);
        CHECK(waited);
    }

    pthread_mutex_lock(&p.lock);
    t.entered_before_stop = p.dpc_entered;
    t.completed_before_stop = p.dpc_completed;
    t.pending_before_stop = !!xbox_DpcPending();
    t.inserted_before_stop = guest[DPC_OBJECT + 2] != 0;
    pthread_mutex_unlock(&p.lock);
    CHECK(t.entered_before_stop == executing && !t.completed_before_stop);
    CHECK(t.pending_before_stop == !executing && t.inserted_before_stop == !executing);

    mcpx_apu_shutdown(apu);
    apu = NULL;
    t.shutdown_returned = 1;
    t.ram_cleared = g_apu_ram_ptr == NULL;
    pthread_mutex_lock(&p.lock);
    t.entered_after_stop = p.dpc_entered;
    t.completed_after_stop = p.dpc_completed;
    t.pending_after_stop = !!xbox_DpcPending();
    t.inserted_after_stop = guest[DPC_OBJECT + 2] != 0;
    p.stop_returned = 1;
    p.dpc_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);

    if (executing) {
        CHECK(pthread_join(drainer, NULL) == 0);
        drainer_started = 0;
    } else {
        drain_main(&p);
    }
    pthread_mutex_lock(&p.lock);
    t.dpc_count = p.dpc_count;
    t.dpc_args_ok = p.dpc_args_ok;
    t.dpc_started_after_stop = p.dpc_started_after_stop;
    t.dpc_live_at_entry = p.dpc_live_at_entry;
    t.dpc_null_at_entry = p.dpc_null_at_entry;
    t.dpc_completed_after_stop = p.dpc_completed_after_stop;
    t.dpc_null_at_finish = p.dpc_null_at_finish;
    t.drained = p.drained;
    t.drain_result = p.drain_result;
    pthread_mutex_unlock(&p.lock);
    t.pending_after_drain = !!xbox_DpcPending();
    t.inserted_after_drain = guest[DPC_OBJECT + 2] != 0;
    t.object_args_ok = guest_u32(&p, DPC_OBJECT + 20) == DPC_ARG1 &&
                       guest_u32(&p, DPC_OBJECT + 24) == DPC_ARG2;
    error = trace_error(executing, &t);
    if (error)
        fprintf(stderr, "DPC %s: %s\n", executing ? "executing" : "queued", error);
    else
        result = 0;

cleanup:
    pthread_mutex_lock(&p.lock);
    p.holder_release = 1;
    p.dpc_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    if (holder_started && pthread_join(holder, NULL) != 0)
        workers_joined = 0;
    if (drainer_started && pthread_join(drainer, NULL) != 0)
        workers_joined = 0;
    if (apu)
        mcpx_apu_shutdown(apu);
    if (workers_joined && guest != MAP_FAILED &&
        munmap(guest, (size_t)GUEST_SIZE) != 0) {
        perror("munmap");
        result = 1;
    }
    if (!workers_joined) {
        fprintf(stderr, "Could not join a DPC probe worker\n");
        result = 1;
    } else {
        pthread_cond_destroy(&p.cv);
        pthread_mutex_destroy(&p.lock);
    }
    alarm(0);
    return result;
#undef CHECK
}

static int run_case(const char *program, const char *option)
{
    char *const args[] = {(char *)program, (char *)option, NULL};
    struct timespec deadline, now, pause = {0, 10000000};
    int status, error;
    pid_t pid, done;

    error = posix_spawnp(&pid, program, NULL, NULL, args, environ);
    if (error != 0) {
        fprintf(stderr, "Could not spawn DPC %s: %s\n", option, strerror(error));
        return 0;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        perror("clock_gettime");
        goto terminate;
    }
    deadline.tv_sec += CASE_TIMEOUT_SECONDS + 5;
    for (;;) {
        done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) {
            error = errno;
            fprintf(stderr, "waitpid failed: %s\n", strerror(error));
            if (error == ECHILD)
                return 0;
            goto terminate;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("clock_gettime");
            goto terminate;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            fprintf(stderr, "DPC %s timed out; terminating child\n", option);
            goto terminate;
        }
        nanosleep(&pause, NULL);
    }

terminate:
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}

static int configure_environment(void)
{
    static const char *const unset_names[] = {
        "RECOMP_AUDIO_WAV", "BW_AUDIO_WAV6", "BW_APU_STEMS", "BW_APU_GPDUMP",
        "BW_APU_AUDIT", "RECOMP_APU_VOICE_DUMP", "RECOMP_IRQ_SAFE_FORCE",
        "RECOMP_USB", "RECOMP_AC97_READY"
    };
    for (size_t i = 0; i < sizeof unset_names / sizeof unset_names[0]; ++i) {
        if (unsetenv(unset_names[i]) != 0)
            return 0;
    }
    return setenv("RECOMP_DISPATCH_GATE", "1", 1) == 0 &&
           setenv("RECOMP_IRQ_SAFE_POINTS", "1", 1) == 0 &&
           setenv("RECOMP_DPC_ON_RAISE", "1", 1) == 0 &&
           setenv("SDL_AUDIODRIVER", "dummy", 1) == 0 &&
           setenv("BW_APU_DSP", "xemu", 1) == 0 &&
           setenv("BW_APU_DSP_THREAD", "1", 1) == 0 &&
           setenv("BW_APU_DSP_FALLBACK", "0", 1) == 0;
}

int main(int argc, char **argv)
{
    int queued, executing;

    if (argc == 2 && !strcmp(argv[1], "--oracle-self-test"))
        return oracle_self_test();
    if (!configure_environment()) {
        perror("DPC probe environment");
        return 2;
    }
    if (argc == 2 && (!strcmp(argv[1], "--queued") ||
                      !strcmp(argv[1], "--executing"))) {
        int rc = child_case(!strcmp(argv[1], "--executing"));
        printf("Known-unsafe %s DPC counterexample: %s (not a safety test)\n",
               argv[1] + 2, rc == 0 ? "reproduced" : "not reproduced");
        return rc;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--queued|--executing|--oracle-self-test]\n", argv[0]);
        return 2;
    }
    queued = run_case(argv[0], "--queued");
    executing = run_case(argv[0], "--executing");
    puts("Known-unsafe counterexamples (reproduced does not mean safe):");
    printf("ISR-queued DPC runs after APU shutdown: %s\n",
           queued ? "reproduced" : "not reproduced");
    printf("Executing DPC survives shutdown with empty queue: %s\n",
           executing ? "reproduced" : "not reproduced");
    return queued && executing ? 0 : 1;
}
