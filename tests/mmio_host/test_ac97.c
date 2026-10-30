#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int s_fail_mprotect;
static int test_mprotect(void *addr, size_t size, int prot)
{
    if (s_fail_mprotect) {
        s_fail_mprotect = 0;
        errno = EPERM;
        return -1;
    }
    return mprotect(addr, size, prot);
}

#define mprotect test_mprotect
#include "../../src/toolkit_patches/kernel/xbox_memory_layout.c"
#undef mprotect
#include "../../src/bw_posix_fault.h"

#define CHECK(expr) do {                                                      \
    if (!(expr)) {                                                            \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
        return 0;                                                             \
    }                                                                         \
} while (0)

static _Atomic unsigned s_fault_count;
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "fault counter must be signal-safe");

static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    int saved_errno = errno;
    (void)signal_number;
    if (!xbox_Ac97PosixFault(context, (uintptr_t)info->si_addr))
        _exit(90);
    atomic_fetch_add_explicit(&s_fault_count, 1, memory_order_relaxed);
    errno = saved_errno;
}

static int drain_faults(void)
{
    return (int)atomic_exchange_explicit(&s_fault_count, 0, memory_order_relaxed);
}

static int test_real_stores(volatile uint8_t *guest, size_t page_size)
{
    uint8_t vector[16] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
    };
    uint64_t a = UINT64_C(0x0123456789abcdef);
    uint64_t b = UINT64_C(0xfedcba9876543210);
    uint32_t word = 0xcafe1234u;
    uintptr_t base;
    uint32_t index;
    int32_t negative;

    CHECK(drain_faults() == 0);
    guest[0x10b] = 3;
    CHECK(guest[0x10b] == 1);
    ((volatile Ac97U16 *)(guest + 0x10a))->v = 0x02abu;
    CHECK(((volatile Ac97U16 *)(guest + 0x10a))->v == 0x00abu);
    ((volatile Ac97U32 *)(guest + 0x108))->v = 0xa2aabbccu;
    CHECK(((volatile Ac97U32 *)(guest + 0x108))->v == 0xa0aabbccu);
    ((volatile Ac97U64 *)(guest + 0x108))->v = UINT64_C(0x01020304a2bbccdd);
    CHECK(((volatile Ac97U64 *)(guest + 0x108))->v ==
          UINT64_C(0x01020304a0bbccdd));
    guest[0x17b] = 0xff;
    CHECK(guest[0x17b] == 0xfd);
    guest[0x18b] = 0xff;
    CHECK(guest[0x18b] == 0xff);
    guest[page_size - 1] = 0x5a;
    CHECK(guest[page_size - 1] == 0x5a);
    ((volatile Ac97U32 *)(guest + 0x201))->v = word;
    CHECK(((volatile Ac97U32 *)(guest + 0x201))->v == word);

    base = (uintptr_t)guest + 0x220;
    __asm__ volatile("str %w[value], [%[base], #4]!"
                     : [base] "+&r"(base) : [value] "r"(word) : "memory");
    CHECK(base == (uintptr_t)guest + 0x224);
    CHECK(((volatile Ac97U32 *)(guest + 0x224))->v == word);

    base = (uintptr_t)guest + 0x230;
    __asm__ volatile("str %x[value], [%[base]], #8"
                     : [base] "+&r"(base) : [value] "r"(a) : "memory");
    CHECK(base == (uintptr_t)guest + 0x238);
    CHECK(((volatile Ac97U64 *)(guest + 0x230))->v == a);

    base = (uintptr_t)guest + 0x248;
    __asm__ volatile("stur %w[value], [%[base], #-4]"
                     : : [base] "r"(base), [value] "r"(word) : "memory");
    CHECK(((volatile Ac97U32 *)(guest + 0x244))->v == word);

    base = (uintptr_t)guest + 0x250;
    index = 3;
    __asm__ volatile("str %w[value], [%[base], %w[index], uxtw #2]"
                     : : [base] "r"(base), [index] "r"(index),
                         [value] "r"(word) : "memory");
    CHECK(((volatile Ac97U32 *)(guest + 0x25c))->v == word);

    base = (uintptr_t)guest + 0x270;
    negative = -2;
    __asm__ volatile("str %w[value], [%[base], %w[index], sxtw #2]"
                     : : [base] "r"(base), [index] "r"(negative),
                         [value] "r"(word) : "memory");
    CHECK(((volatile Ac97U32 *)(guest + 0x268))->v == word);

    base = (uintptr_t)guest + 0x280;
    __asm__ volatile("stp %x[a], %x[b], [%[base]], #16"
                     : [base] "+&r"(base) : [a] "r"(a), [b] "r"(b) : "memory");
    CHECK(base == (uintptr_t)guest + 0x290);
    CHECK(((volatile Ac97U64 *)(guest + 0x280))->v == a);
    CHECK(((volatile Ac97U64 *)(guest + 0x288))->v == b);

    base = (uintptr_t)guest + 0x2a0;
    __asm__ volatile("ldr q0, [%[source]]\n\tstr q0, [%[base]]"
                     : : [source] "r"(vector), [base] "r"(base)
                     : "v0", "memory");
    CHECK(memcmp((const void *)(guest + 0x2a0), vector, sizeof vector) == 0);
    CHECK(drain_faults() == 15);
    return 1;
}

