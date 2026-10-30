/* Manual counterexample, not a shutdown-safety test: success means a
 * real queued or executing DPC outlived the real APU shutdown. The ISR and
 * DPC bodies and guest memory are synthetic; no device MMIO trap is enabled. */
#include "apu.h"
#include "apu_state.h"
#include "bw_edition.h"
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define IRQ_OBJECT 0x1000u
#define ISR_ADDRESS 0x1234u
#define DPC_OBJECT 0x2000u
#define DPC_ADDRESS 0x2345u
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
uint32_t xbox_GetConnectedInterrupt(uint32_t vector)
{
    return vector == 5 ? IRQ_OBJECT : 0;
}
uint32_t xbox_AllocThreadTib(void) { return 0; }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int holder_ready, holder_release;
    int isr_queued;
    int block_dpc, dpc_entered, dpc_release, dpc_after_stop;
    int stop_returned, drained, drain_result;
} Probe;

static Probe *s_probe;

static void guest_isr(void)
{
    int queued = xbox_DpcQueue(DPC_OBJECT, 1, 2);
    Probe *p = s_probe;

    pthread_mutex_lock(&p->lock);
    p->isr_queued = queued;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->lock);
    g_esp += 12;
}

static void guest_dpc(void)
{
    Probe *p = s_probe;

    pthread_mutex_lock(&p->lock);
    p->dpc_entered = 1;
    pthread_cond_broadcast(&p->cv);
    while (p->block_dpc && !p->dpc_release)
        pthread_cond_wait(&p->cv, &p->lock);
    if (p->stop_returned && !g_apu_ram_ptr)
        p->dpc_after_stop = 1;
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
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static int child_case(int executing)
{
    Probe p = {.lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
               .block_dpc = executing};
    uint8_t *guest;
    MCPXAPUState *apu;
    pthread_t holder, drainer;
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
    *(uint32_t *)(guest + DPC_OBJECT + 12) = DPC_ADDRESS;
    setenv("RECOMP_DISPATCH_GATE", "1", 1);
    setenv("RECOMP_IRQ_SAFE_POINTS", "1", 1);
    setenv("RECOMP_DPC_ON_RAISE", "1", 1);
    unsetenv("RECOMP_IRQ_SAFE_FORCE");
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BW_APU_DSP", "xemu", 1);
    setenv("BW_APU_DSP_FALLBACK", "0", 1);
    apu = mcpx_apu_init_standalone(guest);
    if (!apu)
        return 3;
    s_probe = &p;
    probe_register_real_apu_isr();
    if (pthread_create(&holder, NULL, holder_main, &p) != 0)
        return 4;
    pthread_mutex_lock(&p.lock);
    while (!p.holder_ready)
        pthread_cond_wait(&p.cv, &p.lock);
    pthread_mutex_unlock(&p.lock);
    if (xbox_IrqPost(XBOX_IRQ_APU) != XBOX_IRQ_POSTED)
        return 5;
    pthread_mutex_lock(&p.lock);
    p.holder_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    if (pthread_join(holder, NULL) != 0 || !p.isr_queued || !xbox_DpcPending())
        return 6;

    if (executing) {
        if (pthread_create(&drainer, NULL, drain_main, &p) != 0)
            return 7;
        pthread_mutex_lock(&p.lock);
        while (!p.dpc_entered)
            pthread_cond_wait(&p.cv, &p.lock);
        pthread_mutex_unlock(&p.lock);
        ok &= !xbox_DpcPending();
    }

    mcpx_apu_shutdown(apu);
    ok &= g_apu_ram_ptr == NULL;
    if (!executing)
        ok &= xbox_DpcPending();
    pthread_mutex_lock(&p.lock);
    p.stop_returned = 1;
    p.dpc_release = 1;
    pthread_cond_broadcast(&p.cv);
    pthread_mutex_unlock(&p.lock);
    if (executing)
        ok &= pthread_join(drainer, NULL) == 0;
    else
        drain_main(&p);
    ok &= p.dpc_after_stop && p.drained == 1 && p.drain_result == 0;
    ok &= !xbox_DpcPending();
    munmap(guest, (size_t)GUEST_SIZE);
    return ok ? 0 : 8;
}

static int run_case(int executing)
{
    int status;
    pid_t pid = fork();

    if (pid == 0)
        _exit(child_case(executing));
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return 0;
    return WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
    int queued, executing;

    alarm(35);
    /* A caller's audio-debug settings must not create or overwrite files. */
    unsetenv("RECOMP_AUDIO_WAV");
    unsetenv("BW_AUDIO_WAV6");
    unsetenv("BW_APU_STEMS");
    unsetenv("BW_APU_GPDUMP");
    unsetenv("BW_APU_AUDIT");
    unsetenv("RECOMP_APU_VOICE_DUMP");
    if (argc == 2 && (!strcmp(argv[1], "--queued") ||
                      !strcmp(argv[1], "--executing"))) {
        int rc = child_case(!strcmp(argv[1], "--executing"));
        printf("Known-unsafe DPC counterexample: %s\n",
               rc == 0 ? "reproduced" : "not reproduced");
        fflush(stdout);
        _exit(rc);
    }
    if (argc != 1)
        return 2;
    queued = run_case(0);
    executing = run_case(1);
    alarm(0);
    puts("Known-unsafe counterexamples (reproduced does not mean safe):");
    printf("ISR-queued DPC runs after APU shutdown: %s\n",
           queued ? "reproduced" : "not reproduced");
    printf("Executing DPC survives shutdown with empty queue: %s\n",
           executing ? "reproduced" : "not reproduced");
    return queued && executing ? 0 : 1;
}
