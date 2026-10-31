#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "mach_exc.h"
#include <errno.h>
#include <fcntl.h>
#include <mach/arm/thread_status.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
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

enum { EXIT_BAD = 80, EXIT_PRE_SUBMISSION = 81, EXIT_OWNERSHIP_FAILED = 82,
       EXIT_UNAVAILABLE = 77 };
enum { MODE_HANDOFF, MODE_LOCKED, MODE_ATOMIC_LOCKED, MODE_PENDING_SUBMIT,
       MODE_PENDING_REJECT, MODE_ARMING, MODE_ARMING_SELF_REJECT, MODE_ARMING_FAILURE };
enum { ARMING, GPU_OWNED, HANDOFF_IN_PROGRESS, CPU_OWNED, OWNERSHIP_FAILED };

extern boolean_t mach_exc_server(mach_msg_header_t *, mach_msg_header_t *);

static void *s_cpu, *s_tiled;
static size_t s_size;
static id<MTLCommandBuffer> s_command;
static id<MTLSharedEvent> s_event;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint s_faults;
static atomic_int s_owner, s_submitted;
static int s_mode, s_notify;
static mach_port_t s_submitter;

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
    (void)port; (void)thread; (void)task;
    uintptr_t fault = count >= 2 ? (uintptr_t)code[1] : 0;
    int cpu_fault = fault >= (uintptr_t)s_cpu && fault - (uintptr_t)s_cpu < s_size;
    int tiled_fault = fault >= (uintptr_t)s_tiled && fault - (uintptr_t)s_tiled < s_size;
    if (exception != EXC_BAD_ACCESS || (!cpu_fault && !tiled_fault) ||
        *flavor != ARM_THREAD_STATE64 ||
        old_count != ARM_THREAD_STATE64_COUNT || *new_count < old_count)
        _exit(EXIT_BAD);
    atomic_fetch_add_explicit(&s_faults, 1, memory_order_release);
    if ((s_mode == MODE_LOCKED || s_mode == MODE_PENDING_SUBMIT) &&
        write(s_notify, "L", 1) != 1)
        _exit(EXIT_BAD);
    if (s_mode == MODE_LOCKED) {
        if (pthread_mutex_lock(&s_lock) != 0)
            _exit(EXIT_BAD);
        _exit(EXIT_BAD);
    }
    const struct timespec delay = {0, 1000000};
    for (;;) {
        int owner = atomic_load_explicit(&s_owner, memory_order_acquire);
        if (owner == CPU_OWNED)
            break;
        if (owner == OWNERSHIP_FAILED)
            _exit(EXIT_OWNERSHIP_FAILED);
        if (owner == ARMING && s_mode == MODE_ARMING_SELF_REJECT && thread == s_submitter)
            _exit(EXIT_PRE_SUBMISSION);
        if (owner == GPU_OWNED &&
            atomic_compare_exchange_strong_explicit(&s_owner, &owner, HANDOFF_IN_PROGRESS,
                                                    memory_order_acq_rel, memory_order_acquire)) {
            if (s_mode == MODE_PENDING_REJECT && thread == s_submitter &&
                !atomic_load_explicit(&s_submitted, memory_order_acquire))
                _exit(EXIT_PRE_SUBMISSION);
            while (!atomic_load_explicit(&s_submitted, memory_order_acquire))
                nanosleep(&delay, NULL);
            @autoreleasepool {
                [s_command waitUntilCompleted];
                if (s_command.status != MTLCommandBufferStatusCompleted)
                    _exit(EXIT_BAD);
            }
            if (mprotect(s_cpu, s_size, PROT_READ | PROT_WRITE) != 0 ||
                mprotect(s_tiled, s_size, PROT_READ | PROT_WRITE) != 0)
                _exit(EXIT_BAD);
            atomic_store_explicit(&s_owner, CPU_OWNED, memory_order_release);
            break;
        }
        nanosleep(&delay, NULL);
    }
    memcpy(new_state, old_state, old_count * sizeof(natural_t));
    *new_count = old_count;
    return KERN_SUCCESS;
}

static void *exception_server(void *opaque)
{
    mach_port_t port = (mach_port_t)(uintptr_t)opaque;
    (void)mach_msg_server(mach_exc_server, 16384, port, 0);
    _exit(EXIT_BAD);
}

static void *release_gpu(void *opaque)
{
    (void)opaque;
    const struct timespec delay = {0, 1000000};
    while (atomic_load_explicit(&s_faults, memory_order_acquire) < 2)
        nanosleep(&delay, NULL);
    @autoreleasepool {
        s_event.signaledValue = 1;
    }
    return NULL;
}