static int test_rejected_contexts(volatile uint8_t *guest, size_t page_size)
{
    struct __darwin_mcontext64 state = {0};
    ucontext_t uc = {0};
    uint32_t instruction = 0xf9000020u; /* str x0, [x1] */
    uintptr_t pc = (uintptr_t)&instruction;

    uc.uc_mcontext = &state;
    state.__ss.__pc = pc;
    state.__ss.__x[0] = UINT64_C(0x123456789abcdef0);
    state.__ss.__x[1] = (uintptr_t)guest + page_size - 4;
    state.__es.__esr = 0x92000047u;
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    CHECK(state.__ss.__pc == pc);

    state.__ss.__x[1] = (uintptr_t)guest + 0x200;
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1] + 4));
    CHECK(state.__ss.__pc == pc);
    state.__es.__esr &= ~(1u << 6);
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x92000061u; /* alignment fault */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x92000050u; /* synchronous external abort */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x92000049u; /* access-flag fault */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x92000447u; /* fault address not valid */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x920000c7u; /* abort during a stage-1 table walk */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    state.__es.__esr = 0x92000047u;
    instruction = 0xf9400020u; /* ldr x0, [x1] */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    instruction = 0xc8007c20u; /* stxr: deliberately unsupported */
    CHECK(!xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));

    instruction = 0xa9000440u; /* stp x0, x1, [x2] */
    state.__ss.__x[2] = (uintptr_t)guest - 8;
    CHECK(!xbox_Ac97PosixFault(&uc, (uintptr_t)guest));
    CHECK(state.__ss.__pc == pc);
    CHECK(!xbox_Ac97PosixFault(NULL, (uintptr_t)guest));

    instruction = 0x39000020u; /* strb w0, [x1] */
    state.__ss.__x[0] = 0xff;
    state.__ss.__x[1] = (uintptr_t)guest + 0x11b;
    CHECK(xbox_Ac97PosixFault(&uc, state.__ss.__x[1]));
    CHECK(state.__ss.__pc == pc + 4);
    CHECK(guest[0x11b] == 0xfd);
    return 1;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int go;
    int failed;
    volatile uint8_t *control;
} Stress;

static void *stress_writer(void *opaque)
{
    Stress *s = opaque;
    int i, failed = 0;

    pthread_mutex_lock(&s->mutex);
    s->ready++;
    pthread_cond_broadcast(&s->cond);
    while (!s->go)
        pthread_cond_wait(&s->cond, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    for (i = 0; i < 500; i++) {
        *s->control = 0xff;
        if (*s->control & AC97_RR)
            failed = 1;
    }
    if (failed) {
        pthread_mutex_lock(&s->mutex);
        s->failed = 1;
        pthread_mutex_unlock(&s->mutex);
    }
    return NULL;
}

static int test_concurrent_stores(volatile uint8_t *guest)
{
    Stress s = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0,
                guest + 0x10b};
    pthread_t workers[4];
    int i;

    CHECK(drain_faults() == 0);
    for (i = 0; i < 4; i++)
        CHECK(pthread_create(&workers[i], NULL, stress_writer, &s) == 0);
    pthread_mutex_lock(&s.mutex);
    while (s.ready != 4)
        pthread_cond_wait(&s.cond, &s.mutex);
    s.go = 1;
    pthread_cond_broadcast(&s.cond);
    pthread_mutex_unlock(&s.mutex);
    for (i = 0; i < 4; i++)
        CHECK(pthread_join(workers[i], NULL) == 0);
    CHECK(!s.failed);
    CHECK(drain_faults() == 4 * 500);
    CHECK(pthread_cond_destroy(&s.cond) == 0);
    CHECK(pthread_mutex_destroy(&s.mutex) == 0);
    return 1;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
    int release;
} FileLock;

