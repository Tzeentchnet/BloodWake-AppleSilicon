#define _DARWIN_C_SOURCE 1
#include "bw_edition.h"
#include "bw_framelog.h"
#include "bw_macos_boot.h"
#ifdef BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK
#include "bw_macos_input_bridge.h"
#endif
#include "bw_posix_fault.h"
#include "bw_profile.h"
#include "bw_settings.h"
#include "bw_widescreen.h"
#include "gpu/bw_metal_backend.h"
#include "toolkit_patches/bw_runtime_lifecycle.h"
#include "toolkit_patches/video/bw_framebuffer_metal.h"

#include <xbox/xboxrecomp.h>
#include "video_player.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

extern RECOMP_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_OhciInit(void);
extern void bw_state_start(void);
extern void bw_debugflags_start(void);
extern unsigned bw_hifps_boot(int gameplay);
extern void bw_set_min_flip_interval(unsigned n);
extern int xbox_Nv2aMirrorCounter(uint32_t device_ptr_va, uint32_t src_off,
                                  uint32_t dst_off);
extern MCPXAPUState *g_apu_state;

typedef struct {
    BwMacosLaunch launch;
    int has_launch;
    int fault_installed;
    int memory_ready;
    int backend_ready;
    int kernel_ready;
    int guest_entered;
    int diagnostics_started;
} BwNativeBoot;

static BwNativeBoot s_boot;

