/*
 * Experimental checked-dispatch fixture only. It is not connected to the
 * generated runtime or the host, and its deliberately small register allowlist
 * is not a coverage claim. Device calls here run in ordinary thread context.
 */
#include "apu.h"
#include "apu_state.h"
#include "apu_xaudio2.h"
#include "../../src/toolkit_patches/usb/ohci.c"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GUEST_SIZE (UINT64_C(1) << 32)
#define RAM_SIZE (64u * 1024u * 1024u)
#define APU_BASE 0xFE800000u
#define APU_END 0xFE830000u
#define DSP_START 0xFE830000u
#define DSP_SIZE 0x50000u
#define TEST_ED 0x1000u
#define TEST_TD 0x2000u
#define TEST_TAIL 0x2100u
#define TEST_BUFFER 0x3000u
#define EXIT_REENTER 55
#define EXIT_BAD_CHILD 56

#define CHECK(expr) do {                                                      \
    if (!(expr)) {                                                            \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
        return 0;                                                             \
    }                                                                         \
} while (0)

void test_set_guest_base(uint8_t *base);
void test_set_phys_ram_page(uint32_t page, uint8_t named);
uint32_t bw_checked_test_contig_high_base(void);

typedef enum { NOT_MMIO, HANDLED, FATAL } Outcome;
typedef enum { READ_ACCESS, WRITE_ACCESS, ATOMIC_ADD_ACCESS } Operation;

typedef struct {
    MCPXAPUState *apu;
    OhciController *hc[2];
    int test_nested_apu_dma;
} CheckedFixture;

static _Thread_local unsigned s_depth;
static _Thread_local int s_inject_reentry;
static _Thread_local CheckedFixture *s_reentry_fixture;
static uint32_t s_test_contig_high;

static Outcome checked_access(CheckedFixture *, uint64_t, unsigned,
                              Operation, uint64_t *);

uint32_t xbox_ContiguousAllocatedBytes(void)
{
    if (s_inject_reentry) {
        uint64_t value = 0;
        s_inject_reentry = 0;
        if (checked_access(s_reentry_fixture, APU_BASE + NV_PAPU_XGSCNT,
                           4, READ_ACCESS, &value) == FATAL)
            _exit(EXIT_REENTER);
        _exit(EXIT_BAD_CHILD);
    }
    return s_test_contig_high ? s_test_contig_high
                              : bw_checked_test_contig_high_base();
}

size_t xbox_GetMappedSize(void) { return RAM_SIZE; }

DWORD xbox_InputGetState(DWORD port, XBOX_INPUT_STATE *state)
{
    if (port != 0)
        return 1;
    memset(state, 0, sizeof(*state));
    state->Gamepad.wButtons = XBOX_GAMEPAD_START | XBOX_GAMEPAD_DPAD_UP;
    state->Gamepad.bAnalogButtons[0] = 0xA5;
    state->Gamepad.sThumbLX = 0x1234;
    state->Gamepad.sThumbLY = -256;
    state->Gamepad.sThumbRX = -1;
    state->Gamepad.sThumbRY = 0x2468;
    return 0;
}

DWORD xbox_InputSetState(DWORD port, const XBOX_VIBRATION *value)
{
    (void)port; (void)value;
    return 0;
}

int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }

static int apu_test_register(uint32_t off, const CheckedFixture *fixture)
{
    switch (off) {
    case NV_PAPU_TVL2D:
    case NV_PAPU_TVL3D:
    case NV_PAPU_TVLMP:
    case NV_PAPU_ISTS:
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
    case NV_PAPU_XGSCNT:
    case NV_PAPU_GPSMAXSGE:
    case NV_PAPU_EPOFBASE0:
    case NV_PAPU_EPOFEND0:
    case NV_PAPU_EPOFCUR0:
        return 1;
    case NV_PAPU_FEMEMADDR:
    case NV_PAPU_FEMEMDATA:
        return fixture->test_nested_apu_dma;
    default:
        return 0;
    }
}