static void *cpu_access(void *opaque)
{
    uintptr_t index = (uintptr_t)opaque;
    volatile uint32_t *pixel = (volatile uint32_t *)(index ? s_tiled : s_cpu) + index;
    if (*pixel != 0xFFFF0000u)
        _exit(EXIT_BAD);
    *pixel = 0xFF123450u + (uint32_t)index;
    return NULL;
}

static void run_child(int mode)
{
    long page_size = sysconf(_SC_PAGESIZE);
    size_t page;
    size_t bytes = 2560u * 480u;
    mach_vm_address_t alias = 0, tiled = 0;
    vm_prot_t current, maximum;
    mach_port_t port;
    pthread_t servers[2], threads[2], release;
    s_mode = mode;
    s_submitter = mach_thread_self();
    if (page_size <= 0)
        _exit(EXIT_BAD);
    page = (size_t)page_size;
    s_size = (bytes + page - 1) / page * page;
    s_cpu = mmap(NULL, s_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (s_cpu == MAP_FAILED ||
        mach_vm_remap(mach_task_self(), &alias, s_size, 0, VM_FLAGS_ANYWHERE,
                      mach_task_self(), (mach_vm_address_t)(uintptr_t)s_cpu,
                      FALSE, &current, &maximum, VM_INHERIT_NONE) != KERN_SUCCESS ||
        mach_vm_remap(mach_task_self(), &tiled, s_size, 0, VM_FLAGS_ANYWHERE,
                      mach_task_self(), (mach_vm_address_t)(uintptr_t)s_cpu,
                      FALSE, &current, &maximum, VM_INHERIT_NONE) != KERN_SUCCESS)
        _exit(EXIT_BAD);
    s_tiled = (void *)(uintptr_t)tiled;

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        s_event = [device newSharedEvent];
        id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)alias
                                                        length:s_size
                                                       options:MTLResourceStorageModeShared
                                                   deallocator:nil];
        MTLTextureDescriptor *desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                              width:640 height:480 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:2560];
        s_command = [queue commandBuffer];
        if (!device || !queue || !s_event || !buffer || !texture || !s_command)
            _exit(EXIT_BAD);
        [s_command encodeWaitForEvent:s_event value:1];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
        id<MTLRenderCommandEncoder> encoder = [s_command renderCommandEncoderWithDescriptor:pass];
        if (!encoder)
            _exit(EXIT_BAD);
        [encoder endEncoding];
        if (mode != MODE_PENDING_SUBMIT && mode != MODE_PENDING_REJECT) {
            [s_command commit];
            atomic_store_explicit(&s_submitted, 1, memory_order_release);
        }

        if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
            mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
            _exit(EXIT_BAD);
        if (task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, port,
                EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64) != KERN_SUCCESS)
            _exit(EXIT_UNAVAILABLE);
        for (unsigned i = 0; i < 2; i++)
            if (pthread_create(&servers[i], NULL, exception_server, (void *)(uintptr_t)port))
                _exit(EXIT_BAD);
        atomic_store_explicit(&s_owner, ARMING, memory_order_release);
        if (mprotect(s_cpu, s_size, PROT_NONE) != 0)
            _exit(EXIT_BAD);
        if (mode == MODE_ARMING_SELF_REJECT) {
            cpu_access(NULL);
            _exit(EXIT_BAD);
        }
        if (mode == MODE_ARMING_FAILURE) {
            const struct timespec delay = {0, 1000000};
            if (pthread_create(&threads[0], NULL, cpu_access, NULL))
                _exit(EXIT_BAD);
            while (!atomic_load_explicit(&s_faults, memory_order_acquire))
                nanosleep(&delay, NULL);
            atomic_store_explicit(&s_owner, OWNERSHIP_FAILED, memory_order_release);
            for (;;)
                nanosleep(&delay, NULL);
        }
        if (mode == MODE_ARMING) {
            const struct timespec delay = {0, 1000000};
            if (pthread_create(&threads[0], NULL, cpu_access, NULL))
                _exit(EXIT_BAD);
            while (!atomic_load_explicit(&s_faults, memory_order_acquire))
                nanosleep(&delay, NULL);
            *(volatile uint32_t *)s_tiled = 0xFFABCDEFu;
        }
        if (mprotect(s_tiled, s_size, PROT_NONE) != 0)
            _exit(EXIT_BAD);
        atomic_store_explicit(&s_owner, GPU_OWNED, memory_order_release);

        if (mode == MODE_PENDING_SUBMIT || mode == MODE_PENDING_REJECT) {
            cpu_access(NULL);
            _exit(EXIT_BAD);
        }
        if (mode == MODE_LOCKED || mode == MODE_ATOMIC_LOCKED) {
            s_event.signaledValue = 1;
            if (pthread_mutex_lock(&s_lock))
                _exit(EXIT_BAD);
            cpu_access(NULL);
            if (pthread_mutex_unlock(&s_lock) || mode != MODE_ATOMIC_LOCKED ||
                *(uint32_t *)(uintptr_t)alias != 0xFF123450u)
                _exit(EXIT_BAD);
            _exit(0);
        }
        if (pthread_create(&release, NULL, release_gpu, NULL))
            _exit(EXIT_BAD);
        for (uintptr_t i = mode == MODE_ARMING ? 1 : 0; i < 2; i++)
            if (pthread_create(&threads[i], NULL, cpu_access, (void *)i))
                _exit(EXIT_BAD);
        for (unsigned i = 0; i < 2; i++)
            if (pthread_join(threads[i], NULL))
                _exit(EXIT_BAD);
        if (pthread_join(release, NULL))
            _exit(EXIT_BAD);
        if (((uint32_t *)(uintptr_t)alias)[0] != 0xFF123450u ||
            ((uint32_t *)(uintptr_t)alias)[1] != 0xFF123451u)
            _exit(EXIT_BAD);
    }
    _exit(0);
}