static void *metal_monitor(void *arg)
{
    const struct timespec delay = {0, 50000000};
    (void)arg;
    for (;;) {
        const char *error = bw_metal_backend_error();
        if (error) {
            static const char prefix[] = "[BW] Metal runtime error: ";
            (void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
            (void)write(STDERR_FILENO, error, strlen(error));
            (void)write(STDERR_FILENO, "\n", 1);
            _exit(1);
        }
        nanosleep(&delay, NULL);
    }
}

static int start_metal_monitor(void)
{
    pthread_t thread;
    if (pthread_create(&thread, NULL, metal_monitor, NULL) != 0)
        return -1;
    pthread_detach(thread);
    return 0;
}

static const char *game_dir(void)
{
    const char *dir = getenv("BW_GAME_DIR");
    return dir && *dir ? dir : "game/disc";
}

static int env_default(const char *name, const char *value)
{
    return getenv(name) ? 0 : setenv(name, value, 0);
}

static int configure_native(void)
{
    const char *gpu = getenv("BW_GPU");
    const char *watch = getenv("RECOMP_WATCH");
    const char *keyboard_hook = getenv("BW_MACOS_KEYBOARD_HOOK");
    const char *synthetic_input = getenv("BW_SYNTHETIC_INPUT_DIAGNOSTIC");

    if (keyboard_hook && strcmp(keyboard_hook, "0") && strcmp(keyboard_hook, "1")) {
        fprintf(stderr, "[BW] BW_MACOS_KEYBOARD_HOOK must be 0 or 1\n");
        return -1;
    }
    if (synthetic_input && strcmp(synthetic_input, "0") && strcmp(synthetic_input, "1")) {
        fprintf(stderr, "[BW] BW_SYNTHETIC_INPUT_DIAGNOSTIC must be 0 or 1\n");
        return -1;
    }
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    if (synthetic_input && !strcmp(synthetic_input, "1")) {
        if (!keyboard_hook || strcmp(keyboard_hook, "1")) {
            fprintf(stderr, "[BW] synthetic input diagnostic requires the experimental keyboard hook\n");
            return -1;
        }
        bw_macos_input_bridge_trace_synthetic(1);
        fprintf(stderr, "[BW] synthetic SDL input diagnostic enabled; no physical keys are generated\n");
    }
#else
    if (synthetic_input && !strcmp(synthetic_input, "1")) {
        fprintf(stderr, "[BW] synthetic input diagnostic was not included in this build\n");
        return -1;
    }
#endif
#ifdef BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK
    bw_macos_input_bridge_configure(keyboard_hook && !strcmp(keyboard_hook, "1"));
    if (keyboard_hook && !strcmp(keyboard_hook, "1"))
        fprintf(stderr, "[BW] experimental game-specific keyboard hook enabled; "
                        "ABI and guest behavior are not fully verified\n");
#else
    if (keyboard_hook && !strcmp(keyboard_hook, "1")) {
        fprintf(stderr, "[BW] experimental keyboard hook was not included in this build\n");
        return -1;
    }
#endif

    if (gpu && strcasecmp(gpu, "metal")) {
        fprintf(stderr, "[BW] BW_GPU must be 'metal' on the native macOS host\n");
        return -1;
    }
    if (watch && *watch) {
        fprintf(stderr, "[BW] RECOMP_WATCH is not safe on POSIX and is unavailable\n");
        return -1;
    }
    /* These switches are presence-based in the toolkit, including "=0".
     * The APU and OHCI callbacks acquire host locks and cannot run in a
     * POSIX signal handler. A safe origin/lock-ownership protocol is not
     * available yet; refusing is preferable to deadlock or fake devices. */
    if (getenv("RECOMP_AC97_READY") || getenv("RECOMP_USB")) {
        fprintf(stderr, "[BW] audio/USB guest MMIO is unavailable on macOS: "
                        "lock-safe dispatch is not implemented\n");
        return -1;
    }
    if (env_default("BW_GPU", "metal") ||
        env_default("RECOMP_PB_EXEC", "1") ||
        env_default("RECOMP_FB_WINDOW", "1") ||
        env_default("RECOMP_KEYBOARD", "1") ||
        env_default("RECOMP_VBLANK", "paced") ||
        env_default("RECOMP_FLIP_PACING", "1") ||
        env_default("RECOMP_DISPATCH_GATE", "1")) {
        fprintf(stderr, "[BW] cannot configure native defaults\n");
        return -1;
    }
#ifdef BW_EXPERIMENTAL_MACOS_KEYBOARD_HOOK
    if (keyboard_hook && !strcmp(keyboard_hook, "1")) {
        const char *keyboard = getenv("RECOMP_KEYBOARD");
        if (!keyboard || !*keyboard || *keyboard == '0') {
            fprintf(stderr, "[BW] experimental keyboard hook requires RECOMP_KEYBOARD enabled\n");
            return -1;
        }
    }
#endif
#ifdef BW_EXPERIMENTAL_SYNTHETIC_INPUT
    if (synthetic_input && !strcmp(synthetic_input, "1")) {
        const char *window = getenv("RECOMP_FB_WINDOW");
        const char *headless = getenv("RECOMP_HEADLESS");
        if (!window || !*window || *window == '0' ||
            (headless && *headless && *headless != '0')) {
            fprintf(stderr, "[BW] synthetic input diagnostic requires a visible SDL window\n");
            return -1;
        }
    }
#endif
    fprintf(stderr, "[BW] audio and guest USB are disabled: safe trapped-MMIO "
                    "dispatch is not available\n");
    return 0;
}

static int load_xbe(void **data_out, size_t *size_out)
{
    char path[PATH_MAX];
    FILE *f;
    long length;
    void *data;
    size_t read_size;
    int n = snprintf(path, sizeof(path), "%s/default.xbe", game_dir());
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fprintf(stderr, "[BW] BW_GAME_DIR is too long\n");
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[BW] cannot open default.xbe under BW_GAME_DIR (or game/disc)\n");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (length = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "[BW] cannot determine the XBE size\n");
        return -1;
    }
    data = malloc((size_t)length);
    if (!data) {
        fclose(f);
        fprintf(stderr, "[BW] cannot allocate the XBE buffer\n");
        return -1;
    }
    read_size = fread(data, 1, (size_t)length, f);
    if (fclose(f) != 0 || read_size != (size_t)length) {
        free(data);
        fprintf(stderr, "[BW] cannot read the complete XBE\n");
        return -1;
    }
    *data_out = data;
    *size_out = (size_t)length;
    return 0;
}

