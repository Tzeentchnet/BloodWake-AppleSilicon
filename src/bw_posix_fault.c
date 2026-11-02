#define _DARWIN_C_SOURCE 1
#include "bw_posix_fault.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)

#include "platform/mmio_decode_a64.h"

/* AC'97 has a bounded, signal-safe store decoder operating through a writable
 * shared alias. APU callbacks may take locks or call host code, so a fault is
 * first redirected to an ordinary-thread trampoline on the same guest thread.
 * OHCI remains disabled and is never dispatched here. */
extern int xbox_Ac97PosixFault(void *ucv, uintptr_t fault);
extern int xbox_PosixMmioFault(void *ucv, uintptr_t fault);

#define BW_APU_BASE UINT32_C(0xFE800000)
#define BW_APU_TRAP_BYTES UINT32_C(0x00030000)
#define BW_DEFERRED_SLOTS 32u
#define BW_DEFERRED_STACK_SIZE (64u * 1024u)

typedef __typeof__(*(((ucontext_t *)0)->uc_mcontext)) BwSavedMachineContext;
typedef struct {
    uintptr_t owner;
    unsigned active;
    ucontext_t context;
    BwSavedMachineContext machine;
    uint8_t *stack_mapping;
    uint8_t *stack;
    uintptr_t access_start;
    int saved_errno;
} BwDeferredFault;

static uintptr_t s_guest_base;
static int s_apu_enabled;
static struct sigaction s_previous_bus, s_previous_segv;
static const struct sigaction s_default = {.sa_handler = SIG_DFL};
static int s_installed;
static uintptr_t s_active[128];
static BwDeferredFault s_deferred[BW_DEFERRED_SLOTS];
static volatile uint32_t *s_restore_page;
static size_t s_page_size;

_Static_assert(__atomic_always_lock_free(sizeof(uintptr_t), 0),
               "signal address atomics must be lock-free");
_Static_assert(__atomic_always_lock_free(sizeof(unsigned), 0),
               "signal state atomics must be lock-free");

extern void bw_posix_restore_trap(volatile uint32_t *) __attribute__((noreturn));
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _bw_posix_restore_trap\n"
        "_bw_posix_restore_trap:\n"
        "str wzr, [x0]\n"
        "brk #0\n");

static void copy_signal_bytes(void *to, const void *from, size_t count)
{
    volatile unsigned char *dst = to;
    const volatile unsigned char *src = from;
    for (size_t i = 0; i < count; i++)
        dst[i] = src[i];
}

static int add_scaled(uint64_t base, uint64_t value, unsigned shift,
                      int is_signed, uint64_t *result)
{
    int negative = is_signed && (value >> 63);
    uint64_t magnitude = negative ? (~value + 1) : value;
    if (magnitude > (UINT64_MAX >> shift))
        return 0;
    magnitude <<= shift;
    if (negative) {
        if (magnitude > base)
            return 0;
        *result = base - magnitude;
    } else {
        if (magnitude > UINT64_MAX - base)
            return 0;
        *result = base + magnitude;
    }
    return 1;
}

static uint64_t sign_bits(uint32_t value, unsigned bits)
{
    uint64_t sign = UINT64_C(1) << (bits - 1);
    return (value & sign) ? (uint64_t)value | (UINT64_MAX << bits) : value;
}

/* Restrict deferred accesses to allocated scalar/vector load/store forms and
 * reject any instruction whose complete span is not inside the trapped APU
 * aperture. No model callback or saved-context mutation occurs before this
 * check succeeds. */
