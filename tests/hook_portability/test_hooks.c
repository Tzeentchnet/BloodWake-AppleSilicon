#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bw_edition.h"
#include "bw_hook_platform.h"
#include "bw_widescreen.h"

static uint32_t *guest_words;
#define G32(va) (guest_words[(uint32_t)(va) / 4u])
#define GF32(va) (*(float *)&G32(va))

ptrdiff_t g_xbox_mem_offset;
BW_HOOK_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
BW_HOOK_TLS uint32_t g_ebp, g_seh_ebp, g_eflags;
BW_HOOK_TLS double g_fp_stack[8];
BW_HOOK_TLS int g_fp_top;
volatile uint32_t g_icall_trace[16], g_icall_trace_idx;
volatile uint64_t g_icall_count;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
int g_bw_fl_on;

static bw_edition_t edition;
static unsigned hits[BW_FIX_COUNT], summaries;
static int host_surround, audio_calls, fix_calls, splash_calls, split_calls;
static float split_arg;
static unsigned flip_hz, segwalk;
static int gate_depth, safepoints, voice_gen_calls, voice_dispatches;
static uint32_t voice_obj, voice_apu, voice_handle, voice_sp;
static int ack_calls, queued_calls;
static uint32_t ack_regs, ack_user, expected_put;
static int expect_queued;
static int relaunch_fail, relaunch_stage;

bw_edition_t bw_edition(void) { return edition; }
const char *bw_edition_name(bw_edition_t e) { return e == BW_ED_ORIGINAL ? "original" : "remastered"; }
const char *bw_edition_title(bw_edition_t e) { return e == BW_ED_ORIGINAL ? "Original" : "Remastered"; }
void bw_fix_note_hit(bw_fix_id_t id) { hits[id]++; }
void bw_edition_add_summary(void (*fn)(void)) { assert(fn); summaries++; }
void bw_edition_summary(void) { if (relaunch_stage == 3) relaunch_stage = 4; }
int bw_audio_surround_wanted(void) { return host_surround; }
void bw_set_host_flip_pace(unsigned hz) { flip_hz = hz; }
void bw_segwalk_default(int on) { segwalk = (unsigned)on; }

void sub_000D9240_gen(void) { audio_calls++; g_eax = 0xBEEF00A5u; g_esp += 4; }
void sub_00012C40_gen(void) { fix_calls++; g_eax = 0xABCDEF01u; g_esp += 4; }
void sub_00013180_gen(void) { splash_calls++; g_eax = 0x12340001u; g_esp += 4; }
void sub_00038720_gen(void) { split_calls++; split_arg = GF32(g_esp + 0x14); g_esp += 24; }
void sub_0012C9F0_gen(void) { assert(!"unexpected original kickoff"); }

void xbox_IrqSafePoint(void) { safepoints++; }
void xbox_DispatchGateEnter(void) { assert(!gate_depth); gate_depth++; }
void xbox_DispatchGateLeave(void) { assert(gate_depth == 1); gate_depth--; }
static int state_storage;
void *g_state = &state_storage;
uint64_t mcpx_apu_mmio_read(void *state, uint64_t addr, unsigned size)
{
    assert(state == g_state && addr == 0x2054 && size == 4);
    return voice_handle;
}

static void voice_idle_guest(void)
{
    assert(gate_depth == 1 && g_ecx == voice_apu);
    assert(g_esp == voice_sp - 8 && G32(g_esp + 4) == voice_handle);
    G32(voice_obj + 8) &= ~0x10000000u;
    g_eax = g_ecx = g_edx = 0;
    g_esp += 8;
    voice_dispatches++;
}

void (*recomp_lookup(uint32_t va))(void)
{
    return va == 0x000FD27Au ? voice_idle_guest : NULL;
}

void sub_000FE041_gen(void)
{
    assert(g_ecx == voice_obj && g_eax == 0x55AAu && g_edx == 0x33CCu);
    assert(g_esp == voice_sp && !(G32(voice_obj + 8) & 0x10000000u));
    voice_gen_calls++;
    g_esp += 4;
}

void bw_ack_kick(void)
{
    assert(G32(ack_regs + 0x100410u) & 0x10000u);
    if (expect_queued)
        assert(G32(ack_user + 0x40) == (expected_put & 0x03FFFFFFu));
    G32(ack_regs + 0x100410u) &= ~0x10000u;
    ack_calls++;
}
void bw_put_queue_push(unsigned put)
{
    assert(expect_queued && put == (expected_put & 0x03FFFFFFu));
    queued_calls++;
}
void bw_flush_wait(void) { assert(!"unexpected blocked flush"); }
double bw_now_ms(void) { return (double)bw_hook_milliseconds(); }
unsigned long long bw_fl_ticks(void) { return 0; }
unsigned bw_fl_us_since(unsigned long long t) { (void)t; return 0; }
void bw_fl_event_(unsigned a, unsigned b, unsigned c, unsigned d)
{
    (void)a; (void)b; (void)c; (void)d;
}