static int set_map_size(void)
{
    const char *spec = getenv("BW_MAP_MB");
    char *end;
    long mb = 128;
    if (spec && *spec) {
        errno = 0;
        mb = strtol(spec, &end, 10);
        if (errno || *end || mb < 64 || mb > 1024) {
            fprintf(stderr, "[BW] BW_MAP_MB must be between 64 and 1024\n");
            return -1;
        }
    }
    if (mb > 64)
        xbox_SetMapSize((size_t)mb << 20);
    return 0;
}

static int install_launch_data(void)
{
    uint8_t page[4096];
    uint32_t va;
    uint8_t *base = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    size_t mapped = xbox_GetMappedSize();

    if (!s_boot.has_launch)
        return 0;
    if (bw_macos_launch_page(&s_boot.launch, page) != 0) {
        fprintf(stderr, "[BW] launch data does not target this title\n");
        return -1;
    }
    va = xbox_HeapAlloc(sizeof(page), 4096);
    if (!va || va > mapped || mapped - va < sizeof(page)) {
        fprintf(stderr, "[BW] cannot allocate the guest launch data page\n");
        return -1;
    }
    memcpy(base + va, page, sizeof(page));
    *(uint32_t *)(base + XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = va;
    return 0;
}

/* The presenter calls this on its guest pthread (or the main thread for an
 * explicitly headless run). Kernel bridge initialization and g_esp must be
 * on the thread that actually enters the lifted code. */
static int guest_main(void)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    char err[256];
    int gameplay;

    if (bw_edition_init(err, sizeof(err)) != 0) {
        fprintf(stderr, "[BW] %s\n", err);
        return 1;
    }
    bw_profile_init();
    bw_fl_init();
    bw_fl_thread(0);
    s_boot.diagnostics_started = 1;
    if (load_xbe(&xbe_data, &xbe_size) != 0 || set_map_size() != 0) {
        free(xbe_data);
        return 1;
    }
    if (bw_posix_fault_install() != 0) {
        fprintf(stderr, "[BW] cannot install the safe AC'97 fault handler\n");
        free(xbe_data);
        return 1;
    }
    s_boot.fault_installed = 1;
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "[BW] cannot initialize the Xbox memory layout\n");
        free(xbe_data);
        return 1;
    }
    s_boot.memory_ready = 1;
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    bw_posix_fault_set_base((uintptr_t)g_xbox_mem_offset);
    bw_edition_apply_image_fixes(xbe_data, xbe_size);
    free(xbe_data);
    bw_sdl_framebuffer_expect_metal();
    bw_metal_backend_set_framebuffer_callback(bw_sdl_framebuffer_prepare_metal);
    if (!bw_metal_backend_start()) {
        fprintf(stderr, "[BW] Metal initialization failed: %s\n",
                bw_metal_backend_error());
        return 1;
    }
    s_boot.backend_ready = 1;

    /* These are the real device constructors. configure_native refuses the
     * switches that would require unsafe APU/OHCI fault dispatch today. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        if (!g_apu_state) {
            fprintf(stderr, "[BW] cannot initialize the emulated APU\n");
            return 1;
        }
    }
    xbox_OhciInit();
    xbox_kernel_init();
    s_boot.kernel_ready = 1;
    xbox_path_init(game_dir(), bw_edition_prepare_saves());
    xbox_kernel_bridge_init();
    if (install_launch_data() != 0)
        return 1;
    gameplay = s_boot.has_launch && bw_macos_launch_is_gameplay(&s_boot.launch);
    if (gameplay && !bw_hifps_boot(gameplay)) {
        const char *hold = getenv("BW_GAMEPLAY_30");
        if (!(hold && hold[0] == '0'))
            bw_set_min_flip_interval(2);
    }
    bw_ws_init(gameplay, 1);
    if (!getenv("BW_NO_SWAP_MIRROR") &&
        xbox_Nv2aMirrorCounter(0x00136B00u, 0x2B60u, 0x2518u) != 0)
        fprintf(stderr, "[BW] GPU swap counter mirror could not be registered\n");
    g_esp = XBOX_STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BW] flat dispatch unavailable; using binary search\n");

    /* These diagnostics and the watchdog create persistent workers; start
     * them only after all fallible setup and never unmap while they can run. */
    if (start_metal_monitor() != 0) {
        fprintf(stderr, "[BW] cannot start the Metal runtime monitor\n");
        return 1;
    }
    bw_state_start();
    bw_debugflags_start();
    xbox_WatchdogStart();
    s_boot.guest_entered = 1;
    xbe_entry_point();
    return 0;
}