static Outcome checked_access(CheckedFixture *fixture, uint64_t address,
                              unsigned size, Operation operation, uint64_t *value)
{
    static const struct { uint64_t base, end; int device; } ranges[] = {
        { APU_BASE, APU_END, -1 },
        { XBOX_OHCI0_BASE, XBOX_OHCI0_BASE + XBOX_OHCI_SIZE, 0 },
        { XBOX_OHCI1_BASE, XBOX_OHCI1_BASE + XBOX_OHCI_SIZE, 1 },
    };
    uint64_t end;

    if (!fixture || !value || !size || size > 8 || address >= GUEST_SIZE ||
            (uint64_t)size > GUEST_SIZE - address)
        return FATAL;
    end = address + size;
    for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        uint32_t off;
        if (address >= ranges[i].end || end <= ranges[i].base)
            continue;
        if (address < ranges[i].base || end > ranges[i].end ||
                (operation != READ_ACCESS && operation != WRITE_ACCESS) ||
                s_depth)
            return FATAL;
        off = (uint32_t)(address - ranges[i].base);
        if (ranges[i].device < 0) {
            if (!fixture->apu || size != 4 || (off & 3) ||
                    !apu_test_register(off, fixture))
                return FATAL;
            s_depth++;
            if (operation == READ_ACCESS)
                *value = mcpx_apu_mmio_read(fixture->apu, off, 4);
            else
                mcpx_apu_mmio_write(fixture->apu, off, *value, 4);
        } else {
            OhciController *hc = fixture->hc[ranges[i].device];
            uint32_t reg = off & ~3u;
            if (!hc || (size != 1 && size != 2 && size != 4) ||
                    (off & (size - 1)) || (off & 3) + size > 4)
                return FATAL;
            /* Subword lanes are tested only on this ordinary storage
             * register, not on write-one-to-clear/other special registers. */
            if (reg != HcControlHeadED &&
                    (size != 4 || (reg != HcRevision &&
                                   reg != HcControl &&
                                   reg != HcInterruptStatus)))
                return FATAL;
            s_depth++;
            if (operation == READ_ACCESS)
                *value = ohci_read(hc, off, (int)size);
            else
                ohci_write(hc, off, *value, (int)size);
        }
        s_depth--;
        return HANDLED;
    }
    return NOT_MMIO;
}

static uint8_t *map_guest(void)
{
    uint8_t *base = mmap(NULL, (size_t)GUEST_SIZE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    if (mprotect(base, RAM_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(base + 0x80000000u, RAM_SIZE, PROT_READ | PROT_WRITE) ||
            mprotect(base + DSP_START, DSP_SIZE, PROT_READ | PROT_WRITE)) {
        munmap(base, (size_t)GUEST_SIZE);
        return NULL;
    }
    test_set_guest_base(base);
    return base;
}

static int wait_child(pid_t child, int expected_exit, int expected_signal)
{
    struct timespec delay = {0, 10000000};
    int status;
    for (int i = 0; i < 500; i++) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child)
            return expected_signal
                ? (WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV ||
                                           WTERMSIG(status) == SIGBUS))
                : (WIFEXITED(status) && WEXITSTATUS(status) == expected_exit);
        if (result < 0)
            return 0;
        nanosleep(&delay, NULL);
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    fprintf(stderr, "bounded child timed out\n");
    return 0;
}

static int test_uncovered_sentinel(uint8_t *base)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct rlimit no_core = {0, 0};
        void *(*volatile direct_memset)(void *, int, size_t) = memset;
        setrlimit(RLIMIT_CORE, &no_core);
        signal(SIGSEGV, SIG_DFL);
        signal(SIGBUS, SIG_DFL);
        /* This emulates a raw REP STOSB bulk path that bypasses the fixture.
         * The protected aperture must fault, not fake a device store. */
        direct_memset(base + XBOX_OHCI0_BASE, 0, 4);
        _exit(EXIT_BAD_CHILD);
    }
    CHECK(wait_child(child, 0, 1));
    return 1;
}

