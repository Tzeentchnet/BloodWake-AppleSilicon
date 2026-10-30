#define _DARWIN_C_SOURCE 1
#include "bw_posix_fault.h"
#include "mmio_decode_a64.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)

static uint8_t *s_guest;
static volatile uint32_t *s_mmio;
static uint8_t s_registers[0x4000];
static volatile sig_atomic_t s_decline, s_nested, s_forwarded;
static _Thread_local int s_tls_count;

static uint64_t read_register(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    const volatile uint8_t *p = dev;
    if (off + (unsigned)size <= sizeof(s_registers))
        for (int i = 0; i < size; i++)
            v |= (uint64_t)p[off + (unsigned)i] << (8 * i);
    return v;
}

static void write_register(void *dev, uint32_t off, uint64_t value, int size)
{
    volatile uint8_t *p = dev;
    if (off + (unsigned)size <= sizeof(s_registers))
        for (int i = 0; i < size; i++)
            p[off + (unsigned)i] = (uint8_t)(value >> (8 * i));
}

/* A signal-safe synthetic AC'97 decoder. The real device alias and its
 * concurrent fault behavior are tested by tests/mmio_host. */
int xbox_Ac97PosixFault(void *ucv, uintptr_t fault)
{
    if (fault < (uintptr_t)s_mmio ||
        fault - (uintptr_t)s_mmio >= sizeof(s_registers))
        return 0;
    s_tls_count++;
    errno = EIO;
    if (s_nested)
        *(volatile uint32_t *)(s_guest + 0xfe800100u) = 1;
    if (s_decline)
        return 0;
    return mmio_emulate_a64((ucontext_t *)ucv,
                            (uint32_t)(fault - (uintptr_t)s_mmio),
                            s_registers, read_register, write_register);
}

static void previous_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    s_forwarded++;
    A64_CTX_PC((ucontext_t *)ucv) += 4;
}

static int child_fails_closed(int nested)
{
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        bw_posix_fault_uninstall();
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        if (bw_posix_fault_install() != 0)
            _exit(80);
        bw_posix_fault_set_base((uintptr_t)s_guest);
        s_decline = !nested;
        s_nested = nested;
        alarm(5);
        *s_mmio = 0x1234;
        _exit(81);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return 0;
    if ((WIFSIGNALED(status) &&
         (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) ||
        (nested && WIFEXITED(status) &&
         (WEXITSTATUS(status) == 128 + SIGSEGV ||
          WEXITSTATUS(status) == 128 + SIGBUS)))
        return 1;
    fprintf(stderr, "fail-closed child nested=%d status=%d\n", nested, status);
    return 0;
}

static void *fault_thread(void *arg)
{
    uintptr_t n = (uintptr_t)arg;
    volatile uint32_t *p = s_mmio + 16 + n;
    for (unsigned i = 0; i < 100; i++) {
        *p = (uint32_t)(n * 1000 + i);
        if (*p != n * 1000 + i)
            return (void *)1;
    }
    return s_tls_count == 200 ? NULL : (void *)1;
}

int main(void)
{
    struct sigaction sa;
    uint64_t expected[2] = { 0x0123456789abcdefull, 0xfedcba9876543210ull };
    uint64_t got[2] = { 0, 0 }, flags = 0;
    uint32_t value = 0;
    pthread_t threads[8];
    int failures = 0;
    sigset_t mask, after;

    s_guest = mmap(NULL, 0x100000000ull, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s_guest == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    s_mmio = (volatile uint32_t *)(s_guest + 0xfec00000u);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = previous_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) || sigaction(SIGBUS, &sa, NULL) ||
        bw_posix_fault_install()) {
        perror("install");
        return 1;
    }
    bw_posix_fault_set_base((uintptr_t)s_guest);
    *s_mmio = 0xaabbccddu;
    if (*s_mmio != 0xaabbccddu || s_tls_count != 2)
        failures++;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    errno = EDOM;
    value = *s_mmio;
    pthread_sigmask(SIG_SETMASK, NULL, &after);
    if (value != 0xaabbccddu || errno != EDOM ||
        !sigismember(&after, SIGUSR1))
        failures++;

    __asm__ volatile(
        "ldr q31, [%2]\n\t"
        "cmp %w3, %w3\n\t"
        "ldr %w0, [%4]\n\t"
        "mrs %1, nzcv\n\t"
        "str q31, [%5]\n\t"
        : "=&r"(value), "=&r"(flags)
        : "r"(expected), "r"(1), "r"(s_mmio), "r"(got)
        : "v31", "cc", "memory");
    if (value != 0xaabbccddu || memcmp(expected, got, sizeof(got)) ||
        (flags & 0xf0000000u) != 0x60000000u)
        failures++;

    /* APU/OHCI and declined instructions go to the previous handler. */
    *(volatile uint32_t *)(s_guest + 0xfe800100u) = 1;
    *(volatile uint32_t *)(s_guest + 0xfed00000u) = 1;
    s_decline = 1;
    *s_mmio = 1;
    s_decline = 0;
    if (s_forwarded != 3)
        failures++;

    if (!child_fails_closed(0) || !child_fails_closed(1))
        failures++;

    for (uintptr_t i = 0; i < 8; i++)
        if (pthread_create(&threads[i], NULL, fault_thread, (void *)i))
            return 1;
    for (int i = 0; i < 8; i++) {
        void *rc;
        pthread_join(threads[i], &rc);
        if (rc)
            failures++;
    }

    bw_posix_fault_uninstall();
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(SIGSEGV, &sa, NULL) != 0 ||
        bw_posix_fault_install() == 0 || errno != ENOTSUP)
        failures++;
    sa.sa_flags = SA_SIGINFO;
    sigaddset(&sa.sa_mask, SIGUSR1);
    if (sigaction(SIGSEGV, &sa, NULL) != 0 ||
        bw_posix_fault_install() == 0 || errno != ENOTSUP)
        failures++;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    munmap(s_guest, 0x100000000ull);
    if (failures)
        fprintf(stderr, "fault handling: %d failures\n", failures);
    else
        puts("fault handling: synthetic checks passed");
    return failures ? 1 : 0;
}

#else
int main(void) { return 77; }
#endif
