/* These tests pass when the documented counterexamples are reproduced. */
#include "../../src/toolkit_patches/kernel/xbox_memory_layout.c"
#include "../../src/toolkit_patches/kernel/kernel_memory.c"

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_mutex_t heap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static unsigned gate_step;
static uint32_t guest_va;
static uint8_t *bytes;
static uint8_t observed_first, observed_second, observed_byte;
static ULONG_PTR translated;
static int fault_notice[2], fault_resume[2];
static uintptr_t protected_start, protected_end;
static uint32_t worker_allocation;
static uint8_t *worker_write;

VOID AcquireSRWLockExclusive(PSRWLOCK lock)
{
    (void)lock;
    if (pthread_mutex_lock(&heap_mutex)) abort();
}
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)
{
    (void)lock;
    if (pthread_mutex_unlock(&heap_mutex)) abort();
}
VOID AcquireSRWLockShared(PSRWLOCK lock) { AcquireSRWLockExclusive(lock); }
VOID ReleaseSRWLockShared(PSRWLOCK lock) { ReleaseSRWLockExclusive(lock); }
LONG InterlockedCompareExchange(volatile LONG *target, LONG exchange, LONG compare)
{
    __atomic_compare_exchange_n(target, &compare, exchange, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return compare;
}
LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}
LONG InterlockedIncrement(volatile LONG *target)
{
    return __atomic_add_fetch(target, 1, __ATOMIC_SEQ_CST);
}
VOID SwitchToThread(void) { sched_yield(); }
DWORD GetTickCount(void) { return 1; }

static void step(unsigned value)
{
    if (pthread_mutex_lock(&gate_mutex)) abort();
    gate_step = value;
    if (pthread_cond_broadcast(&gate_cond)) abort();
    if (pthread_mutex_unlock(&gate_mutex)) abort();
}

static void await(unsigned value)
{
    if (pthread_mutex_lock(&gate_mutex)) abort();
    while (gate_step < value)
        if (pthread_cond_wait(&gate_cond, &gate_mutex)) abort();
    if (pthread_mutex_unlock(&gate_mutex)) abort();
}

static void *read_heap_mapping(void *unused)
{
    const volatile uint8_t *names = xbox_PhysRamNameMap();
    (void)unused;
    observed_first = bw_shared_u8_load(&names[guest_va / CONTIG_PAGE]);
    step(1);
    await(2);
    observed_second = bw_shared_u8_load(&names[guest_va / CONTIG_PAGE + 1]);
    observed_byte = bytes[0];
    return NULL;
}

static int heap_counterexample(void)
{
    pthread_t reader;
    uint32_t next;
    int ok;

    if (unsetenv("RECOMP_HEAP_RECLAIM")) return 0;
    bytes = calloc(2, CONTIG_PAGE);
    if (!bytes) return 0;
    g_memory_offset = (ptrdiff_t)(uintptr_t)bytes - XBOX_HEAP_BASE;
    guest_va = xbox_HeapAlloc(2 * CONTIG_PAGE, CONTIG_PAGE);
    if (guest_va != XBOX_HEAP_BASE || xbox_HeapReclaimEnabled()) return 0;
    bytes[0] = 0x31;
    xbox_PhysNoteRamName(guest_va);
    xbox_PhysNoteRamName(guest_va + CONTIG_PAGE);
    if (pthread_create(&reader, NULL, read_heap_mapping, NULL)) return 0;
    await(1);
    xbox_HeapFree(guest_va);
    next = xbox_HeapAlloc(2 * CONTIG_PAGE, CONTIG_PAGE);
    bytes[0] = 0x52;
    step(2);
    if (pthread_join(reader, NULL)) return 0;
    ok = next == guest_va && observed_first == 1 && observed_second == 0 &&
         observed_byte == 0x52;
    free(bytes);
    return ok;
}

static void *read_contiguous_mapping(void *unused)
{
    (void)unused;
    observed_first = bw_shared_u8_load(&xbox_ContiguousPageMap()[1]);
    step(1);
    await(2);
    observed_second = bw_shared_u8_load(&xbox_ContiguousPageMap()[2]);
    observed_byte = bytes[0];
    return NULL;
}

