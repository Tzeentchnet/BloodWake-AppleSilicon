/* Manual counterexample, not a shutdown-safety test: success means the
 * current unsafe behavior was reproduced. Each case uses synthetic guest
 * RAM, a dummy audio device and the real APU ISR and shutdown code. The
 * expected post-shutdown memory fault is contained in a child process. */
#include "apu.h"
#include "apu_state.h"
#include "bw_edition.h"
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define IRQ_OBJECT 0x1000u
#define ISR_ADDRESS 0x1234u
#define GUEST_SIZE (UINT64_C(1) << 32)

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
int xbox_DpcPending(void) { return 0; }
int xbox_DpcDrainHere(void) { return 0; }
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return vector == 5 ? IRQ_OBJECT : 0;
}
uint32_t xbox_AllocThreadTib(void) { return 0; }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int holder_ready, holder_release;
    int callback_entered, callback_release, callback_after_stop;
    int stop_returned;
} Probe;
static Probe *s_probe;

static void guest_isr(void)
{
    Probe *p = s_probe;
    pthread_mutex_lock(&p->lock);
    p->callback_entered = 1;
    pthread_cond_broadcast(&p->cv);
    while (!p->callback_release)
        pthread_cond_wait(&p->cv, &p->lock);
    if (p->stop_returned && !g_apu_ram_ptr)
        p->callback_after_stop = 1;
    pthread_mutex_unlock(&p->lock);
}

typedef void (*GuestFn)(void);
GuestFn recomp_lookup(uint32_t address)
{
    return address == ISR_ADDRESS ? guest_isr : NULL;
}

void probe_register_real_apu_isr(void);
int probe_shutdown_started(MCPXAPUState *d);

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

static void expected_fault(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)context;
    _exit(info && (uintptr_t)info->si_addr == IRQ_OBJECT ? 42 : 43);
}

static int child_case(int posts, int executing)
{
    Probe p = {.lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};
    uint8_t *guest;
    MCPXAPUState *apu;
    pthread_t holder;
    XboxIrqStats stats;
    struct sigaction action = {0};
    int ok = 1;

    alarm(15);
    guest = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest == MAP_FAILED ||
        mprotect(guest, g_xbox_total_ram, PROT_READ | PROT_WRITE) ||
        mprotect(guest + 0x80000000u, g_xbox_total_ram, PROT_READ | PROT_WRITE) ||
        mprotect(guest + 0xFE830000u, 0x50000u, PROT_READ | PROT_WRITE))
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    *(uint32_t *)(guest + IRQ_OBJECT) = ISR_ADDRESS;
    setenv("RECOMP_DISPATCH_GATE", "1", 1);
    setenv("RECOMP_IRQ_SAFE_POINTS", "1", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BW_APU_DSP", "xemu", 1);
    setenv("BW_APU_DSP_FALLBACK", "0", 1);
    unsetenv("RECOMP_IRQ_SAFE_FORCE");
    apu = mcpx_apu_init_standalone(guest);
    if (!apu)
        return 3;
    s_probe = &p;
    probe_register_real_apu_isr();
    xbox_IrqStatsGet(&stats, 1);
    if (pthread_create(&holder, NULL, holder_main, &p) != 0)
        return 4;
    pthread_mutex_lock(&p.lock);
    while (!p.holder_ready)
        pthread_cond_wait(&p.cv, &p.lock);
    pthread_mutex_unlock(&p.lock);
    for (int i = 0; i < posts; ++i)
        ok &= xbox_IrqPost(XBOX_IRQ_APU) == XBOX_IRQ_POSTED;
    xbox_IrqStatsGet(&stats, 0);
    ok &= stats.posted[XBOX_IRQ_APU] == 1;
    if (executing) {
        pthread_mutex_lock(&p.lock);
        p.holder_release = 1;
        pthread_cond_broadcast(&p.cv);
        while (!p.callback_entered)
            pthread_cond_wait(&p.cv, &p.lock);
        pthread_mutex_unlock(&p.lock);
    }

    mcpx_apu_shutdown(apu);
    ok &= g_apu_ram_ptr == NULL;
    if (!executing) {
        action.sa_sigaction = expected_fault;
        action.sa_flags = SA_SIGINFO;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, NULL) || sigaction(SIGBUS, &action, NULL))
            return 5;
    }
    pthread_mutex_lock(&p.lock);
    p.stop_returned = 1;
    p.holder_release = 1;
    p.callback_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    pthread_join(holder, NULL);
    ok &= executing && p.callback_after_stop;
    munmap(guest, (size_t)GUEST_SIZE);
    return ok ? 0 : 6;
}