static int test_nested_apu_callback(uint8_t *base)
{
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CheckedFixture fixture = {0};
        uint64_t value = TEST_BUFFER;
        fixture.apu = mcpx_apu_init_standalone(base);
        fixture.test_nested_apu_dma = 1;
        if (!fixture.apu || checked_access(&fixture, APU_BASE + NV_PAPU_FEMEMADDR,
                4, WRITE_ACCESS, &value) != HANDLED)
            _exit(EXIT_BAD_CHILD);
        s_reentry_fixture = &fixture;
        s_inject_reentry = 1;
        value = 0x12345678;
        /* The real callback resolves this synthetic RAM address through the
         * injected high-water getter; the nested device request must not run.
         * Termination, rather than unwinding through arbitrary device locks,
         * is the test's required reaction to FATAL. */
        (void)checked_access(&fixture, APU_BASE + NV_PAPU_FEMEMDATA,
                             4, WRITE_ACCESS, &value);
        _exit(EXIT_BAD_CHILD);
    }
    CHECK(wait_child(child, EXIT_REENTER, 0));
    return 1;
}

static int write_checked(CheckedFixture *f, uint32_t address, unsigned size,
                         uint64_t value)
{
    return checked_access(f, address, size, WRITE_ACCESS, &value) == HANDLED;
}

static int read_checked(CheckedFixture *f, uint32_t address, unsigned size,
                        uint64_t *value)
{
    return checked_access(f, address, size, READ_ACCESS, value) == HANDLED;
}