static int contiguous_counterexample(void)
{
    pthread_t reader;
    uint32_t next, high;
    int cleared, ok;

    if (setenv("RECOMP_HEAP_RECLAIM", "1", 1)) return 0;
    bytes = calloc(2, CONTIG_PAGE);
    if (!bytes) return 0;
    g_memory_offset = (ptrdiff_t)(uintptr_t)bytes -
                      (XBOX_CONTIG_BASE + CONTIG_PAGE);
    guest_va = xbox_ContiguousAllocEx(2 * CONTIG_PAGE, CONTIG_PAGE,
                                      CONTIG_PAGE, 3 * CONTIG_PAGE - 1);
    if (guest_va != XBOX_CONTIG_BASE + CONTIG_PAGE) return 0;
    high = xbox_ContiguousAllocatedBytes();
    bytes[0] = 0x31;
    if (pthread_create(&reader, NULL, read_contiguous_mapping, NULL)) return 0;
    await(1);
    if (!xbox_ContiguousFree(guest_va)) return 0;
    cleared = !bw_shared_u8_load(&xbox_ContiguousPageMap()[1]) &&
              !bw_shared_u8_load(&xbox_ContiguousPageMap()[2]);
    next = xbox_ContiguousAllocEx(2 * CONTIG_PAGE, CONTIG_PAGE,
                                   CONTIG_PAGE, 3 * CONTIG_PAGE - 1);
    bytes[0] = 0x52;
    step(2);
    if (pthread_join(reader, NULL)) return 0;
    ok = cleared && next == guest_va && high == xbox_ContiguousAllocatedBytes() &&
         observed_first == 1 && observed_second == 1 && observed_byte == 0x52;
    free(bytes);
    return ok;
}

static void *translate_window(void *unused)
{
    (void)unused;
    translated = xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)
                                            (XBOX_CONTIG_BASE + guest_va));
    observed_first = bw_shared_u8_load(&bw_PhysLastTranslationMap()[guest_va / CONTIG_PAGE]);
    step(1);
    return NULL;
}

static int alias_counterexample(void)
{
    pthread_t writer;
    ULONG_PTR ram_phys;
    int ok;

    if (unsetenv("BW_PHYS_TRACE")) return 0;
    bytes = calloc(1, CONTIG_PAGE);
    if (!bytes) return 0;
    g_memory_offset = (ptrdiff_t)(uintptr_t)bytes - XBOX_HEAP_BASE;
    guest_va = xbox_HeapAlloc(CONTIG_PAGE, CONTIG_PAGE);
    if (guest_va != XBOX_HEAP_BASE || guest_va >= XBOX_CONTIG_SIZE) return 0;
    xbox_PhysNoteRamName(guest_va);
    xbox_ContiguousClaim(XBOX_CONTIG_BASE + guest_va, CONTIG_PAGE);
    if (pthread_create(&writer, NULL, translate_window, NULL)) return 0;
    await(1);
    ram_phys = xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)guest_va);
    if (pthread_join(writer, NULL)) return 0;
    ok = translated == ram_phys && ram_phys == guest_va && observed_first == 2 &&
         bw_shared_u8_load(&bw_PhysLastTranslationMap()[guest_va / CONTIG_PAGE]) == 1 &&
         bw_shared_u8_load(&xbox_PhysRamNameMap()[guest_va / CONTIG_PAGE]) == 1 &&
         bw_shared_u8_load(&xbox_ContiguousPageMap()[guest_va / CONTIG_PAGE]) == 1;
    free(bytes);
    return ok;
}

static void protection_fault(int sig, siginfo_t *info, void *context)
{
    char token = 1;
    uintptr_t address = info ? (uintptr_t)info->si_addr : 0;
    (void)context;
    if (!info || (sig != SIGSEGV && sig != SIGBUS) ||
        address < protected_start || address >= protected_end ||
        write(fault_notice[1], &token, 1) != 1 ||
        read(fault_resume[0], &token, 1) != 1)
        _exit(1);
}

static int install_protection_fault(void)
{
    struct sigaction action = {0};
    action.sa_sigaction = protection_fault;
    action.sa_flags = SA_SIGINFO;
    if (sigemptyset(&action.sa_mask) || pipe(fault_notice) || pipe(fault_resume))
        return 0;
    return sigaction(SIGSEGV, &action, NULL) == 0 &&
           sigaction(SIGBUS, &action, NULL) == 0;
}

static int await_protection_fault(void)
{
    char token;
    return read(fault_notice[0], &token, 1) == 1;
}

static int resume_protection_fault(void)
{
    char token = 1;
    return write(fault_resume[1], &token, 1) == 1;
}

static void *allocate_block(void *unused)
{
    (void)unused;
    worker_allocation = xbox_ContiguousAllocEx(CONTIG_PAGE, CONTIG_PAGE,
                                                CONTIG_PAGE, 2 * CONTIG_PAGE - 1);
    return NULL;
}

