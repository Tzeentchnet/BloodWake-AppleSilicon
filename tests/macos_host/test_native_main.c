#define _DARWIN_C_SOURCE 1
#include "bw_edition.h"
#include "bw_macos_boot.h"
#include "toolkit_patches/video/bw_framebuffer_metal.h"
#include <xbox/xboxrecomp.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int bw_native_main(int argc, char **argv);

RECOMP_TLS uint32_t g_esp;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_region_base = 0x100000;
uint32_t g_xbox_stack_base = 0x200000, g_xbox_stack_size = 0x80000;
MCPXAPUState *g_apu_state;
static uint8_t *memory;
static pthread_t ui_thread, guest_thread_id;
static int step, fl_initialized, marked_guest, did_cleanup;
static unsigned pb_methods;
static int metal_expected;
static BwMetalFramebufferCallback metal_framebuffer_callback;
static _Atomic int metal_error;

static void check(int ok)
{
    if (!ok)
        _exit(70);
}

void bw_settings_apply(void) {}
void bw_settings_print_schema(FILE *f) { fputs("{}\n", f); }
int bw_edition_init(char *err, size_t n) { (void)err; (void)n; step = 1; return 0; }
void bw_edition_apply_image_fixes(const void *p, size_t n)
{
    check(step == 2 && p && n == 4);
    step = 3;
}
const char *bw_edition_prepare_saves(void) { return NULL; }
void bw_edition_summary(void) {}
void bw_profile_init(void) {}
void bw_profile_dump(const char *why) { (void)why; }
void bw_fl_init(void) { fl_initialized = 1; }
void bw_fl_thread(int role) { check(fl_initialized && role == 0); marked_guest = 1; }
void bw_fl_dump(const char *why) { (void)why; }
void bw_state_start(void) {}
void bw_debugflags_start(void) {}
void bw_ws_init(int gameplay, int log) { check(log == 1); (void)gameplay; }
unsigned bw_hifps_boot(int gameplay) { check(gameplay); return 0; }
void bw_set_min_flip_interval(unsigned n) { check(n == 2); }

int bw_posix_fault_install(void) { check(step == 1 && marked_guest); return 0; }
void bw_posix_fault_set_base(uintptr_t base) { (void)base; }
void bw_posix_fault_uninstall(void) { did_cleanup |= 8; }
void xbox_SetMapSize(size_t n) { check(n == 128u * 1024 * 1024); }
BOOL xbox_MemoryLayoutInit(const void *p, size_t n)
{
    check(step == 1 && p && n == 4);
    memory = calloc(1, 4u * 1024 * 1024);
    check(memory != NULL);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    step = 2;
    return TRUE;
}
void xbox_MemoryLayoutShutdown(void)
{
    check(did_cleanup & 1);
    free(memory);
    memory = NULL;
    did_cleanup |= 4;
}
void *xbox_GetMemoryBase(void) { return memory; }
ptrdiff_t xbox_GetMemoryOffset(void) { return g_xbox_mem_offset; }
size_t xbox_GetMappedSize(void) { return 4u * 1024 * 1024; }
void nv2a_pb_scan_ring(uint32_t start, uint32_t end);
void nv2a_pb_exec_walk_begin(void) {}
void nv2a_pb_exec_report(void) {}
void xbox_Nv2aAckBusyBits(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t off) { (void)off; return NULL; }
int nv2a_pb_exec_idx16(const uint32_t *p, uint32_t n)
{
    (void)p;
    (void)n;
    return 0;
}
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    check(subch == 0 && method == 0x1d70 && param == 0x12345678);
    *(uint32_t *)(memory + 0x21000) = param;
    pb_methods++;
}
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    check(size == 4096 && alignment == 4096);
    return 0x3000;
}
int bw_metal_backend_start(void)
{
    check(step == 3 && memory && metal_expected &&
          metal_framebuffer_callback == bw_sdl_framebuffer_prepare_metal);
    step = 4;
    return getenv("BW_TEST_METAL_START_FAIL") ? 0 : 1;
}
void bw_sdl_framebuffer_expect_metal(void)
{
    check(step == 3 && !metal_expected && !metal_framebuffer_callback);
    metal_expected = 1;
}
int bw_sdl_framebuffer_prepare_metal(const BwMetalFramebuffer *frame)
{
    (void)frame;
    return 1;
}
void bw_metal_backend_set_framebuffer_callback(BwMetalFramebufferCallback callback)
{
    check(step == 3 && metal_expected && !metal_framebuffer_callback);
    metal_framebuffer_callback = callback;
}
const char *bw_metal_backend_error(void)
{
    return atomic_load(&metal_error) || getenv("BW_TEST_METAL_START_FAIL")
           ? "synthetic failure" : NULL;
}
void bw_metal_backend_stop(void) { did_cleanup |= 2; }
MCPXAPUState *mcpx_apu_init_standalone(uint8_t *p) { (void)p; _exit(71); }
void mcpx_apu_shutdown(MCPXAPUState *d) { (void)d; _exit(71); }
void xbox_OhciInit(void) { check(step == 4); step = 5; }
void xbox_kernel_init(void) { check(step == 5); step = 6; }
void xbox_kernel_shutdown(void) { did_cleanup |= 16; }
void xbox_path_init(const char *game, const char *save)
{
    check(step == 6 && game && !strcmp(game, "bw-host-fixture") && !save);
    step = 7;
}
void xbox_kernel_bridge_init(void) { check(step == 7); step = 8; }
int xbox_Nv2aMirrorCounter(uint32_t va, uint32_t src, uint32_t dst)
{
    check(step == 8 && va == 0x00136b00u && src == 0x2b60 && dst == 0x2518);
    step = 9;
    return 0;
}
int recomp_dispatch_init(void) { check(step == 9); step = 10; return 1; }
void xbox_WatchdogStart(void) { check(step == 10); step = 11; }
int bw_xbox_ohci_stop(void)
{
    did_cleanup |= 1;
    return getenv("BW_TEST_OHCI_STOP_FAIL") ? -1 : 0;
}
int bw_xbox_nv2a_stop(void)
{
    did_cleanup |= 1;
    return getenv("BW_TEST_NV2A_STOP_FAIL") ? -1 : 0;
}

