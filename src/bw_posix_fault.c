#define _DARWIN_C_SOURCE 1
#include "bw_posix_fault.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)

/* AC'97 has a bounded, signal-safe store decoder operating through a
 * writable shared alias. The APU and OHCI callbacks do not: they may take
 * locks or call host code. Never call xbox_PosixMmioFault from this handler. */
extern int xbox_Ac97PosixFault(void *ucv, uintptr_t fault);

static uintptr_t s_guest_base;
static struct sigaction s_previous_bus, s_previous_segv;
static const struct sigaction s_default = {.sa_handler = SIG_DFL};
static int s_installed;
static uintptr_t s_active[128];

_Static_assert(__atomic_always_lock_free(sizeof(uintptr_t), 0),
               "signal address atomics must be lock-free");

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

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    int saved_errno = errno;
    uintptr_t base = __atomic_load_n(&s_guest_base, __ATOMIC_ACQUIRE);
    uintptr_t fault = si ? (uintptr_t)si->si_addr : 0;
    uintptr_t thread_id;
    unsigned slot;

    /* A nested synchronous fault must terminate rather than repeatedly
     * fault with SIGSEGV/SIGBUS blocked. The Apple thread register is a
     * stable per-thread identity; the fixed lock-free table avoids first-use
     * TLS allocation from inside a handler. */
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(thread_id));
    if (!thread_id)
        _exit(128 + sig);
    for (slot = 0; slot < sizeof(s_active) / sizeof(s_active[0]); slot++)
        if (__atomic_load_n(&s_active[slot], __ATOMIC_ACQUIRE) == thread_id)
            _exit(128 + sig);
    for (slot = 0; slot < sizeof(s_active) / sizeof(s_active[0]); slot++) {
        uintptr_t expected = 0;
        if (__atomic_compare_exchange_n(&s_active[slot], &expected, thread_id,
                                        0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
    }
    if (slot == sizeof(s_active) / sizeof(s_active[0]))
        _exit(128 + sig);
    if (synchronous_fault(sig, si) && base && fault >= base &&
        (uint64_t)(fault - base) < 0x100000000ull &&
        xbox_Ac97PosixFault(ucv, fault)) {
        __atomic_store_n(&s_active[slot], 0, __ATOMIC_RELEASE);
        errno = saved_errno;
        return;
    }
    forward_fault(sig, si, ucv);
    __atomic_store_n(&s_active[slot], 0, __ATOMIC_RELEASE);
    errno = saved_errno;
}

int bw_posix_fault_install(void)
{
    struct sigaction sa;
    if (s_installed)
        return 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &s_previous_bus) != 0)
        return -1;
    if (!previous_handler_supported(&s_previous_bus)) {
        sigaction(SIGBUS, &s_previous_bus, NULL);
        errno = ENOTSUP;
        return -1;
    }
    if (sigaction(SIGSEGV, &sa, &s_previous_segv) != 0) {
        sigaction(SIGBUS, &s_previous_bus, NULL);
        return -1;
    }
    if (!previous_handler_supported(&s_previous_segv)) {
        sigaction(SIGSEGV, &s_previous_segv, NULL);
        sigaction(SIGBUS, &s_previous_bus, NULL);
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

void bw_posix_fault_uninstall(void)
{
    if (!s_installed)
        return;
    __atomic_store_n(&s_guest_base, 0, __ATOMIC_RELEASE);
    sigaction(SIGBUS, &s_previous_bus, NULL);
    sigaction(SIGSEGV, &s_previous_segv, NULL);
    s_installed = 0;
}

#else
int bw_posix_fault_install(void) { errno = ENOTSUP; return -1; }
void bw_posix_fault_set_base(uintptr_t base) { (void)base; }
void bw_posix_fault_uninstall(void) {}
#endif