static void *hold_stderr(void *opaque)
{
    FileLock *s = opaque;
    flockfile(stderr);
    pthread_mutex_lock(&s->mutex);
    s->ready = 1;
    pthread_cond_signal(&s->cond);
    while (!s->release)
        pthread_cond_wait(&s->cond, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    funlockfile(stderr);
    return NULL;
}

static int test_locked_stdio(volatile uint8_t *guest)
{
    FileLock s = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
    pthread_t worker;

    CHECK(pthread_create(&worker, NULL, hold_stderr, &s) == 0);
    pthread_mutex_lock(&s.mutex);
    while (!s.ready)
        pthread_cond_wait(&s.cond, &s.mutex);
    pthread_mutex_unlock(&s.mutex);

    guest[0x12b] = 3;
    CHECK(guest[0x12b] == 1);
    pthread_mutex_lock(&s.mutex);
    s.release = 1;
    pthread_cond_signal(&s.cond);
    pthread_mutex_unlock(&s.mutex);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(drain_faults() == 1);
    CHECK(pthread_cond_destroy(&s.cond) == 0);
    CHECK(pthread_mutex_destroy(&s.mutex) == 0);
    return 1;
}

static int test_watch_disabled(void)
{
    struct sigaction bus_before, bus_after, segv_before, segv_after;

    CHECK(sigaction(SIGBUS, NULL, &bus_before) == 0);
    CHECK(sigaction(SIGSEGV, NULL, &segv_before) == 0);
    CHECK(setenv("RECOMP_WATCH", "[[0x4000]]+4", 1) == 0);
    xbox_WatchInit();
    CHECK(unsetenv("RECOMP_WATCH") == 0);
    CHECK(sigaction(SIGBUS, NULL, &bus_after) == 0);
    CHECK(sigaction(SIGSEGV, NULL, &segv_after) == 0);
    CHECK(bus_before.sa_sigaction == bus_after.sa_sigaction);
    CHECK(segv_before.sa_sigaction == segv_after.sa_sigaction);
    CHECK(bus_before.sa_flags == bus_after.sa_flags);
    CHECK(segv_before.sa_flags == segv_after.sa_flags);
    return 1;
}

static int test_unknown_fault(void)
{
    size_t size = (size_t)sysconf(_SC_PAGESIZE);
    volatile uint8_t *page = mmap(NULL, size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
    int status;
    pid_t pid;

    CHECK(page != MAP_FAILED);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        volatile uint8_t value = *page;
        (void)value;
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 90);
    CHECK(munmap((void *)page, size) == 0);
    return 1;
}

static void prior_fault_handler(int signal_number, siginfo_t *info, void *context)
{
    (void)signal_number;
    (void)info;
    (void)context;
    _exit(92);
}

static int host_forwards_fault(volatile uint8_t *page, uint32_t guest_address)
{
    int status;
    pid_t pid;

    bw_posix_fault_set_base((uintptr_t)page - guest_address);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        volatile uint8_t value = *page;
        (void)value;
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 92);
    return 1;
}

static int test_host_integration(volatile uint8_t *guest, size_t page_size)
{
    struct sigaction action = {0}, previous_bus, previous_segv, restored;
    volatile uint8_t *blocked;
    pid_t pid;
    int status;

    action.sa_sigaction = prior_fault_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGBUS, &action, &previous_bus) == 0);
    CHECK(sigaction(SIGSEGV, &action, &previous_segv) == 0);
    CHECK(bw_posix_fault_install() == 0);
    bw_posix_fault_set_base((uintptr_t)guest -
                            (XBOX_MCPX_BASE + AC97_NABM_OFFSET));
    errno = EAGAIN;
    guest[0x13b] = 3;
    CHECK(guest[0x13b] == 1 && errno == EAGAIN);

    blocked = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(blocked != MAP_FAILED);
    CHECK(host_forwards_fault(blocked, 0xFE800000u));
    CHECK(host_forwards_fault(blocked, 0xFED00000u));
    CHECK(host_forwards_fault(blocked, 0x00003000u));
    CHECK(munmap((void *)blocked, page_size) == 0);

    /* The writable alias was mapped VM_INHERIT_NONE. A child write faults
     * once on the guest page and again inside the handler on the absent
     * alias; the nested fault must terminate rather than recurse. */
    bw_posix_fault_set_base((uintptr_t)guest -
                            (XBOX_MCPX_BASE + AC97_NABM_OFFSET));
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        guest[0x10b] = 3;
        _exit(0);
    }
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && (WEXITSTATUS(status) == 128 + SIGBUS ||
                                WEXITSTATUS(status) == 128 + SIGSEGV));

    bw_posix_fault_uninstall();
    CHECK(sigaction(SIGBUS, NULL, &restored) == 0);
    CHECK(restored.sa_sigaction == prior_fault_handler);
    CHECK(sigaction(SIGSEGV, NULL, &restored) == 0);
    CHECK(restored.sa_sigaction == prior_fault_handler);
    CHECK(sigaction(SIGBUS, &previous_bus, NULL) == 0);
    CHECK(sigaction(SIGSEGV, &previous_segv, NULL) == 0);
    return 1;
}