static int checked_span(ucontext_t *uc, uintptr_t fault, uintptr_t start,
                        size_t length, uint32_t *offset)
{
    uint32_t esr, insn;
    unsigned rn, rt, vector, bytes, element, load, mode;
    uint64_t base, address, ignored;

    if (!uc || !uc->uc_mcontext || !offset || !length ||
        length - 1 > UINTPTR_MAX - start ||
        A64_CTX_PC(uc) > UINTPTR_MAX - 4 || (A64_CTX_PC(uc) & 3))
        return 0;
    esr = uc->uc_mcontext->__es.__esr;
    if (((esr >> 26) & 63) != 0x24 || (esr & ((1u << 10) | (1u << 7))) ||
        !(((esr & 63) >= 4 && (esr & 63) <= 7) ||
          ((esr & 63) >= 13 && (esr & 63) <= 15)))
        return 0;
    insn = *(const uint32_t *)(uintptr_t)A64_CTX_PC(uc);
    rn = (insn >> 5) & 31;
    rt = insn & 31;
    vector = (insn >> 26) & 1;
    base = *mmio_a64_xreg(uc, (int)rn);
    if (rn == 31 && (base & 15))
        return 0;

    if ((insn & 0x3b000000u) == 0x39000000u ||
        (insn & 0x3b000000u) == 0x38000000u) {
        unsigned size = insn >> 30, opc = (insn >> 22) & 3;
        unsigned scale, form = (insn >> 24) & 3;
        if (vector) {
            if ((opc & 2) && size != 0)
                return 0;
            bytes = (opc & 2) ? 16 : 1u << size;
            load = opc & 1;
        } else {
            if ((opc == 2 && size == 3) || (opc == 3 && size >= 2))
                return 0;
            bytes = 1u << size;
            load = opc != 0;
        }
        scale = bytes == 16 ? 4 : size;
        element = bytes;
        if (form == 1) {
            if (!add_scaled(base, (insn >> 10) & 0xfff, scale, 0, &address))
                return 0;
        } else if (form == 0 && (insn & (1u << 21))) {
            unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7;
            uint64_t reg = rm == 31 ? 0 : *mmio_a64_xreg(uc, (int)rm);
            int signed_offset = option == 6 || option == 7;
            if (((insn >> 10) & 3) != 2 ||
                (option != 2 && option != 3 && option != 6 && option != 7))
                return 0;
            if (option == 2)
                reg = (uint32_t)reg;
            else if (option == 6)
                reg = sign_bits((uint32_t)reg, 32);
            if (!add_scaled(base, reg, (insn & (1u << 12)) ? scale : 0,
                            signed_offset, &address))
                return 0;
        } else if (form == 0) {
            uint64_t imm = sign_bits((insn >> 12) & 0x1ff, 9);
            mode = (insn >> 10) & 3;
            if (mode == 2 || (mode != 0 && !vector && rn != 31 && rn == rt))
                return 0;
            if (!add_scaled(base, imm, 0, 1, &ignored))
                return 0;
            address = mode == 1 ? base : ignored;
        } else {
            return 0;
        }
    } else if ((insn & 0x3a000000u) == 0x28000000u) {
        unsigned opc = insn >> 30, rt2 = (insn >> 10) & 31;
        uint64_t imm = sign_bits((insn >> 15) & 0x7f, 7);
        mode = (insn >> 23) & 7;
        load = (insn >> 22) & 1;
        if (mode < 1 || mode > 3 || (load && rt == rt2))
            return 0;
        if (vector) {
            if (opc > 1)
                return 0;
            element = 4u << opc;
        } else {
            if (opc == 0 || (opc == 1 && load))
                element = 4;
            else if (opc == 2)
                element = 8;
            else
                return 0;
            if (mode != 2 && rn != 31 && (rn == rt || rn == rt2))
                return 0;
        }
        bytes = 2 * element;
        if (!add_scaled(base, imm, element == 8 ? 3 : 2, 1, &ignored))
            return 0;
        address = mode == 1 ? base : ignored;
    } else {
        return 0;
    }
    if (!!(esr & (1u << 6)) == !!load || (address & (element - 1)) ||
        address < start || bytes > length || address - start > length - bytes ||
        address > UINTPTR_MAX - (bytes - 1) ||
        fault < address || fault - address >= bytes ||
        address - start > UINT32_MAX)
        return 0;
    *offset = (uint32_t)(address - start);
    return 1;
}

static int synchronous_fault(int sig, const siginfo_t *si)
{
    if (!si)
        return 0;
    if (sig == SIGSEGV)
        return si->si_code == SEGV_MAPERR || si->si_code == SEGV_ACCERR;
    if (sig == SIGBUS)
        return si->si_code == BUS_ADRALN || si->si_code == BUS_ADRERR ||
               si->si_code == BUS_OBJERR;
    return 0;
}