static int run_case(int posts, int executing)
{
    int status;
    pid_t pid = fork();
    if (pid == 0)
        _exit(child_case(posts, executing));
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 0;
    return WEXITSTATUS(status) == (executing ? 0 : 42);
}

typedef struct {
    MCPXAPUState *apu;
    atomic_int returned;
} StopArg;

static void *stop_main(void *opaque)
{
    StopArg *arg = opaque;
    mcpx_apu_shutdown(arg->apu);
    atomic_store(&arg->returned, 1);
    return NULL;
}

static int child_concurrent_case(void)
{
    struct timespec delay = {0, 1000000};
    uint8_t *guest;
    MCPXAPUState *apu;
    pthread_t first, second;
    StopArg a = {0}, b = {0};
    int ok;

    alarm(15);
    guest = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest == MAP_FAILED ||
        mprotect(guest, g_xbox_total_ram, PROT_READ | PROT_WRITE) ||
        mprotect(guest + 0x80000000u, g_xbox_total_ram, PROT_READ | PROT_WRITE) ||
        mprotect(guest + 0xFE830000u, 0x50000u, PROT_READ | PROT_WRITE))
        return 2;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BW_APU_DSP", "xemu", 1);
    setenv("BW_APU_DSP_FALLBACK", "0", 1);
    apu = mcpx_apu_init_standalone(guest);
    if (!apu)
        return 3;
    a.apu = b.apu = apu;
    qemu_mutex_lock(&apu->lock);
    if (pthread_create(&first, NULL, stop_main, &a))
        return 4;
    while (!probe_shutdown_started(apu))
        nanosleep(&delay, NULL);
    if (pthread_create(&second, NULL, stop_main, &b))
        return 5;
    if (pthread_join(second, NULL))
        return 6;
    ok = atomic_load(&b.returned) && !atomic_load(&a.returned) &&
         g_apu_ram_ptr == guest;
    qemu_mutex_unlock(&apu->lock);
    ok &= pthread_join(first, NULL) == 0;
    ok &= atomic_load(&a.returned) && !g_apu_ram_ptr;
    munmap(guest, (size_t)GUEST_SIZE);
    return ok ? 0 : 7;
}

static int run_concurrent_case(void)
{
    int status;
    pid_t pid = fork();
    if (pid == 0)
        _exit(child_concurrent_case());
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 0;
    return WEXITSTATUS(status) == 0;
}

int main(void)
{
    /* A caller's audio-debug settings must not create or overwrite files. */
    unsetenv("RECOMP_AUDIO_WAV");
    unsetenv("BW_AUDIO_WAV6");
    unsetenv("BW_APU_STEMS");
    unsetenv("BW_APU_GPDUMP");
    unsetenv("BW_APU_AUDIT");
    unsetenv("RECOMP_APU_VOICE_DUMP");
    int queued = run_case(1, 0);
    int executing = run_case(1, 1);
    int coalesced = run_case(3, 0);
    int concurrent = run_concurrent_case();
    puts("Known-unsafe counterexamples (reproduced does not mean safe):");
    printf("APU queued post-shutdown callback faults: %s\n", queued ? "reproduced" : "not reproduced");
    printf("APU executing callback survives shutdown: %s\n", executing ? "reproduced" : "not reproduced");
    printf("APU coalesced post-shutdown callback faults: %s\n", coalesced ? "reproduced" : "not reproduced");
    printf("APU concurrent shutdown returns early: %s\n", concurrent ? "reproduced" : "not reproduced");
    return queued && executing && coalesced && concurrent ? 0 : 1;
}
