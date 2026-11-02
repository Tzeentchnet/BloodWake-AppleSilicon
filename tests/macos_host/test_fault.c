#define _DARWIN_C_SOURCE 1
#include "bw_posix_fault.h"
#include "mmio_decode_a64.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__)

static uint8_t *s_guest;
static volatile uint32_t *s_mmio;
static uint8_t s_registers[0x4000];
static uint8_t s_apu_registers[0x30000];
static pthread_mutex_t s_apu_lock;
static _Atomic unsigned s_apu_calls;
static volatile sig_atomic_t s_decline, s_nested, s_forwarded;
static _Thread_local int s_tls_count;

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define BW_FAULT_TEST_TSAN 1
#endif
#endif
#ifndef BW_FAULT_TEST_TSAN
#define BW_FAULT_TEST_TSAN 0
#endif

enum { NESTED_MASK_BLOCKED = 79 };

static int nested_fault_signals_unblocked(void)
{
    sigset_t mask;
    int segv, bus;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) != 0)
        return -1;
    segv = sigismember(&mask, SIGSEGV);
    bus = sigismember(&mask, SIGBUS);
    return segv < 0 || bus < 0 ? -1 : !segv && !bus;
}

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
    if (s_nested) {
        int unblocked = nested_fault_signals_unblocked();
        if (unblocked != 1)
            _exit(unblocked == 0 ? NESTED_MASK_BLOCKED : 78);
        *(volatile uint32_t *)(s_guest + 0xfe800100u) = 1;
    }
    if (s_decline)
        return 0;
    return mmio_emulate_a64((ucontext_t *)ucv,
                            (uint32_t)(fault - (uintptr_t)s_mmio),
                            s_registers, read_register, write_register);
}

static uint64_t read_apu_register(void *dev, uint32_t off, int size)
{
    uint64_t value = 0;
    const uint8_t *registers = dev;
    if (off <= sizeof(s_apu_registers) &&
        (size_t)size <= sizeof(s_apu_registers) - off)
        memcpy(&value, registers + off, (size_t)size);
    return value;
}

static void write_apu_register(void *dev, uint32_t off, uint64_t value, int size)
{
    uint8_t *registers = dev;
    if (off <= sizeof(s_apu_registers) &&
        (size_t)size <= sizeof(s_apu_registers) - off)
        memcpy(registers + off, &value, (size_t)size);
}

int xbox_PosixMmioFault(void *ucv, uintptr_t access)
{
    uintptr_t start = (uintptr_t)s_guest + 0xfe800000u;
    int handled;
    if (!ucv || access < start || access - start >= sizeof(s_apu_registers))
        return 0;
    if (pthread_mutex_lock(&s_apu_lock) != 0)
        _exit(91);
    errno = ENOTTY;
    handled = mmio_emulate_a64((ucontext_t *)ucv, (uint32_t)(access - start),
                               s_apu_registers,
                               read_apu_register, write_apu_register);
    atomic_fetch_add_explicit(&s_apu_calls, 1, memory_order_relaxed);
    if (pthread_mutex_unlock(&s_apu_lock) != 0)
        _exit(91);
    return handled;
}

static void previous_handler(int sig, siginfo_t *si, void *ucv)
{
    (void)sig;
    (void)si;
    s_forwarded++;
    A64_CTX_PC((ucontext_t *)ucv) += 4;
}

static int wait_child_bounded(pid_t pid, int *status, long timeout_ms)
{
    struct timespec deadline, now, delay = {0, 10000000};
    int result = 0;
    pid_t waited;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        perror("clock_gettime");
        result = -1;
        goto terminate;
    }
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    for (;;) {
        waited = waitpid(pid, status, WNOHANG);
        if (waited == pid)
            return 1;
        if (waited < 0 && errno != EINTR) {
            perror("waitpid");
            return -1;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            perror("clock_gettime");
            result = -1;
            break;
        }
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            break;
        nanosleep(&delay, NULL);
    }

terminate:
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        perror("kill child");
        return -1;
    }
    do {
        waited = waitpid(pid, status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid) {
        perror("reap child");
        return -1;
    }
    return result;
}

