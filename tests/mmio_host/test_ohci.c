#include "../../src/toolkit_patches/usb/ohci.c"

#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do {                                                      \
    if (!(expr)) {                                                            \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
        return 0;                                                             \
    }                                                                         \
} while (0)

static ptrdiff_t s_test_offset;
static size_t s_test_ram_size;
static uint8_t s_test_phys_names[(64u << 20) / 4096];
static void *s_fail_protect;
static void *s_fail_restore;
static unsigned s_protect_calls;

uint32_t g_xbox_image_lo, g_xbox_image_hi;

ptrdiff_t xbox_GetMemoryOffset(void) { return s_test_offset; }
size_t xbox_GetMappedSize(void) { return s_test_ram_size; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
const volatile uint8_t *xbox_PhysRamNameMap(void) { return s_test_phys_names; }

LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protection, PDWORD old)
{
    int prot = protection == PAGE_NOACCESS ? PROT_NONE : PROT_READ | PROT_WRITE;
    if (old)
        *old = PAGE_READWRITE;
    s_protect_calls++;
    if ((protection == PAGE_NOACCESS && address == s_fail_protect)
            || (protection == PAGE_READWRITE && address == s_fail_restore))
        return FALSE;
    return mprotect(address, size, prot) == 0;
}

static int test_register_semantics(uint8_t *ram)
{
    OhciController hc;

    s_ndp = 4;
    s_trace = 0;
    s_test_offset = (ptrdiff_t)(uintptr_t)ram;
    s_test_ram_size = (size_t)sysconf(_SC_PAGESIZE);
    ohci_reset(&hc, XBOX_OHCI0_BASE, 0);
    CHECK(ohci_read(&hc, HcRevision, 4) == 0x10);
    CHECK((ohci_read(&hc, HcRhDescriptorA, 4) & 0xff) == 4);

    ohci_write(&hc, HcControl, 0x80, 4);
    ohci_write(&hc, HcCommandStatus, CS_HCR, 4);
    CHECK(!(ohci_read(&hc, HcCommandStatus, 4) & CS_HCR));
    CHECK(!(ohci_read(&hc, HcControl, 4) & 0xc0));

    ohci_write(&hc, HcInterruptEnable, INTR_MIE | INTR_RHSC | INTR_WDH, 4);
    ohci_write(&hc, HcInterruptDisable, INTR_RHSC, 4);
    CHECK(ohci_read(&hc, HcInterruptEnable, 4) == (INTR_MIE | INTR_WDH));

    hc.reg[HcHCCA / 4] = 0x100;
    hc.reg[HcDoneHead / 4] = 0x1234;
    hc.reg[HcInterruptStatus / 4] = INTR_WDH | INTR_RHSC;
    *(uint32_t *)(ram + 0x100 + HCCA_DONE_HEAD) = 0x1234;
    ohci_write(&hc, HcInterruptStatus, INTR_WDH, 4);
    CHECK(*(uint32_t *)(ram + 0x100 + HCCA_DONE_HEAD) == 0);
    CHECK(hc.reg[HcDoneHead / 4] == 0);
    CHECK(hc.reg[HcInterruptStatus / 4] == INTR_RHSC);
    CHECK(hc.ack_seq == 1);

    hc.reg[HcRhPortStatus1 / 4] |= PORT_CCS | PORT_PRSC;
    ohci_write(&hc, HcRhPortStatus1, PORT_W_PRS_SET_RESET | PORT_PRSC, 4);
    CHECK((hc.reg[HcRhPortStatus1 / 4] & (PORT_PES | PORT_PRSC)) ==
          (PORT_PES | PORT_PRSC));
    CHECK(hc.reg[HcInterruptStatus / 4] & INTR_RHSC);
    return 1;
}

typedef struct {
    OhciController *hc;
    unsigned lane;
} Writer;

static void *write_lane(void *opaque)
{
    Writer *w = opaque;
    unsigned i;
    for (i = 0; i < 20000; i++)
        ohci_write(w->hc, HcControlHeadED + w->lane, 0x11u * (w->lane + 1), 1);
    return NULL;
}

