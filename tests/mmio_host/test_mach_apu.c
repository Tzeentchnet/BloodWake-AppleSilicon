/*
 * A deliberately bounded Mach exception experiment, not a production bridge.
 * The server executes real APU code on a different thread. Each scenario runs
 * in a child because a deadlocked or nested exception must not strand tests.
 */
#include "apu.h"
#include "apu_state.h"
#include "mach_exc.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GUEST_SIZE (UINT64_C(1) << 32)
#define RAM_SIZE (64u * 1024u * 1024u)
#define APU_BASE 0xFE800000u
#define DSP_START 0xFE830000u
#define DSP_SIZE 0x50000u

enum { MODE_READ, MODE_LOCKED, MODE_NESTED, MODE_UNCOVERED };
enum { EXIT_LOCKED = 41, EXIT_NESTED = 42, EXIT_UNCOVERED = 43, EXIT_BAD = 44 };

void test_set_guest_base(uint8_t *base);
extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

static MCPXAPUState *s_apu;
static uint8_t *s_guest;
static int s_mode;
static int s_notify_fd = -1;
static _Atomic int s_in_dispatch;

kern_return_t catch_mach_exception_raise(mach_port_t port, mach_port_t thread,
        mach_port_t task, exception_type_t exception,
        mach_exception_data_t code, mach_msg_type_number_t count)
{
    (void)port; (void)thread; (void)task; (void)exception; (void)code; (void)count;
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state(mach_port_t port,
        exception_type_t exception, const mach_exception_data_t code,
        mach_msg_type_number_t count, int *flavor, const thread_state_t old_state,
        mach_msg_type_number_t old_count, thread_state_t new_state,
        mach_msg_type_number_t *new_count)
{
    (void)port; (void)exception; (void)code; (void)count; (void)flavor;
    (void)old_state; (void)old_count; (void)new_state; (void)new_count;
    return KERN_FAILURE;
}

kern_return_t catch_mach_exception_raise_state_identity(mach_port_t port,
        mach_port_t thread, mach_port_t task, exception_type_t exception,
        mach_exception_data_t code, mach_msg_type_number_t count, int *flavor,
        thread_state_t old_state, mach_msg_type_number_t old_count,
        thread_state_t new_state, mach_msg_type_number_t *new_count)
{
    arm_thread_state64_t state;
    uintptr_t fault;
    uint32_t instruction, rt;
    (void)port; (void)thread; (void)task;

    if (exception != EXC_BAD_ACCESS || count < 2)
        _exit(EXIT_BAD);
    fault = (uintptr_t)code[1];
    if (atomic_exchange_explicit(&s_in_dispatch, 1, memory_order_acq_rel)) {
        if (s_mode == MODE_NESTED && fault == (uintptr_t)s_guest)
            _exit(EXIT_NESTED);
        _exit(EXIT_BAD);
    }
    if (s_mode == MODE_UNCOVERED)
        _exit(fault == (uintptr_t)(s_guest + APU_BASE + NV_PAPU_XGSCNT + 4)
              ? EXIT_UNCOVERED : EXIT_BAD);
    if (fault != (uintptr_t)(s_guest + APU_BASE + NV_PAPU_XGSCNT))
        _exit(EXIT_BAD);
    if (s_mode == MODE_LOCKED) {
        if (TryEnterCriticalSection(&s_apu->lock.cs)) {
            LeaveCriticalSection(&s_apu->lock.cs);
            _exit(EXIT_BAD);
        }
        if (write(s_notify_fd, "L", 1) != 1)
            _exit(EXIT_BAD);
        /* This real callback takes the lock owned by the suspended thread. */
        (void)mcpx_apu_mmio_read(s_apu, NV_PAPU_XGSCNT, 4);
        _exit(EXIT_BAD);
    }
    if (s_mode == MODE_NESTED) {
        /* FEMEMDATA writes through a guest-supplied physical address. Page
         * zero is inaccessible, so this faults inside the server callback. */
        mcpx_apu_mmio_write(s_apu, NV_PAPU_FEMEMDATA, 1, 4);
        _exit(EXIT_BAD);
    }
    if (*flavor != ARM_THREAD_STATE64 || old_count != ARM_THREAD_STATE64_COUNT
            || *new_count < ARM_THREAD_STATE64_COUNT)
        _exit(EXIT_BAD);
    memcpy(&state, old_state, sizeof(state));
    instruction = *(const uint32_t *)(uintptr_t)state.__pc;
    rt = instruction & 31u;
    if ((instruction & 0xFFC00000u) != 0xB9400000u || rt >= 29)
        _exit(EXIT_UNCOVERED);
    state.__x[rt] = (uint32_t)mcpx_apu_mmio_read(s_apu, NV_PAPU_XGSCNT, 4);
    state.__pc += 4;
    memcpy(new_state, &state, sizeof(state));
    *new_count = ARM_THREAD_STATE64_COUNT;
    atomic_store_explicit(&s_in_dispatch, 0, memory_order_release);
    return KERN_SUCCESS;
}

static void *serve_exceptions(void *port)
{
    mach_port_t name = (mach_port_t)(uintptr_t)port;
    (void)mach_msg_server(mach_exc_server, 16384, name, 0);
    _exit(EXIT_BAD);
}

static void run_child(int mode)
{
    mach_port_t port;
    pthread_t workers[2];
    uint64_t before[2] = {UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)};
    uint64_t after[2] = {0, 0};
    uint32_t result, flags;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    volatile uint32_t *reg;

    s_mode = mode;
    s_guest = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s_guest == MAP_FAILED ||
            mprotect(s_guest, RAM_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(s_guest + 0x80000000u, RAM_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(s_guest + DSP_START, DSP_SIZE, PROT_READ | PROT_WRITE))
        _exit(EXIT_BAD);
    test_set_guest_base(s_guest);
    s_apu = mcpx_apu_init_standalone(s_guest);
    if (!s_apu)
        _exit(EXIT_BAD);
    mcpx_apu_mmio_write(s_apu, NV_PAPU_XGSCNT, 0x1234, 4);
    mcpx_apu_mmio_write(s_apu, NV_PAPU_FEMEMADDR, 0, 4);
    if (mprotect(s_guest, page, PROT_NONE))
        _exit(EXIT_BAD);
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
            mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
        _exit(EXIT_BAD);
    if (task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, port,
            EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64) != KERN_SUCCESS)
        _exit(77);
    for (int i = 0; i < 2; i++)
        if (pthread_create(&workers[i], NULL, serve_exceptions,
                           (void *)(uintptr_t)port) != 0)
            _exit(EXIT_BAD);
    if (mode == MODE_LOCKED)
        qemu_mutex_lock(&s_apu->lock);
    reg = (volatile uint32_t *)(s_guest + APU_BASE + NV_PAPU_XGSCNT +
                                (mode == MODE_UNCOVERED ? 4 : 0));
    __asm__ volatile(
        "ldr q31, [%[before]]\n\t"
        "cmp wzr, wzr\n\t"
        "ldr %w[result], [%[reg]]\n\t"
        "cset %w[flags], eq\n\t"
        "str q31, [%[after]]"
        : [result] "=&r"(result), [flags] "=&r"(flags)
        : [reg] "r"(reg), [before] "r"(before), [after] "r"(after)
        : "v31", "cc", "memory");
    if (mode != MODE_READ || result != 0x1234 || flags != 1 ||
            memcmp(before, after, sizeof(before)) != 0)
        _exit(EXIT_BAD);
    _exit(0);
}