int bw_macos_record_relaunch(uint32_t path, uint32_t data)
{
    assert(path == 0x12340u && data == 0x14000u);
    if (relaunch_fail)
        return -1;
    assert(relaunch_stage == 0);
    relaunch_stage = 1;
    return 0;
}
void bw_profile_dump(const char *why)
{
    assert(!strcmp(why, "relaunch") && relaunch_stage == 1);
    relaunch_stage = 2;
}
void bw_fl_dump(const char *why)
{
    assert(!strcmp(why, "relaunch") && relaunch_stage == 2);
    relaunch_stage = 3;
}
void sub_000DB8B9_gen(void)
{
    assert(relaunch_stage == 4);
    relaunch_stage = 5;
    g_eax = 0xF00Du;
    g_esp += 12;
}

void sub_000D9240(void);
void sub_00012C40(void);
void sub_00013180(void);
void sub_00038720(void);
void sub_000FE041(void);
void sub_0012C9F0(void);
void sub_000DB8B9(void);
unsigned bw_hifps_boot(int gameplay);
extern int g_bw_kickoff_async;

static volatile long atomic_count;
typedef struct { uint32_t value; int valid; } ThreadArg;
static void *thread_worker(void *opaque)
{
    ThreadArg *arg = opaque;
    int i;
    g_eax = arg->value;
    for (i = 0; i < 10000; i++) {
        if (g_eax != arg->value)
            return NULL;
        bw_hook_increment(&atomic_count);
    }
    arg->valid = 1;
    return NULL;
}

static void aborted(int sig) { (void)sig; _exit(101); }

static void test_platform(void)
{
    pthread_t threads[4];
    ThreadArg args[4];
    uint64_t before = bw_hook_counter(), after, size;
    long value = 5;
    int i, status;
    pid_t pid;

    g_eax = 42;
    for (i = 0; i < 4; i++) {
        args[i].value = 100u + (uint32_t)i;
        args[i].valid = 0;
        assert(!pthread_create(&threads[i], NULL, thread_worker, &args[i]));
    }
    for (i = 0; i < 4; i++) {
        assert(!pthread_join(threads[i], NULL));
        assert(args[i].valid);
    }
    assert(g_eax == 42 && atomic_count == 40000);
    assert(bw_hook_compare_exchange(&value, 9, 5) == 5 && value == 9);
    assert(bw_hook_compare_exchange(&value, 7, 5) == 9 && value == 9);
    assert(bw_hook_exchange_add(&value, 3) == 9 && value == 12);
    assert(bw_hook_exchange(&value, 4) == 12 && value == 4);
    bw_hook_store_fence();
    bw_hook_yield();
    after = bw_hook_counter();
    assert(after >= before && bw_hook_counter_frequency() == 1000000000ull);
    assert(bw_hook_file_size(__FILE__, &size) && size > 100);

    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        signal(SIGABRT, aborted);
        bw_hook_call_guest(0xDEADBEEFu);
        _exit(102);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 101);
}

static void test_audio(int forced, int surround)
{
    host_surround = surround;
    if (forced)
        setenv("BW_AUDIO_SPEAKERS", "surround", 1);
    else
        unsetenv("BW_AUDIO_SPEAKERS");
    g_eax = 1;
    g_esp = 0x20000u;
    sub_000D9240();
    assert(audio_calls == 1 && g_esp == 0x20004u);
    assert(g_eax == ((forced || surround) ? 2u : 0xBEEF00A5u));
}