static int publication_counterexample(void)
{
    long page = sysconf(_SC_PAGESIZE);
    size_t mapping_size, protected_offset;
    pthread_t worker;
    uint8_t *mapping;
    uint32_t expected = XBOX_CONTIG_BASE + CONTIG_PAGE;
    int observed;
    if (page <= 0 || page > XBOX_CONTIG_SIZE / 2 || (page & (page - 1)) ||
        !install_protection_fault()) return 0;
    mapping_size = (size_t)page * 2u + CONTIG_PAGE;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    memset(mapping, 0xA5, mapping_size);
    g_memory_offset = (ptrdiff_t)(uintptr_t)mapping - XBOX_CONTIG_BASE;
    protected_offset = CONTIG_PAGE & ~((size_t)page - 1u);
    protected_start = (uintptr_t)mapping + protected_offset;
    protected_end = protected_start + (size_t)page;
    if (mprotect((void *)protected_start, (size_t)page, PROT_READ) ||
        pthread_create(&worker, NULL, allocate_block, NULL) || !await_protection_fault())
        return 0;
    observed = xbox_ContiguousBlockSize(expected) == CONTIG_PAGE &&
               bw_shared_u8_load(&xbox_ContiguousPageMap()[1]) == 1 &&
               xbox_ContiguousAllocatedBytes() >= 2 * CONTIG_PAGE &&
               mapping[CONTIG_PAGE] == 0xA5;
    if (mprotect((void *)protected_start, (size_t)page, PROT_READ | PROT_WRITE) ||
        !resume_protection_fault() || pthread_join(worker, NULL))
        return 0;
    observed = observed && worker_allocation == expected && mapping[CONTIG_PAGE] == 0;
    if (munmap(mapping, mapping_size)) return 0;
    return observed;
}

static void *write_block(void *unused)
{
    (void)unused;
    *worker_write = 0x6B;
    return NULL;
}

static int host_page_counterexample(void)
{
    long page = sysconf(_SC_PAGESIZE);
    size_t mapping_size;
    pthread_t worker;
    uint8_t *mapping;
    uint32_t first, second;
    int observed;
    if (page <= 0 || page > XBOX_CONTIG_SIZE / 2 || (page & (page - 1))) return 0;
    if ((size_t)page < 2u * CONTIG_PAGE) return 77;
    if (!install_protection_fault()) return 0;
    mapping_size = (size_t)page * 3u;
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return 0;
    g_memory_offset = (ptrdiff_t)(uintptr_t)mapping - XBOX_CONTIG_BASE;
    first = xbox_ContiguousAllocEx(CONTIG_PAGE, CONTIG_PAGE, (uint32_t)page,
                                   (uint32_t)page + 2u * CONTIG_PAGE - 1u);
    second = xbox_ContiguousAllocEx(CONTIG_PAGE, CONTIG_PAGE, (uint32_t)page,
                                    (uint32_t)page + 2u * CONTIG_PAGE - 1u);
    if (first != XBOX_CONTIG_BASE + (uint32_t)page || second != first + CONTIG_PAGE)
        return 0;
    protected_start = (uintptr_t)mapping + (size_t)page;
    protected_end = protected_start + (size_t)page;
    worker_write = mapping + (size_t)page + CONTIG_PAGE;
    if (mprotect((void *)protected_start, (size_t)page, PROT_READ) ||
        pthread_create(&worker, NULL, write_block, NULL) || !await_protection_fault())
        return 0;
    observed = *worker_write == 0 &&
               xbox_ContiguousBlockSize(first) == CONTIG_PAGE &&
               xbox_ContiguousBlockSize(second) == CONTIG_PAGE;
    if (mprotect((void *)protected_start, (size_t)page, PROT_READ | PROT_WRITE) ||
        !resume_protection_fault() || pthread_join(worker, NULL))
        return 0;
    observed = observed && *worker_write == 0x6B;
    if (munmap(mapping, mapping_size)) return 0;
    return observed;
}

int main(int argc, char **argv)
{
    int ok;
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "heap")) ok = heap_counterexample();
    else if (!strcmp(argv[1], "contiguous")) ok = contiguous_counterexample();
    else if (!strcmp(argv[1], "alias")) ok = alias_counterexample();
    else if (!strcmp(argv[1], "publication")) ok = publication_counterexample();
    else if (!strcmp(argv[1], "host-page")) {
        ok = host_page_counterexample();
        if (ok == 77) return 77;
    }
    else return 2;
    if (!ok) fprintf(stderr, "expected counterexample was not reproduced\n");
    return !ok;
}