static int arm_mapping(size_t mapping_size)
{
    g_mcpx_memory = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(g_mcpx_memory != MAP_FAILED);
    CHECK(ac97_arm_write_trap());
    CHECK(g_ac97_page != NULL);
    CHECK(atomic_load_explicit(&g_ac97_posix_active, memory_order_acquire) != NULL);
    return 1;
}

static int test_failed_protection(size_t mapping_size)
{
    mach_vm_address_t alias, region;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t rc;

    g_mcpx_memory = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(g_mcpx_memory != MAP_FAILED);
    s_fail_mprotect = 1;
    CHECK(!ac97_arm_write_trap());
    CHECK(!s_fail_mprotect && !g_ac97_page);
    CHECK(atomic_load_explicit(&g_ac97_posix_active, memory_order_acquire) == NULL);
    alias = (mach_vm_address_t)g_ac97_posix_page.writable;
    CHECK(alias != 0);
    region = alias;
    rc = mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                        (vm_region_info_t)&info, &count, &object);
    if (object != MACH_PORT_NULL)
        CHECK(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
    CHECK(rc == KERN_INVALID_ADDRESS || (rc == KERN_SUCCESS && region > alias));
    CHECK(munmap(g_mcpx_memory, mapping_size) == 0);
    g_mcpx_memory = NULL;
    return 1;
}

static int disarm_mapping(size_t mapping_size)
{
    ac97_posix_disarm();
    CHECK(g_ac97_page == NULL);
    CHECK(atomic_load_explicit(&g_ac97_posix_active, memory_order_acquire) == NULL);
    CHECK(munmap(g_mcpx_memory, mapping_size) == 0);
    g_mcpx_memory = NULL;
    return 1;
}

int main(void)
{
    struct sigaction action = {0};
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t mapping_size = AC97_NABM_OFFSET + page_size;
    volatile uint8_t *guest;
    void *incomplete;

    if (ac97_arm_write_trap())
        return 1;
    incomplete = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (incomplete == MAP_FAILED ||
            munmap((uint8_t *)incomplete + AC97_NABM_OFFSET, page_size) != 0)
        return 1;
    g_mcpx_memory = incomplete;
    if (ac97_arm_write_trap() || g_ac97_page)
        return 1;
    if (munmap(incomplete, AC97_NABM_OFFSET) != 0)
        return 1;
    g_mcpx_memory = NULL;
    if (!test_failed_protection(mapping_size))
        return 1;
    action.sa_sigaction = fault_handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    if (sigaction(SIGBUS, &action, NULL) || sigaction(SIGSEGV, &action, NULL)
            || !arm_mapping(mapping_size))
        return 1;
    guest = (volatile uint8_t *)g_ac97_page;
    if (!test_real_stores(guest, page_size)
            || !test_rejected_contexts(guest, page_size)
            || !test_concurrent_stores(guest)
            || !test_locked_stdio(guest)
            || !test_watch_disabled()
            || !test_unknown_fault()
            || !test_host_integration(guest, page_size)
            || !disarm_mapping(mapping_size)
            || !arm_mapping(mapping_size))
        return 1;
    guest = (volatile uint8_t *)g_ac97_page;
    guest[0x10b] = 3;
    if (guest[0x10b] != 1 || drain_faults() != 1
            || !disarm_mapping(mapping_size))
        return 1;
    return 0;
}