/* Calling an earlier handler directly cannot reproduce an alternate signal
 * stack, one-shot disposition, restart behavior, explicit mask or recursive
 * delivery. Refuse those configurations instead of silently changing them. */
static int previous_handler_supported(const struct sigaction *previous)
{
    int sig;
    if (previous->sa_handler == SIG_DFL || previous->sa_handler == SIG_IGN)
        return 1;
    if (previous->sa_flags & (SA_ONSTACK | SA_RESETHAND | SA_NODEFER | SA_RESTART))
        return 0;
    for (sig = 1; sig < NSIG; sig++)
        if (sigismember(&previous->sa_mask, sig) > 0)
            return 0;
    return 1;
}

static void forward_fault(int sig, siginfo_t *si, void *ucv)
{
    const struct sigaction *previous =
        sig == SIGBUS ? &s_previous_bus : &s_previous_segv;

    if (previous->sa_handler == SIG_IGN && !synchronous_fault(sig, si))
        return;
    if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) {
        if (previous->sa_flags & SA_SIGINFO)
            previous->sa_sigaction(sig, si, ucv);
        else
            previous->sa_handler(sig);
        return;
    }

    if (previous->sa_handler == SIG_IGN) {
        sigaction(sig, &s_default, NULL);
    } else {
        sigaction(sig, previous, NULL);
    }
    if (!synchronous_fault(sig, si))
        raise(sig);
    /* A synchronous instruction is retried under the prior disposition. */
}

static void release_deferred_resources(void)
{
    for (unsigned i = 0; i < BW_DEFERRED_SLOTS; i++) {
        if (s_deferred[i].stack_mapping)
            munmap(s_deferred[i].stack_mapping,
                   BW_DEFERRED_STACK_SIZE + 2 * s_page_size);
        memset(&s_deferred[i], 0, sizeof(s_deferred[i]));
    }
    if (s_restore_page)
        munmap((void *)s_restore_page, s_page_size);
    s_restore_page = NULL;
    s_page_size = 0;
}

static int allocate_deferred_resources(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || (uint64_t)page > SIZE_MAX ||
        BW_DEFERRED_STACK_SIZE > SIZE_MAX - 2u * (size_t)page) {
        errno = EOVERFLOW;
        return -1;
    }
    s_page_size = (size_t)page;
    s_restore_page = mmap(NULL, s_page_size, PROT_NONE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s_restore_page == MAP_FAILED) {
        s_restore_page = NULL;
        return -1;
    }
    for (unsigned i = 0; i < BW_DEFERRED_SLOTS; i++) {
        size_t bytes = BW_DEFERRED_STACK_SIZE + 2 * s_page_size;
        s_deferred[i].stack_mapping =
            mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (s_deferred[i].stack_mapping == MAP_FAILED) {
            s_deferred[i].stack_mapping = NULL;
            release_deferred_resources();
            return -1;
        }
        s_deferred[i].stack = s_deferred[i].stack_mapping + s_page_size;
        if (mprotect(s_deferred[i].stack, BW_DEFERRED_STACK_SIZE,
                     PROT_READ | PROT_WRITE) != 0) {
            release_deferred_resources();
            return -1;
        }
    }
    return 0;
}

static BwDeferredFault *stack_deferred(const ucontext_t *context)
{
    uintptr_t sp;
    if (!context || !context->uc_mcontext)
        return NULL;
    sp = A64_CTX_SP(context);
    for (unsigned i = 0; i < BW_DEFERRED_SLOTS; i++) {
        uintptr_t start = (uintptr_t)s_deferred[i].stack;
        if (__atomic_load_n(&s_deferred[i].owner, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&s_deferred[i].active, __ATOMIC_ACQUIRE) &&
            sp >= start && sp <= start + BW_DEFERRED_STACK_SIZE)
            return &s_deferred[i];
    }
    return NULL;
}