void xbe_entry_point(void)
{
    const uint32_t packet[] = { (1u << 18) | 0x1d70u, 0x12345678u };
    uint32_t page = *(uint32_t *)(memory + XBOX_KERNEL_DATA_BASE +
                                  KDATA_LAUNCH_DATA_PAGE);
    const char *mode = getenv("BW_TEST_MODE");
    check(step == 11 && g_esp == XBOX_STACK_TOP &&
          pthread_equal(pthread_self(), guest_thread_id) &&
          !pthread_equal(pthread_self(), ui_thread));
    check(!strcmp(getenv("BW_GPU"), "metal") &&
          !strcmp(getenv("RECOMP_FB_WINDOW"), "1"));
    if (mode && (!strcmp(mode, "overrides") ||
                 !strcmp(mode, "overrides-empty")))
        check(!strcmp(getenv("RECOMP_KEYBOARD"), "0") &&
              !strcmp(getenv("RECOMP_FLIP_PACING"), "") &&
              !strcmp(getenv("RECOMP_PB_EXEC"),
                      !strcmp(mode, "overrides") ? "0" : "") &&
              !strcmp(getenv("RECOMP_VBLANK"),
                      !strcmp(mode, "overrides") ? "" : "0"));
    else
        check(!strcmp(getenv("RECOMP_KEYBOARD"), "1") &&
              !strcmp(getenv("RECOMP_FLIP_PACING"), "1") &&
              !strcmp(getenv("RECOMP_PB_EXEC"), "1") &&
              !strcmp(getenv("RECOMP_VBLANK"), "paced"));
    memcpy(memory + 0x20000, packet, sizeof(packet));
    nv2a_pb_scan_ring(0x20000, 0x20000 + sizeof(packet));
    check(pb_methods == 1 && *(uint32_t *)(memory + 0x21000) == packet[1]);
    if (mode && !strcmp(mode, "metal-runtime")) {
        atomic_store(&metal_error, 1);
        sleep(2);
        _exit(72);
    }
    if (mode && !strcmp(mode, "run-limit")) {
        sleep(3);
        _exit(72);
    }
    if (!page) {
        memcpy(memory + 0x10000, "D:\\default.xbe", 15);
        memcpy(memory + 0x11000, "SPLASH", 6);
        memory[0x11008] = 1;
        memory[0x1102a] = 0x7a;
        check(bw_macos_record_relaunch(0x10000, 0x11000) == 0);
    } else {
        check(page == 0x3000 &&
              !strcmp((char *)memory + page + 8, "\\Device\\CdRom0;default.xbe") &&
              memory[page + 0x400 + 8] == 1 &&
              memory[page + 0x400 + 42] == 0x7a);
    }
}

static void *run_guest(void *p)
{
    int (*fn)(void) = p;
    guest_thread_id = pthread_self();
    return (void *)(intptr_t)fn();
}

int xbox_HostWindowMain(int (*fn)(void))
{
    pthread_t thread;
    void *result;
    ui_thread = pthread_self();
    if (getenv("BW_TEST_EARLY_WINDOW_RETURN"))
        return 0;
    check(pthread_create(&thread, NULL, run_guest, (void *)fn) == 0);
    check(pthread_join(thread, &result) == 0);
    return (int)(intptr_t)result;
}

static int invoke(int argc, char **argv, const char *mode)
{
    setenv("BW_TEST_MODE", mode, 1);
    return bw_native_main(argc, argv);
}

static void unsafe_cleanup_returned(void)
{
    _exit(79);
}