static int test_validation(CheckedFixture *fixture, uint8_t *base)
{
    CheckedFixture absent = {0};
    uint64_t v = UINT64_C(0xfeedfacecafebeef);
    uint32_t *dsp = (uint32_t *)(base + DSP_START);

    CHECK(checked_access(&absent, 0x1000, 4, READ_ACCESS, &v) == NOT_MMIO);
    CHECK(checked_access(&absent, APU_BASE + NV_PAPU_XGSCNT, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(&absent, XBOX_OHCI0_BASE, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, APU_BASE + NV_PAPU_XGSCNT, 1, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, APU_BASE + NV_PAPU_XGSCNT + 2, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, APU_BASE + 0x10, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, APU_END - 2, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, XBOX_OHCI0_BASE - 2, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, XBOX_OHCI0_BASE + OHCI_REG_MAX, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, XBOX_OHCI0_BASE + HcControlHeadED, 8, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, XBOX_OHCI0_BASE + HcControlHeadED + 3, 2, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, XBOX_OHCI0_BASE + HcControlHeadED, 4, ATOMIC_ADD_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, UINT32_MAX - 1u, 4, READ_ACCESS, &v) == FATAL);
    CHECK(checked_access(fixture, APU_BASE, 0, READ_ACCESS, &v) == FATAL);
    CHECK(v == UINT64_C(0xfeedfacecafebeef));
    CHECK(checked_access(fixture, DSP_START, 4, WRITE_ACCESS, &v) == NOT_MMIO);
    *dsp = 0x12345678;
    CHECK(*dsp == 0x12345678);
    CHECK(checked_access(fixture, 0xFE850000u, 4, READ_ACCESS, &v) == NOT_MMIO);
    return 1;
}

static int test_ohci_lanes(CheckedFixture *f)
{
    uint64_t v;
    uint32_t addr = XBOX_OHCI0_BASE + HcControlHeadED;
    CHECK(write_checked(f, addr, 4, 0x11223344));
    CHECK(read_checked(f, addr, 1, &v) && v == 0x44);
    CHECK(read_checked(f, addr + 2, 2, &v) && v == 0x1122);
    CHECK(write_checked(f, addr + 1, 1, 0xAA));
    CHECK(write_checked(f, addr + 2, 2, 0xBEEF));
    CHECK(read_checked(f, addr, 4, &v) && v == 0xBEEFAA44);
    return 1;
}

typedef struct {
    CheckedFixture *fixture;
    unsigned lane;
    int ok;
} LaneWorker;

static void *write_lane(void *opaque)
{
    LaneWorker *w = opaque;
    w->ok = 1;
    for (unsigned i = 0; i < 20000; i++) {
        if (!write_checked(w->fixture, XBOX_OHCI0_BASE + HcControlHeadED + w->lane,
                           1, 0x11u * (w->lane + 1))) {
            w->ok = 0;
            break;
        }
    }
    return NULL;
}

typedef struct {
    uint8_t *base;
    OhciController *hc;
    int ok;
} ReportWorker;

static const uint8_t expected_report[20] = {
    0, 20, 0x11, 0, 0xA5, 0, 0, 0, 0, 0, 0, 0,
    0x34, 0x12, 0x00, 0xFF, 0xFF, 0xFF, 0x68, 0x24
};

static void prepare_td(uint8_t *base, uint32_t buffer, uint32_t bytes)
{
    uint32_t *ed = (uint32_t *)(base + TEST_ED);
    uint32_t *td = (uint32_t *)(base + TEST_TD);
    ed[0] = 7u | (1u << 7); /* addressed pad, interrupt endpoint 1 */
    ed[1] = TEST_TAIL;
    ed[2] = TEST_TD;
    ed[3] = 0;
    td[0] = TD_DP_IN << 19;
    td[1] = buffer;
    td[2] = TEST_TAIL;
    td[3] = buffer + bytes - 1;
}

static void *send_reports(void *opaque)
{
    ReportWorker *w = opaque;
    w->ok = 1;
    for (unsigned i = 0; i < 1000; i++) {
        uint32_t done = 0;
        prepare_td(w->base, TEST_BUFFER, sizeof(expected_report));
        memset(w->base + TEST_BUFFER, 0xCC, sizeof(expected_report));
        if (ohci_walk_eds(w->hc, TEST_ED, &done) != 1 || done != TEST_TD ||
                ((uint32_t *)(w->base + TEST_TD))[1] != 0 ||
                ((uint32_t *)(w->base + TEST_TD))[0] >> 28 != TD_CC_NOERROR ||
                ((uint32_t *)(w->base + TEST_ED))[2] != TEST_TAIL ||
                memcmp(w->base + TEST_BUFFER, expected_report, sizeof(expected_report))) {
            w->ok = 0;
            break;
        }
    }
    return NULL;
}

static int test_real_devices(CheckedFixture *fixture, uint8_t *base)
{
    UsbSetup setup = {0};
    LaneWorker lanes[4];
    ReportWorker report = {base, fixture->hc[0], 0};
    pthread_t workers[5];
    unsigned started = 0;
    uint64_t v;
    int ok = 1;

    setup.bRequest = 5; /* SET_ADDRESS */
    setup.wValue = 7;
    CHECK(usb_gamepad_control(0, &setup, NULL, 0) == 0);
    setup.bRequest = 9; /* SET_CONFIGURATION */
    setup.wValue = 1;
    CHECK(usb_gamepad_control(0, &setup, NULL, 0) == 0);
    s_plugged_pads = 1;
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_EPOFCUR0, 4, 0x1234));
    CHECK(read_checked(fixture, APU_BASE + NV_PAPU_EPOFCUR0, 4, &v) && v == 0x1234);
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_TVL2D, 4, 0xffff));
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_TVL3D, 4, 0xffff));
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_TVLMP, 4, 0xffff));
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_FECTL, 4,
                        NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING));
    CHECK(write_checked(fixture, APU_BASE + NV_PAPU_SECTL, 4,
                        NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF));

    for (unsigned i = 0; i < 4; i++) {
        lanes[i] = (LaneWorker){fixture, i, 0};
        if (pthread_create(&workers[started], NULL, write_lane, &lanes[i])) {
            ok = 0;
            break;
        }
        started++;
    }
    if (ok && pthread_create(&workers[started], NULL, send_reports, &report) == 0)
        started++;
    else
        ok = 0;
    for (int i = 0; i < 2000 && ok && (i < 100 || xa2_samples_played() == 0); i++) {
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_ISTS, 4, UINT32_MAX);
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_GPSMAXSGE, 4, i & 1);
        ok &= read_checked(fixture, APU_BASE + NV_PAPU_GPSMAXSGE, 4, &v);
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_EPOFBASE0, 4, 0);
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_EPOFEND0, 4, 0);
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_EPOFCUR0, 4, i);
        ok &= write_checked(fixture, APU_BASE + NV_PAPU_XGSCNT, 4, 0);
        ok &= read_checked(fixture, APU_BASE + NV_PAPU_XGSCNT, 4, &v);
        usleep(1000);
    }
    for (unsigned i = 0; i < started; i++)
        if (pthread_join(workers[i], NULL))
            ok = 0;
    for (unsigned i = 0; i < 4 && i < started; i++)
        ok &= lanes[i].ok;
    ok &= report.ok && xa2_samples_played() > 0;
    ok &= read_checked(fixture, XBOX_OHCI0_BASE + HcControlHeadED, 4, &v);
    ok &= v == 0x44332211u;
    CHECK(ok);
    return 1;
}