static void test_hooks(void)
{
    uint32_t sp = 0x20000u;
    edition = BW_ED_ORIGINAL;
    unsetenv("BW_FIX_PROBE");
    unsetenv("BW_SPLIT_LOG");
    unsetenv("BW_SPLIT_WATER");

    g_ecx = 0x11000u; g_esp = sp;
    sub_00012C40();
    assert(fix_calls == 1 && g_eax == 0xABCDEF01u && g_esp == sp + 4);

    g_ecx = 0x11000u; g_esp = sp;
    sub_00013180();
    assert(splash_calls == 1 && g_eax == 0x12340001u && g_esp == sp + 4);

    edition = BW_ED_REMASTERED;
    g_bw_fix_on[BW_FIX_BOAT_LANDING_WEIGHT] = 1;
    G32(0x11000u + 0x28) = 0x12000u;
    G32(0x11000u + 0xE0) = 1500;
    g_ecx = 0x11000u; g_esp = sp;
    sub_00013180();
    assert(splash_calls == 2 && g_eax == 0x12340000u && g_esp == sp + 4);
    assert(hits[BW_FIX_BOAT_LANDING_WEIGHT] == 1 && summaries == 1);

    g_bw_fix_on[BW_FIX_SPLIT_FULL_DETAIL] = 0;
    g_ecx = 0x13000u; g_esp = sp;
    G32(sp) = 0x000390C0u; GF32(sp + 0x14) = 4.0f;
    sub_00038720();
    assert(split_calls == 1 && split_arg == 4.0f && g_esp == sp + 24);
    g_bw_fix_on[BW_FIX_SPLIT_FULL_DETAIL] = 1;
    g_ecx = 0x13000u; g_esp = sp;
    G32(sp) = 0x000390C0u; GF32(sp + 0x14) = 4.0f;
    sub_00038720();
    assert(split_calls == 2 && split_arg == 1.0f && g_esp == sp + 24);
    assert(hits[BW_FIX_SPLIT_FULL_DETAIL] == 1);
}

static void test_voice_wait(void)
{
    const uint32_t buf = 0x16000u, desc = 0x17000u, vmem = 0x18000u;
    voice_obj = 0x11000u; voice_apu = 0x14000u; voice_handle = 5; voice_sp = 0x20000u;
    G32(voice_obj + 8) = 0x10000001u;
    G32(voice_obj + 0xC) = voice_apu;
    G32(voice_obj + 0x10) = voice_handle;
    G32(voice_obj + 0xDC) = 0;
    G32(voice_obj + 0xE0) = buf;
    G32(voice_apu + 0x30) = vmem;
    G32(buf + 0x10) = desc;
    G32(desc) = 2u << 16;
    G32(vmem + voice_handle * 128u + 0x54) = 0;
    setenv("BW_VOICE_WAIT_CHECK_MS", "0", 1);
    setenv("BW_VOICE_WAIT_REACH_MS", "0", 1);
    unsetenv("BW_VOICE_WAIT_RECOVER");
    g_ecx = voice_obj; g_esp = voice_sp; g_eax = 0x55AAu; g_edx = 0x33CCu;
    sub_000FE041();
    assert(voice_dispatches == 1 && voice_gen_calls == 1 && gate_depth == 0);
    assert(safepoints > 64 && g_esp == voice_sp + 4);
}

static void test_kickoff(void)
{
    uint32_t dev = 0x10000u, container = 0x30000u, sp = 0x20000u;
    ack_user = 0x40000u; ack_regs = 0x50000u;
    G32(0x001338E8u) = 0;
    G32(0x00136B00u) = container;
    G32(container + 0x2308) = ack_regs;
    G32(dev + 0x2304) = ack_user;
    G32(dev + 0xC) = 0;
    G32(ack_regs + 0x100410) = 4;
    expected_put = 0x12345678u;
    G32(dev) = expected_put;
    g_ecx = dev; g_esp = sp; g_bw_kickoff_async = 0;
    unsetenv("BW_KICKOFF_SPIN"); unsetenv("BW_KICKLOG");
    sub_0012C9F0();
    assert(ack_calls == 1 && queued_calls == 0);
    assert(G32(ack_user + 0x40) == (expected_put & 0x03FFFFFFu));
    assert(G32(dev + 0x18) == expected_put && G32(ack_regs + 0x100410) == 4);
    assert(g_eax == ack_user && g_edx == dev && g_ecx == dev && g_esp == sp + 4);

    expected_put = 0x00123450u;
    G32(dev) = expected_put; G32(ack_user + 0x44) = expected_put;
    g_ecx = dev; g_esp = sp; g_bw_kickoff_async = 3; expect_queued = 1;
    sub_0012C9F0();
    assert(ack_calls == 2 && queued_calls == 1);
    assert(g_eax == ack_user && g_edx == dev && g_esp == sp + 4);
}

static void test_relaunch(void)
{
    uint32_t sp = 0x20000u;
    int status;
    pid_t pid;
    G32(sp + 4) = 0x12340u; G32(sp + 8) = 0x14000u;
    g_esp = sp;
    sub_000DB8B9();
    assert(relaunch_stage == 5 && g_eax == 0xF00Du && g_esp == sp + 12);

    pid = fork();
    assert(pid >= 0);
    if (!pid) {
        relaunch_fail = 1; relaunch_stage = 0; g_esp = sp;
        sub_000DB8B9();
        _exit(100);
    }
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
}