static BwDeferredFault *reserve_deferred(uintptr_t thread_id)
{
    /* Keep a stack bound to one thread until uninstall: the restore handler
     * returns before the kernel has finished consuming its signal frame. */
    for (unsigned i = 0; i < BW_DEFERRED_SLOTS; i++)
        if (__atomic_load_n(&s_deferred[i].owner, __ATOMIC_ACQUIRE) ==
            thread_id)
            return &s_deferred[i];
    for (unsigned i = 0; i < BW_DEFERRED_SLOTS; i++) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&s_deferred[i].owner, &expected,
                                        thread_id, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return &s_deferred[i];
    }
    return NULL;
}

static void deferred_dispatch(BwDeferredFault *deferred)
    __attribute__((noreturn, noinline));

static void deferred_dispatch(BwDeferredFault *deferred)
{
    static const char dispatch_failed[] =
        "[BW] deferred APU MMIO dispatch failed\n";
    static const char state_failed[] =
        "[BW] deferred APU MMIO thread-state operation failed\n";
    sigset_t faults;
    int cancel_state;

    if (!deferred ||
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state) != 0) {
        (void)write(STDERR_FILENO, state_failed, sizeof(state_failed) - 1);
        _exit(84);
    }
    if (!xbox_PosixMmioFault(&deferred->context, deferred->access_start)) {
        (void)write(STDERR_FILENO, dispatch_failed, sizeof(dispatch_failed) - 1);
        _exit(85);
    }
    if (pthread_setcancelstate(cancel_state, NULL) != 0) {
        (void)write(STDERR_FILENO, state_failed, sizeof(state_failed) - 1);
        _exit(84);
    }
    sigemptyset(&faults);
    sigaddset(&faults, SIGSEGV);
    sigaddset(&faults, SIGBUS);
    if (pthread_sigmask(SIG_UNBLOCK, &faults, NULL) != 0) {
        (void)write(STDERR_FILENO, state_failed, sizeof(state_failed) - 1);
        _exit(84);
    }
    errno = deferred->saved_errno;
    bw_posix_restore_trap(s_restore_page);
}

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    int saved_errno = errno;
    uintptr_t base = __atomic_load_n(&s_guest_base, __ATOMIC_ACQUIRE);
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
    uintptr_t thread_id;
    unsigned active_slot;
    BwDeferredFault *deferred;

    /* A nested synchronous fault must terminate rather than repeatedly
     * fault with SIGSEGV/SIGBUS blocked. The Apple thread register is a
     * stable per-thread identity; the fixed lock-free table avoids first-use
     * TLS allocation from inside a handler. */
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(thread_id));
    if (!thread_id)
        _exit(128 + sig);
    for (active_slot = 0;
         active_slot < sizeof(s_active) / sizeof(s_active[0]); active_slot++)
        if (__atomic_load_n(&s_active[active_slot], __ATOMIC_ACQUIRE) == thread_id)
            _exit(128 + sig);
    for (active_slot = 0;
         active_slot < sizeof(s_active) / sizeof(s_active[0]); active_slot++) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&s_active[active_slot], &expected, thread_id,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
    }
    if (active_slot == sizeof(s_active) / sizeof(s_active[0]))
        _exit(128 + sig);

    deferred = stack_deferred((ucontext_t *)ucv);
    if (deferred) {
        ucontext_t *context = ucv;
        if (!context || !context->uc_mcontext ||
            fault != (uintptr_t)s_restore_page ||
            A64_CTX_PC(context) != (uintptr_t)bw_posix_restore_trap)
            _exit(128 + sig);
        copy_signal_bytes(context->uc_mcontext, &deferred->machine,
                          sizeof(deferred->machine));
        context->uc_sigmask = deferred->context.uc_sigmask;
        saved_errno = deferred->saved_errno;
        __atomic_store_n(&deferred->active, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s_active[active_slot], 0, __ATOMIC_RELEASE);
        errno = saved_errno;
        return;
    }

    if (synchronous_fault(sig, si) && base && fault >= base &&
        (uint64_t)(fault - base) < 0x100000000ull &&
        xbox_Ac97PosixFault(ucv, fault)) {
        __atomic_store_n(&s_active[active_slot], 0, __ATOMIC_RELEASE);
        errno = saved_errno;
        return;
    }

    if (synchronous_fault(sig, si) &&
        __atomic_load_n(&s_apu_enabled, __ATOMIC_ACQUIRE) && base &&
        fault >= base + BW_APU_BASE &&
        fault - (base + BW_APU_BASE) < BW_APU_TRAP_BYTES) {
        ucontext_t *context = ucv;
        uint32_t offset;
        uintptr_t start = base + BW_APU_BASE;
        if (checked_span(context, fault, start, BW_APU_TRAP_BYTES, &offset)) {
            deferred = reserve_deferred(thread_id);
            if (!deferred) {
                static const char exhausted[] =
                    "[BW] deferred APU MMIO slot pool exhausted\n";
                (void)write(STDERR_FILENO, exhausted, sizeof(exhausted) - 1);
                _exit(86);
            }
            if (__atomic_load_n(&deferred->active, __ATOMIC_ACQUIRE)) {
                static const char invalid_state[] =
                    "[BW] deferred APU MMIO slot is already active\n";
                (void)write(STDERR_FILENO, invalid_state,
                            sizeof(invalid_state) - 1);
                _exit(128 + sig);
            }
            copy_signal_bytes(&deferred->context, context,
                              sizeof(deferred->context));
            copy_signal_bytes(&deferred->machine, context->uc_mcontext,
                              sizeof(deferred->machine));
            deferred->context.uc_mcontext = &deferred->machine;
            deferred->access_start = start + offset;
            deferred->saved_errno = saved_errno;
            __atomic_store_n(&deferred->active, 1, __ATOMIC_RELEASE);
            A64_CTX_X(context, 0) = (uintptr_t)deferred;
            A64_CTX_SP(context) =
                ((uintptr_t)deferred->stack + BW_DEFERRED_STACK_SIZE) &
                ~(uintptr_t)15;
            A64_CTX_PC(context) = (uintptr_t)deferred_dispatch;
            __atomic_store_n(&s_active[active_slot], 0, __ATOMIC_RELEASE);
            errno = saved_errno;
            return;
        }
    }

    forward_fault(sig, si, ucv);
    __atomic_store_n(&s_active[active_slot], 0, __ATOMIC_RELEASE);
    errno = saved_errno;
}