static int child_fails_closed(int nested)
{
    pid_t pid = fork();
    int status, waited;
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
    if (pid < 0)
        return 0;
    waited = wait_child_bounded(pid, &status, 6000);
    if (waited != 1) {
        fprintf(stderr, "fail-closed child nested=%d %s\n", nested,
                waited == 0 ? "timed out" : "could not be reaped");
        return 0;
    }
    if (nested && WIFEXITED(status) && WEXITSTATUS(status) == NESTED_MASK_BLOCKED) {
        fprintf(stderr, "nested fault unavailable: SIGBUS or SIGSEGV is blocked inside the handler\n");
        return 2;
    }
    if ((WIFSIGNALED(status) &&
         (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)) ||
        (nested && WIFEXITED(status) &&
         (WEXITSTATUS(status) == 128 + SIGSEGV ||
          WEXITSTATUS(status) == 128 + SIGBUS)))
        return 1;
    fprintf(stderr, "fail-closed child nested=%d status=%d\n", nested, status);
    return 0;
}

static int child_timeout_is_reaped(void)
{
    pid_t pid = fork();
    int status, waited;
    if (pid == 0) {
        for (;;)
            pause();
    }
    if (pid < 0)
        return 0;
    waited = wait_child_bounded(pid, &status, 100);
    return waited == 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
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

static void *apu_fault_thread(void *arg)
{
    uintptr_t n = (uintptr_t)arg;
    volatile uint32_t *p =
        (volatile uint32_t *)(s_guest + 0xfe800100u + n * sizeof(uint32_t));
    for (unsigned i = 0; i < 100; i++) {
        *p = (uint32_t)(n * 1000 + i);
        if (*p != n * 1000 + i)
            return (void *)1;
    }
    return NULL;
}

static int apu_failure(const char *reason)
{
    fprintf(stderr, "deferred APU fault: %s\n", reason);
    return 0;
}

static int test_deferred_apu(void)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(s_guest + 0xfe800100u);
    uint64_t expected[2] = {
        UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)
    };
    uint64_t got[2] = {0, 0};
    uint32_t value = 0, flags = 0;
    sigset_t blocked, before, after;
    unsigned calls;
    int fault_errno;
    sig_atomic_t forwarded;

    bw_posix_fault_enable_apu(1);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &before) != 0)
        return apu_failure("cannot block SIGUSR1");
    errno = EDOM;
    if (pthread_mutex_lock(&s_apu_lock) != 0)
        return apu_failure("cannot lock recursive model mutex");
    __asm__ volatile(
        "ldr q31, [%[expected]]\n\t"
        "cmp wzr, wzr\n\t"
        "str %w[input], [%[reg]]\n\t"
        "ldr %w[value], [%[reg]]\n\t"
        "cset %w[flags], eq\n\t"
        "str q31, [%[got]]"
        : [value] "=&r"(value), [flags] "=&r"(flags)
        : [reg] "r"(reg), [input] "r"(UINT32_C(0x12345678)),
          [expected] "r"(expected), [got] "r"(got)
        : "v31", "cc", "memory");
    fault_errno = errno;
    if (pthread_mutex_unlock(&s_apu_lock) != 0 ||
        pthread_sigmask(SIG_SETMASK, NULL, &after) != 0 ||
        value != UINT32_C(0x12345678) || flags != 1 ||
        memcmp(expected, got, sizeof(got)) != 0 ||
        (!BW_FAULT_TEST_TSAN && fault_errno != EDOM) ||
        sigismember(&after, SIGUSR1) != 1) {
        fprintf(stderr,
                "deferred APU state: value=%08x flags=%u vector=%d errno=%d mask=%d\n",
                value, flags, memcmp(expected, got, sizeof(got)), fault_errno,
                sigismember(&after, SIGUSR1));
        pthread_sigmask(SIG_SETMASK, &before, NULL);
        return apu_failure("machine state was not preserved");
    }
    if (pthread_sigmask(SIG_SETMASK, &before, NULL) != 0)
        return apu_failure("cannot restore signal mask");
    calls = atomic_load_explicit(&s_apu_calls, memory_order_relaxed);
    if (calls != 2)
        return apu_failure("dispatcher call count is not two");

    forwarded = s_forwarded;
    bw_posix_fault_enable_apu(0);
    *reg = 0xdeadbeefu;
    if (s_forwarded != forwarded + 1 ||
        atomic_load_explicit(&s_apu_calls, memory_order_relaxed) != calls)
        return apu_failure("disabled APU fault was not forwarded");
    bw_posix_fault_enable_apu(1);
    return 1;
}