static int check_child(int mode, int expected)
{
    struct timespec delay = {0, 10000000};
    int pipefd[2], notified = 0, after_notify = 0;
    int status = 0;
    pid_t pid;

    if (pipe(pipefd))
        return 0;
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pipefd[0]);
        s_notify_fd = pipefd[1];
        run_child(mode);
    }
    close(pipefd[1]);
    for (int i = 0; i < 1000; i++) {
        char byte;
        if (read(pipefd[0], &byte, 1) == 1 && byte == 'L')
            notified = 1;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            close(pipefd[0]);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 77)
                return -1;
            if (mode != MODE_LOCKED && WIFEXITED(status) && WEXITSTATUS(status) == expected)
                return 1;
            fprintf(stderr, "Mach/APU probe mode %d: unexpected status %d\n", mode, status);
            return 0;
        }
        if (result < 0) {
            close(pipefd[0]);
            return 0;
        }
        if (mode == MODE_LOCKED && notified && ++after_notify == 50) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(pipefd[0]);
            return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        }
        nanosleep(&delay, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    close(pipefd[0]);
    fprintf(stderr, "Mach/APU probe mode %d timed out\n", mode);
    return 0;
}

int main(void)
{
    static const int expected[] = {0, EXIT_LOCKED, EXIT_NESTED, EXIT_UNCOVERED};
    for (int mode = MODE_READ; mode <= MODE_UNCOVERED; mode++) {
        int ok = check_child(mode, expected[mode]);
        if (ok < 0) {
            fprintf(stderr, "Mach exception ports unavailable for this process\n");
            return 77;
        }
        if (!ok)
            return 1;
    }
    return 0;
}