int bw_posix_fault_install(void)
{
    struct sigaction sa;
    if (s_installed)
        return 0;
    if (allocate_deferred_resources() != 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &s_previous_bus) != 0) {
        release_deferred_resources();
        return -1;
    }
    if (!previous_handler_supported(&s_previous_bus)) {
        sigaction(SIGBUS, &s_previous_bus, NULL);
        release_deferred_resources();
        errno = ENOTSUP;
        return -1;
    }
    if (sigaction(SIGSEGV, &sa, &s_previous_segv) != 0) {
        sigaction(SIGBUS, &s_previous_bus, NULL);
        release_deferred_resources();
        return -1;
    }
    if (!previous_handler_supported(&s_previous_segv)) {
        sigaction(SIGSEGV, &s_previous_segv, NULL);
        sigaction(SIGBUS, &s_previous_bus, NULL);
        release_deferred_resources();
        errno = ENOTSUP;
        return -1;
    }
    s_installed = 1;
    return 0;
}

void bw_posix_fault_set_base(uintptr_t base)
{
    __atomic_store_n(&s_guest_base, base, __ATOMIC_RELEASE);
}

void bw_posix_fault_enable_apu(int enabled)
{
    __atomic_store_n(&s_apu_enabled, enabled != 0, __ATOMIC_RELEASE);
}

void bw_posix_fault_uninstall(void)
{
    if (!s_installed)
        return;
    __atomic_store_n(&s_apu_enabled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_guest_base, 0, __ATOMIC_RELEASE);
    sigaction(SIGBUS, &s_previous_bus, NULL);
    sigaction(SIGSEGV, &s_previous_segv, NULL);
    release_deferred_resources();
    s_installed = 0;
}

#else
int bw_posix_fault_install(void) { errno = ENOTSUP; return -1; }
void bw_posix_fault_set_base(uintptr_t base) { (void)base; }
void bw_posix_fault_enable_apu(int enabled) { (void)enabled; }
void bw_posix_fault_uninstall(void) {}
#endif