static void check_scanner_requires_execution_switch(void)
{
    const uint32_t packet[] = { (1u << 18) | 0x1d70u, 0x12345678u };
    memory = calloc(1, 0x22000);
    check(memory != NULL);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    memcpy(memory + 0x20000, packet, sizeof(packet));
    nv2a_pb_scan_ring(0x20000, 0x20000 + sizeof(packet));
    check(pb_methods == 0 && *(uint32_t *)(memory + 0x21000) == 0);
    free(memory);
    memory = NULL;
    g_xbox_mem_offset = 0;
}

int main(int argc, char **argv)
{
    FILE *f;
    int rc;
    if (argc == 2 && !strcmp(argv[1], "--bw-native-child")) {
        int stop_fails = getenv("BW_TEST_OHCI_STOP_FAIL") ||
                         getenv("BW_TEST_NV2A_STOP_FAIL");
        if (stop_fails)
            check(atexit(unsafe_cleanup_returned) == 0);
        rc = bw_native_main(argc, argv);
        if (getenv("BW_TEST_METAL_START_FAIL") && !stop_fails)
            check((did_cleanup & (1 | 4 | 8)) == (1 | 4 | 8) &&
                  !(did_cleanup & (2 | 16)));
        return rc;
    }
    unsetenv("RECOMP_USB");
    unsetenv("RECOMP_AC97_READY");
    unsetenv("BW_GPU");
    unsetenv("RECOMP_FB_WINDOW");
    unsetenv("RECOMP_KEYBOARD");
    unsetenv("RECOMP_FLIP_PACING");
    unsetenv("RECOMP_PB_EXEC");
    unsetenv("RECOMP_PB_SCAN");
    unsetenv("RECOMP_VBLANK");
    unsetenv("BW_LAUNCH_DATA_FILE");
    unsetenv("BW_RUN_SECONDS");
    check_scanner_requires_execution_switch();
    setenv("BW_SETTINGS", "none", 1);
    setenv("BW_GAME_DIR", "bw-host-fixture", 1);
    setenv("BW_HOST_BOOTS", "2", 1);
    if (mkdir("bw-host-fixture", 0700) != 0)
        return 1;
    f = fopen("bw-host-fixture/default.xbe", "wb");
    if (!f || fwrite("fake", 1, 4, f) != 4 || fclose(f))
        return 1;
    if (invoke(argc, argv, "normal") != 0)
        return 2;
    setenv("RECOMP_KEYBOARD", "0", 1);
    setenv("RECOMP_FLIP_PACING", "", 1);
    setenv("RECOMP_PB_EXEC", "0", 1);
    setenv("RECOMP_VBLANK", "", 1);
    if (invoke(argc, argv, "overrides") != 0)
        return 9;
    setenv("RECOMP_PB_EXEC", "", 1);
    setenv("RECOMP_VBLANK", "0", 1);
    if (invoke(argc, argv, "overrides-empty") != 0)
        return 13;
    unsetenv("RECOMP_KEYBOARD");
    unsetenv("RECOMP_FLIP_PACING");
    unsetenv("RECOMP_PB_EXEC");
    unsetenv("RECOMP_VBLANK");
    setenv("BW_TEST_METAL_START_FAIL", "1", 1);
    if (invoke(argc, argv, "normal") == 0)
        return 3;
    setenv("BW_TEST_OHCI_STOP_FAIL", "1", 1);
    if (invoke(argc, argv, "normal") != 1)
        return 10;
    unsetenv("BW_TEST_OHCI_STOP_FAIL");
    setenv("BW_TEST_NV2A_STOP_FAIL", "1", 1);
    if (invoke(argc, argv, "normal") != 1)
        return 11;
    unsetenv("BW_TEST_NV2A_STOP_FAIL");
    unsetenv("BW_TEST_METAL_START_FAIL");
    setenv("BW_TEST_EARLY_WINDOW_RETURN", "1", 1);
    setenv("BW_TEST_NV2A_STOP_FAIL", "1", 1);
    if (invoke(argc, argv, "normal") != 1)
        return 12;
    unsetenv("BW_TEST_EARLY_WINDOW_RETURN");
    unsetenv("BW_TEST_NV2A_STOP_FAIL");
    if (invoke(argc, argv, "metal-runtime") == 0)
        return 4;
    setenv("BW_RUN_SECONDS", "1", 1);
    if (invoke(argc, argv, "run-limit") != 124)
        return 5;
    unsetenv("BW_RUN_SECONDS");
    setenv("RECOMP_USB", "0", 1);
    if (invoke(argc, argv, "normal") == 0)
        return 6;
    unsetenv("RECOMP_USB");
    setenv("RECOMP_AC97_READY", "", 1);
    if (invoke(argc, argv, "normal") == 0)
        return 7;
    unsetenv("RECOMP_AC97_READY");
    setenv("BW_GPU", "d3d11", 1);
    if (invoke(argc, argv, "normal") == 0)
        return 8;
    unlink("bw-host-fixture/default.xbe");
    rmdir("bw-host-fixture");
    puts("native startup and process lifetime: synthetic checks passed");
    return 0;
}