static void finish_diagnostics(const char *why)
{
    if (s_boot.diagnostics_started) {
        bw_fl_dump(why);
        bw_profile_dump(why);
        bw_edition_summary();
    }
}

/* There is no toolkit API to join every guest-created, timer and diagnostic
 * thread. After guest entry the process is therefore the lifetime boundary:
 * do not destroy devices or unmap memory under a potentially live worker. */
static int cleanup_before_guest(void)
{
    if (s_boot.guest_entered)
        return 0;
    if (bw_xbox_ohci_stop() != 0 || bw_xbox_nv2a_stop() != 0) {
        static const char message[] =
            "[BW] device worker did not stop; leaving resources to process exit\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        return -1;
    }
    finish_diagnostics("startup failed");
    if (g_apu_state) {
        mcpx_apu_shutdown(g_apu_state);
        g_apu_state = NULL;
    }
    if (s_boot.backend_ready)
        bw_metal_backend_stop();
    if (s_boot.kernel_ready)
        xbox_kernel_shutdown();
    if (s_boot.memory_ready) {
        bw_posix_fault_set_base(0);
        xbox_MemoryLayoutShutdown();
    }
    if (s_boot.fault_installed)
        bw_posix_fault_uninstall();
    return 0;
}

static void *run_limit_thread(void *arg)
{
    struct timespec remaining = {(time_t)(uintptr_t)arg, 0};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
    {
        static const char message[] = "[BW] run limit reached\n";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    }
    _exit(124);
}

static int start_run_limit(void)
{
    const char *spec = getenv("BW_RUN_SECONDS");
    char *end;
    unsigned long secs;
    pthread_t thread;
    if (!spec || !*spec)
        return 0;
    errno = 0;
    secs = strtoul(spec, &end, 10);
    if (errno || *end || secs > 604800) {
        fprintf(stderr, "[BW] BW_RUN_SECONDS must be between 0 and 604800\n");
        return -1;
    }
    if (!secs)
        return 0;
    if (pthread_create(&thread, NULL, run_limit_thread, (void *)(uintptr_t)secs) != 0)
        return -1;
    pthread_detach(thread);
    return 0;
}

int main(int argc, char **argv)
{
    int rc, cleanup_failed;
    if (argc == 2 && !strcmp(argv[1], "--settings-schema")) {
        bw_settings_print_schema(stdout);
        return ferror(stdout) ? 1 : 0;
    }
    bw_settings_apply();
    if (argc != 2 || strcmp(argv[1], "--bw-native-child"))
        return bw_macos_supervise(argc, argv);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (bw_macos_boot_receive(&s_boot.launch, &s_boot.has_launch) != 0) {
        fprintf(stderr, "[BW] invalid native supervisor launch data\n");
        return 1;
    }
    if (configure_native() != 0 || start_run_limit() != 0)
        return 1;
    rc = xbox_HostWindowMain(guest_main);
    if (s_boot.guest_entered)
        finish_diagnostics("guest return");
    cleanup_failed = cleanup_before_guest() != 0;
    if (cleanup_failed)
        _exit(rc ? rc : 1);
    fflush(stdout);
    fflush(stderr);
    /* A returned guest can have detached workers. _exit avoids atexit
     * destructors racing their references; the OS reclaims the whole boot. */
    if (s_boot.guest_entered)
        _exit(rc);
    return rc;
}