static int test_concurrent_apu(void)
{
    pthread_t threads[8];
    unsigned before =
        atomic_load_explicit(&s_apu_calls, memory_order_relaxed);
    int created = 0, failures = 0;

    bw_posix_fault_enable_apu(1);
    for (; created < 8; created++)
        if (pthread_create(&threads[created], NULL, apu_fault_thread,
                           (void *)(uintptr_t)created)) {
            failures++;
            break;
        }
    for (int i = 0; i < created; i++) {
        void *rc;
        if (pthread_join(threads[i], &rc) != 0 || rc)
            failures++;
    }
    if (atomic_load_explicit(&s_apu_calls, memory_order_relaxed) !=
        before + (unsigned)created * 200) {
        fprintf(stderr,
                "deferred APU fault: concurrent call count is %u, expected %u\n",
                atomic_load_explicit(&s_apu_calls, memory_order_relaxed),
                before + (unsigned)created * 200);
        failures++;
    }
    return failures == 0;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    uint64_t expected[2] = { 0x0123456789abcdefull, 0xfedcba9876543210ull };
    uint64_t got[2] = { 0, 0 }, flags = 0;
    uint32_t value = 0;
    pthread_t threads[8];
    pthread_mutexattr_t recursive;
    int failures = 0;
    sigset_t mask, after, fault_mask, previous_mask;
    int nested_only = argc == 2 && !strcmp(argv[1], "--nested-only");
    int non_nested_only = argc == 2 && !strcmp(argv[1], "--non-nested");
    int apu_only = argc == 2 && !strcmp(argv[1], "--apu-only");
    int apu_stress_only =
        argc == 2 && !strcmp(argv[1], "--apu-stress-only");

    if (argc != 1 && !nested_only && !non_nested_only && !apu_only &&
        !apu_stress_only) {
        fprintf(stderr,
                "usage: %s [--nested-only|--non-nested|--apu-only|"
                "--apu-stress-only]\n", argv[0]);
        return 1;
    }

    s_guest = mmap(NULL, 0x100000000ull, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s_guest == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    s_mmio = (volatile uint32_t *)(s_guest + 0xfec00000u);
    if (pthread_mutexattr_init(&recursive) != 0 ||
        pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE) != 0 ||
        pthread_mutex_init(&s_apu_lock, &recursive) != 0)
        return 1;
    pthread_mutexattr_destroy(&recursive);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = previous_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) || sigaction(SIGBUS, &sa, NULL))
        return 1;
    if (bw_posix_fault_install()) {
        perror("install");
        return 1;
    }
    bw_posix_fault_set_base((uintptr_t)s_guest);
    if (apu_only || apu_stress_only) {
        int result = apu_only ? test_deferred_apu() : test_concurrent_apu();
        bw_posix_fault_uninstall();
        pthread_mutex_destroy(&s_apu_lock);
        munmap(s_guest, 0x100000000ull);
        return result ? 0 : 1;
    }
    if (nested_only) {
        int result = child_fails_closed(1);
        bw_posix_fault_uninstall();
        if (munmap(s_guest, 0x100000000ull) != 0) {
            perror("munmap");
            return 1;
        }
        if (result == 2)
            return BW_FAULT_TEST_TSAN ? 77 : 1;
        return result == 1 ? 0 : 1;
    }
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

    sigemptyset(&fault_mask);
    sigaddset(&fault_mask, SIGSEGV);
    if (pthread_sigmask(SIG_BLOCK, &fault_mask, &previous_mask) != 0) {
        failures++;
    } else {
        if (nested_fault_signals_unblocked() != 0)
            failures++;
        if (pthread_sigmask(SIG_SETMASK, &previous_mask, NULL) != 0)
            failures++;
    }

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
    if (!test_deferred_apu())
        failures++;

    if (!child_fails_closed(0) ||
        (!non_nested_only && child_fails_closed(1) != 1))
        failures++;
    if (!child_timeout_is_reaped())
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
    if (!test_concurrent_apu())
        failures++;

    bw_posix_fault_uninstall();
    pthread_mutex_destroy(&s_apu_lock);
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