static int rejected_buffer(OhciController *hc, uint8_t *base,
                           uint32_t address, uint32_t bytes, uint32_t dp,
                           uint32_t expected_cc)
{
    uint32_t *td = (uint32_t *)(base + TEST_TD);
    prepare_td(base, address, bytes);
    td[0] = dp << 19;
    CHECK(ohci_do_td(hc, 7u | (1u << 7), TEST_TD) == expected_cc);
    CHECK(td[1] == address);
    return 1;
}

static int test_invalid_dma(uint8_t *base)
{
    OhciController hc;
    UsbSetup setup = {0};
    uint32_t done = 0;
    uint32_t *td = (uint32_t *)(base + TEST_TD);
    uint32_t *ed = (uint32_t *)(base + TEST_ED);
    setup.bRequest = 5;
    setup.wValue = 7;
    CHECK(usb_gamepad_control(0, &setup, NULL, 0) == 0);
    setup.bRequest = 9;
    setup.wValue = 1;
    CHECK(usb_gamepad_control(0, &setup, NULL, 0) == 0);
    s_plugged_pads = 1;
    s_trace = 0;
    ohci_reset(&hc, XBOX_OHCI0_BASE, 0);

    CHECK(rejected_buffer(&hc, base, APU_BASE, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    CHECK(rejected_buffer(&hc, base, APU_BASE, 20, TD_DP_OUT,
                          TD_CC_BUFFERUNDERRUN));
    CHECK(rejected_buffer(&hc, base, APU_BASE, 8, TD_DP_SETUP,
                          TD_CC_BUFFERUNDERRUN));
    CHECK(rejected_buffer(&hc, base, RAM_SIZE - 8, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    CHECK(rejected_buffer(&hc, base, OHCI_CONTIG_BASE + RAM_SIZE - 8, 20,
                          TD_DP_IN, TD_CC_BUFFEROVERRUN));
    CHECK(rejected_buffer(&hc, base, UINT32_MAX - 8, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    CHECK(rejected_buffer(&hc, base, 1, UINT32_MAX, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));

    memset(base + 0x3ff7, 0xcc, sizeof(expected_report) + 2);
    prepare_td(base, 0x3ff8, sizeof(expected_report));
    CHECK(ohci_do_td(&hc, 7u | (1u << 7), TEST_TD) == TD_CC_NOERROR);
    CHECK(td[1] == 0 && !memcmp(base + 0x3ff8, expected_report,
                                sizeof(expected_report)));
    CHECK(base[0x3ff7] == 0xcc && base[0x400c] == 0xcc);

    memset(base + OHCI_CONTIG_BASE + 0x5ff8, 0xcc, sizeof(expected_report));
    prepare_td(base, OHCI_CONTIG_BASE + 0x5ff8, sizeof(expected_report));
    CHECK(ohci_do_td(&hc, 7u | (1u << 7), TEST_TD) == TD_CC_NOERROR);
    CHECK(td[1] == 0 &&
          !memcmp(base + OHCI_CONTIG_BASE + 0x5ff8, expected_report,
                  sizeof(expected_report)));

    prepare_td(base, TEST_BUFFER, 1);
    td[0] = TD_DP_OUT << 19;
    td[1] = 0;
    CHECK(ohci_do_td(&hc, 7u | (1u << 7), TEST_TD) == TD_CC_NOERROR);
    CHECK(td[1] == 0);

    prepare_td(base, APU_BASE, sizeof(expected_report));
    CHECK(ohci_walk_eds(&hc, TEST_ED, &done) == 1 && done == TEST_TD);
    CHECK((td[0] >> 28) == TD_CC_BUFFEROVERRUN && td[1] == APU_BASE);
    CHECK((ed[2] & ED_HEAD_HALT) != 0);

    test_set_phys_ram_page(TEST_TD >> 12, 1);
    s_test_contig_high = 0x10000;
    g_xbox_image_lo = 0x5000;
    g_xbox_image_hi = 0x6000;
    CHECK(rejected_buffer(&hc, base, 0x4ff8, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    CHECK(rejected_buffer(&hc, base, 0x4ff0, 0x2021, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    g_xbox_image_lo = 0x5004;
    g_xbox_image_hi = 0x5008;
    CHECK(rejected_buffer(&hc, base, 0x4ff8, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    g_xbox_image_lo = g_xbox_image_hi = 0;
    s_test_contig_high = 0x5004;
    CHECK(rejected_buffer(&hc, base, 0x4ff8, 20, TD_DP_IN,
                          TD_CC_BUFFEROVERRUN));
    s_test_contig_high = 0;
    test_set_phys_ram_page(TEST_TD >> 12, 0);
    return 1;
}

static void isolate_test_environment(void)
{
    static const char *names[] = {
        "RECOMP_PAD_PRESS", "RECOMP_PAD_SCRIPT", "RECOMP_PAD_LIVE",
        "RECOMP_INPUT_DIAG", "RECOMP_AUDIO_WAV", "BW_AUDIO_WAV6",
        "BW_APU_AUDIT", "BW_APU_STEMS", "BW_APU_GPDUMP",
        "RECOMP_APU_VOICE_DUMP", "RECOMP_APU_VOICE_DUMP_FILE"
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unsetenv(names[i]);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("BW_APU_DSP", "xemu", 1);
    setenv("BW_APU_DSP_FALLBACK", "0", 1);
    setenv("BW_APU_DSP_THREAD", "1", 1);
}

int main(int argc, char **argv)
{
    CheckedFixture fixture = {0};
    OhciController hc[2];
    uint8_t *base;
    int ok;

    isolate_test_environment();
    base = map_guest();
    if (!base)
        return 1;
    if (argc == 2 && !strcmp(argv[1], "--invalid-dma")) {
        ok = test_invalid_dma(base);
    } else {
        ok = test_uncovered_sentinel(base) && test_nested_apu_callback(base);
        ohci_reset(&hc[0], XBOX_OHCI0_BASE, 0);
        ohci_reset(&hc[1], XBOX_OHCI1_BASE, 1);
        fixture.hc[0] = &hc[0];
        fixture.hc[1] = &hc[1];
        fixture.apu = mcpx_apu_init_standalone(base);
        ok &= fixture.apu != NULL;
        if (fixture.apu) {
            ok &= test_validation(&fixture, base);
            ok &= test_ohci_lanes(&fixture);
            ok &= test_real_devices(&fixture, base);
            mcpx_apu_shutdown(fixture.apu);
        }
    }
    test_set_guest_base(NULL);
    munmap(base, (size_t)GUEST_SIZE);
    return ok ? 0 : 1;
}