static void check_widescreen_diagnostic(bw_edition_t ed, int enabled, int log,
                                        const char *expected)
{
    int fds[2], saved_stderr;
    ssize_t n;
    size_t used = 0;
    char output[256];

    edition = ed;
    g_bw_fix_on[BW_FIX_WIDESCREEN] = (unsigned char)enabled;
    assert(fflush(stderr) == 0);
    assert(pipe(fds) == 0);
    saved_stderr = dup(STDERR_FILENO);
    assert(saved_stderr >= 0);
    assert(dup2(fds[1], STDERR_FILENO) == STDERR_FILENO);
    assert(close(fds[1]) == 0);
    bw_ws_init(1, log);
    assert(fflush(stderr) == 0);
    assert(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    assert(close(saved_stderr) == 0);

    do {
        n = read(fds[0], output + used, sizeof output - 1 - used);
        assert(n >= 0);
        used += (size_t)n;
        assert(used < sizeof output - 1);
    } while (n);
    output[used] = '\0';
    assert(close(fds[0]) == 0);
    assert(!strcmp(output, expected));
    assert(fabsf(bw_ws_aspect() - 4.0f / 3.0f) < 1e-6f && bw_ws_k() == 1.0f);
}

static void test_widescreen_diagnostic(void)
{
    const char *unavailable =
        "[WS] off: the Metal renderer does not yet support widescreen targets and markers; using 4:3\n";

    unsetenv("BW_ASPECT");
    check_widescreen_diagnostic(BW_ED_ORIGINAL, 0, 0, "");
    setenv("BW_ASPECT", "21:9", 1);
    check_widescreen_diagnostic(BW_ED_ORIGINAL, 0, 1, "");
    check_widescreen_diagnostic(BW_ED_REMASTERED, 0, 0, "");
    check_widescreen_diagnostic(BW_ED_REMASTERED, 0, 1,
                                "[WS] off: 4:3 (BW_FIX=-WIDESCREEN)\n");
    check_widescreen_diagnostic(BW_ED_REMASTERED, 1, 0, unavailable);
    check_widescreen_diagnostic(BW_ED_REMASTERED, 1, 1, unavailable);
    unsetenv("BW_ASPECT");
    check_widescreen_diagnostic(BW_ED_REMASTERED, 1, 0, unavailable);
}

static void test_display(void)
{
    edition = BW_ED_ORIGINAL;
    g_bw_fix_on[BW_FIX_WIDESCREEN] = 0;
    setenv("BW_GPU", "metal", 1); setenv("BW_ASPECT", "21:9", 1);
    bw_ws_init(1, 0);
    assert(fabsf(bw_ws_aspect() - 4.0f / 3.0f) < 1e-6f && bw_ws_k() == 1.0f);
    assert(bw_hifps_boot(1) == 0 && flip_hz == 0);

    edition = BW_ED_REMASTERED;
    g_bw_fix_on[BW_FIX_WIDESCREEN] = 1;
    bw_ws_init(1, 0);
    assert(fabsf(bw_ws_aspect() - 4.0f / 3.0f) < 1e-6f && bw_ws_k() == 1.0f);
    g_bw_fix_on[BW_FIX_HIFPS] = 1;
    setenv("BW_HIFPS", "0", 1);
    assert(bw_hifps_boot(1) == 0 && flip_hz == 0);
    setenv("BW_HIFPS", "90", 1);
    assert(bw_hifps_boot(0) == 0 && flip_hz == 0);
    assert(bw_hifps_boot(1) == 90 && flip_hz == 90 && segwalk == 1);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    guest_words = calloc(0x100000, sizeof *guest_words);
    assert(guest_words);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest_words;
    if (!strcmp(argv[1], "forced")) {
        test_audio(1, 0);
    } else if (!strcmp(argv[1], "surround")) {
        test_audio(0, 1);
    } else if (!strcmp(argv[1], "unpaced")) {
        setenv("RECOMP_FLIP_PACING", "0", 1);
        test_display();
    } else if (!strcmp(argv[1], "widescreen")) {
        test_widescreen_diagnostic();
    } else {
        assert(!strcmp(argv[1], "passthrough"));
        test_platform();
        test_audio(0, 0);
        test_hooks();
        test_voice_wait();
        test_kickoff();
        test_relaunch();
        test_display();
    }
    free(guest_words);
    puts("hook portability tests passed");
    return 0;
}