static int check_child(int mode)
{
    int pipefd[2], status, notified = 0;
    unsigned after_notify = 0;
    if (pipe(pipefd) != 0)
        return 0;
    if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }
    if (!pid) {
        close(pipefd[0]);
        s_notify = pipefd[1];
        run_child(mode);
    }
    close(pipefd[1]);
    const struct timespec delay = {0, 10000000};
    for (unsigned i = 0; i < 1500; i++) {
        int expect_deadlock = mode == MODE_LOCKED || mode == MODE_PENDING_SUBMIT;
        if (expect_deadlock && !notified) {
            char byte;
            if (read(pipefd[0], &byte, 1) == 1 && byte == 'L')
                notified = 1;
        }
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            close(pipefd[0]);
            if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_UNAVAILABLE)
                return -1;
            int expected = mode == MODE_PENDING_REJECT || mode == MODE_ARMING_SELF_REJECT
                         ? EXIT_PRE_SUBMISSION
                         : mode == MODE_ARMING_FAILURE ? EXIT_OWNERSHIP_FAILED : 0;
            return !expect_deadlock && WIFEXITED(status) && WEXITSTATUS(status) == expected;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            close(pipefd[0]);
            return 0;
        }
        if (expect_deadlock && notified && ++after_notify >= 100) {
            if (kill(pid, SIGKILL) != 0) {
                close(pipefd[0]);
                return 0;
            }
            while (waitpid(pid, &status, 0) < 0) {
                if (errno != EINTR) {
                    close(pipefd[0]);
                    return 0;
                }
            }
            close(pipefd[0]);
            return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
        }
        nanosleep(&delay, NULL);
    }
    if (kill(pid, SIGKILL) == 0)
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    close(pipefd[0]);
    return 0;
}

int main(void)
{
    int handoff = check_child(MODE_HANDOFF);
    if (handoff < 0)
        return EXIT_UNAVAILABLE;
    if (!handoff) {
        fprintf(stderr, "Protected Metal handoff failed\n");
        return 1;
    }
    int atomic_locked = check_child(MODE_ATOMIC_LOCKED);
    if (atomic_locked < 0)
        return EXIT_UNAVAILABLE;
    if (!atomic_locked) {
        fprintf(stderr, "Atomic handoff failed with an unrelated lock held\n");
        return 1;
    }
    int arming = check_child(MODE_ARMING);
    if (arming < 0)
        return EXIT_UNAVAILABLE;
    if (!arming) {
        fprintf(stderr, "Handoff failed during sequential alias protection\n");
        return 1;
    }
    int self_rejected = check_child(MODE_ARMING_SELF_REJECT);
    if (self_rejected < 0)
        return EXIT_UNAVAILABLE;
    if (!self_rejected) {
        fprintf(stderr, "Submitter fault during alias protection was not rejected\n");
        return 1;
    }
    int failed = check_child(MODE_ARMING_FAILURE);
    if (failed < 0)
        return EXIT_UNAVAILABLE;
    if (!failed) {
        fprintf(stderr, "Alias-protection failure was not surfaced\n");
        return 1;
    }
    int locked = check_child(MODE_LOCKED);
    if (locked < 0)
        return EXIT_UNAVAILABLE;
    if (!locked) {
        fprintf(stderr, "Expected lock-ownership deadlock was not reproduced\n");
        return 1;
    }
    int pending = check_child(MODE_PENDING_SUBMIT);
    if (pending < 0)
        return EXIT_UNAVAILABLE;
    if (!pending) {
        fprintf(stderr, "Expected pre-submission deadlock was not reproduced\n");
        return 1;
    }
    int rejected = check_child(MODE_PENDING_REJECT);
    if (rejected < 0)
        return EXIT_UNAVAILABLE;
    if (!rejected) {
        fprintf(stderr, "Pre-submission fault was not rejected by submitter identity\n");
        return 1;
    }
    return 0;
}