static int test_concurrent_register_writes(void)
{
    OhciController hc;
    Writer args[4];
    pthread_t workers[4];
    unsigned i;

    ohci_reset(&hc, XBOX_OHCI0_BASE, 0);
    for (i = 0; i < 4; i++) {
        args[i].hc = &hc;
        args[i].lane = i;
        CHECK(pthread_create(&workers[i], NULL, write_lane, &args[i]) == 0);
    }
    for (i = 0; i < 4; i++)
        CHECK(pthread_join(workers[i], NULL) == 0);
    CHECK(ohci_read(&hc, HcControlHeadED, 4) == 0x44332211u);
    CHECK(hc.writes == 80000);
    return 1;
}

static void *stoppable_worker(void *unused)
{
    struct timespec delay = {0, 1000000};
    (void)unused;
    while (!__atomic_load_n(&s_ohci_stop, __ATOMIC_SEQ_CST))
        nanosleep(&delay, NULL);
    return NULL;
}

static int test_protection_and_shutdown(uint8_t *region, size_t page_size)
{
    uint8_t *first = region;
    uint8_t *second = region + (XBOX_OHCI1_BASE - XBOX_OHCI0_BASE);
    uint32_t failed_base = 0;
    unsigned calls;

    CHECK(page_size <= XBOX_OHCI1_BASE - XBOX_OHCI0_BASE);
    s_test_offset = (ptrdiff_t)((uintptr_t)region - XBOX_OHCI0_BASE);
    s_hc[0].base = XBOX_OHCI0_BASE;
    s_hc[1].base = XBOX_OHCI1_BASE;
    s_ohci_trapped = 0;
    s_enabled = 1;
    s_init_done = 1;
    CHECK(ohci_protect_posix(s_test_offset, &failed_base) == 1);
    CHECK(s_ohci_trapped == 3);
    s_ohci_stop = 0;
    CHECK(pthread_create(&s_ohci_thread, NULL, stoppable_worker, NULL) == 0);
    s_ohci_thread_started = 1;
    CHECK(bw_xbox_ohci_stop() == 0);
    CHECK(!s_ohci_thread_started && !s_ohci_trapped && !s_enabled && !s_init_done);
    first[0] = 0x12;
    second[0] = 0x34;
    CHECK(first[0] == 0x12 && second[0] == 0x34);
    calls = s_protect_calls;
    CHECK(bw_xbox_ohci_stop() == 0 && s_protect_calls == calls);

    s_fail_protect = second;
    CHECK(ohci_protect_posix(s_test_offset, &failed_base) == 0);
    CHECK(failed_base == XBOX_OHCI1_BASE && !s_ohci_trapped);
    first[0] = 0x56;
    CHECK(first[0] == 0x56);

    s_fail_restore = first;
    CHECK(ohci_protect_posix(s_test_offset, &failed_base) == -1);
    CHECK(s_ohci_trapped == 1);
    s_fail_restore = NULL;
    s_fail_protect = NULL;
    CHECK(ohci_unprotect_posix() == 0 && !s_ohci_trapped);
    first[0] = 0x78;
    CHECK(first[0] == 0x78);

    CHECK(ohci_protect_posix(s_test_offset, &failed_base) == 1);
    s_enabled = 1;
    s_init_done = 1;
    s_fail_restore = first;
    CHECK(bw_xbox_ohci_stop() == -1);
    CHECK(s_ohci_trapped == 1 && s_enabled && s_init_done);
    second[0] = 0x9a;
    s_fail_restore = NULL;
    CHECK(bw_xbox_ohci_stop() == 0 && !s_ohci_trapped);
    first[0] = 0xbc;
    CHECK(first[0] == 0xbc && second[0] == 0x9a);
    return 1;
}

int main(void)
{
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t region_size = (XBOX_OHCI1_BASE - XBOX_OHCI0_BASE) + page_size;
    uint8_t *ram = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *region = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    int ok;

    if (ram == MAP_FAILED || region == MAP_FAILED)
        return 1;
    ok = test_register_semantics(ram)
      && test_concurrent_register_writes()
      && test_protection_and_shutdown(region, page_size);
    munmap(ram, page_size);
    munmap(region, region_size);
    return ok ? 0 : 1;
}
