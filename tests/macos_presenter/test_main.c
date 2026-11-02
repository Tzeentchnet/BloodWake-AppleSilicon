#include <SDL.h>
#include <SDL_metal.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../src/toolkit_patches/video/bw_framebuffer_metal.h"

#if defined(BW_REAL_SETTINGS_PANEL_TEST)
#include <limits.h>
#include <sys/stat.h>

#include "keyboard.h"
#include "../../src/bw_edition.h"
#include "../../src/bw_macos_mouse.h"
#include "../../src/bw_macos_settings_panel.h"
#endif

#define WIDTH 640u
#define HEIGHT 480u
#define PITCH32 (WIDTH * 4u)
#define PITCH16 (WIDTH * 2u)
#define FRAME_AT 0x1000u
#define CONTIG_AT 0x180000u
#define MEMORY_SIZE 0x400000u

int xbox_HostWindowMain(int (*guest_main)(void));
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch);
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch);
void xbox_FramebufferWindowSetTitle(const uint16_t *title, int max_chars);
int bw_test_presenter_running(void);

typedef struct {
    atomic_int errors, guest_calls, init_calls, input_ready, pump;
    atomic_int quit_calls, window_destroyed, renderer_destroyed, texture_destroyed;
    atomic_int key_events, focus_events, controller_events, updates, presents;
    atomic_int f1_events, settings_regular_events;
    atomic_int panel_open, panel_toggles, panel_opens, panel_closes, panel_close_calls;
    atomic_int panel_notifications, panel_open_notifications, panel_close_notifications;
    atomic_int relative_mode, relative_enables, relative_disables;
    atomic_int gamma_calls, gamma_entered, gamma_release, producer_done;
    atomic_int worker_active, worker_stop, worker_ticks, worker_violation;
    atomic_int memory_dead, late_memory_access, atexit_called;
    atomic_int texture_creations, texture_width, texture_height;
    atomic_int expected_width, expected_height;
    atomic_int metal_attach, metal_resize;
    atomic_uint uploaded_pixel, uploaded_last, gamma_input_first;
    atomic_uint uploaded_pixels[6];
} Shared;

static Shared *s;
static const char *s_case;
static pthread_t s_main, s_worker, s_producer, s_releaser;
static int s_worker_created, s_producer_created, s_releaser_created;
static _Alignas(16384) uint8_t s_memory[MEMORY_SIZE];
static int s_window, s_renderer, s_texture;
static int s_metal_view;
static atomic_int s_sdl_active;
static Uint32 s_done_event, s_frame_event;

static pthread_mutex_t s_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_queue_ready = PTHREAD_COND_INITIALIZER;
static SDL_Event s_queue[128];
static unsigned s_queue_head, s_queue_tail;

static int is_case(const char *name) { return !strcmp(s_case, name); }
static int metal_case(void) { return !strncmp(s_case, "metal-", 6); }

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s\n", s_case, message);
        atomic_fetch_add(&s->errors, 1);
    }
}

static void check_main(void)
{
    check(pthread_equal(pthread_self(), s_main), "SDL/input call left the main thread");
}

#if defined(BW_REAL_SETTINGS_PANEL_TEST)
const bw_fix_info_t g_bw_fixes[BW_FIX_COUNT + 1] = {{0}};
unsigned char g_bw_fix_on[BW_FIX_COUNT + 1];

int _stricmp(const char *left, const char *right)
{
    return strcasecmp(left, right);
}

int _strnicmp(const char *left, const char *right, size_t count)
{
    return strncasecmp(left, right, count);
}

const char *bw_edition_name(bw_edition_t edition)
{
    return edition == BW_ED_REMASTERED ? "remastered" : "original";
}

const char *bw_edition_title(bw_edition_t edition)
{
    return edition == BW_ED_REMASTERED ? "Remastered" : "Original";
}

int bw_edition_parse(const char *value, bw_edition_t *out)
{
    if (!out)
        return -1;
    if (!value || !*value || !strcmp(value, "original")) {
        *out = BW_ED_ORIGINAL;
        return 0;
    }
    if (!strcmp(value, "remastered")) {
        *out = BW_ED_REMASTERED;
        return 0;
    }
    return -1;
}

typedef struct {
    char current[32];
    char pending[32];
    int apply_calls;
    int fail_apply;
} PanelRuntimeSetting;

static PanelRuntimeSetting s_panel_lod = {"far", "", 0, 0};
static PanelRuntimeSetting s_panel_af = {"16", "", 0, 0};
static char s_panel_root[PATH_MAX];
static char s_panel_settings[PATH_MAX];
static char s_panel_bad_settings[PATH_MAX];
static char s_panel_bad_parent[PATH_MAX];
static char s_panel_temp[PATH_MAX];
static ino_t s_panel_initial_inode;
static int s_panel_setup;

static int panel_file_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    char contents[8192];
    size_t size;
    if (!file)
        return 0;
    size = fread(contents, 1, sizeof contents - 1, file);
    fclose(file);
    contents[size] = 0;
    return strstr(contents, needle) != NULL;
}

static int panel_write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    int wrote, closed;
    if (!file)
        return 0;
    wrote = fputs(text, file) >= 0;
    closed = fclose(file) == 0;
    return wrote && closed;
}

static int panel_runtime_get(char *value, size_t value_len, void *context,
                             char *err, size_t errlen)
{
    PanelRuntimeSetting *setting = context;
    int written;
    if (!value || !value_len) {
        if (err && errlen)
            snprintf(err, errlen, "synthetic current-value buffer is too small");
        return -1;
    }
    written = snprintf(value, value_len, "%s", setting->current);
    if (written < 0 || (size_t)written >= value_len) {
        if (err && errlen)
            snprintf(err, errlen, "synthetic current-value buffer is too small");
        return -1;
    }
    return 0;
}

static int panel_lod_apply(const char *value, void *context,
                           char *err, size_t errlen)
{
    PanelRuntimeSetting *setting = context;
    setting->apply_calls++;
    if (setting->fail_apply) {
        if (err && errlen)
            snprintf(err, errlen, "synthetic live LOD rejection");
        return -1;
    }
    snprintf(setting->current, sizeof setting->current, "%s", value);
    return 0;
}

static int panel_af_request(const char *value, void *context,
                            char *err, size_t errlen)
{
    PanelRuntimeSetting *setting = context;
    (void)err;
    (void)errlen;
    setting->apply_calls++;
    snprintf(setting->pending, sizeof setting->pending, "%s", value);
    return 0;
}

static void panel_check_first_open(void)
{
    bw_macos_settings_panel_test_state_t state;
    bw_macos_settings_panel_test_row_t row;
    struct stat current;
    char error[1024] = {0};

    check(bw_macos_settings_panel_test_state(&state) == 0,
          "test seam reads the real AppKit controller");
    check(state.controller_created && state.panel_created && state.visible,
          "F1 creates and shows a real NSPanel");
    check(state.main_thread, "the AppKit panel is owned by the process main thread");
    check(state.row_count == bw_settings_count(),
          "the panel creates one control for every schema setting");
    check(state.no_pause_notice,
          "the panel keeps the explicit gameplay-continues no-pause policy");
    check(xbox_FramebufferKeyDown('A'),
          "guest input is held before the presenter releases focus");
    check(xbox_FramebufferKeyDown(XBOX_VK_RBUTTON) &&
          bw_macos_mouse_captured() && atomic_load(&s->relative_mode),
          "relative mouse capture is active before the F1 release path");

    check(bw_macos_settings_panel_test_row("graphics", "antialiasing", &row) == 0,
          "unsupported Metal control is present");
    check(!row.enabled &&
          row.apply_kind == BW_MACOS_SETTINGS_UNSUPPORTED &&
          strstr(row.status, "Unavailable") &&
          strstr(row.status, "Metal MSAA is not implemented") &&
          strstr(row.support_reason, "Metal MSAA is not implemented"),
          "unsupported control is disabled with its schema reason visible");

    check(bw_macos_settings_panel_test_row("graphics", "draw_distance", &row) == 0 &&
          row.enabled && row.apply_kind == BW_MACOS_SETTINGS_LIVE &&
          !strcmp(row.value, "far") &&
          strstr(row.status, "applies immediately"),
          "live LOD control reflects its callback and apply mode");
    check(bw_macos_settings_panel_test_select("graphics", "draw_distance",
                                              "farther") == 0,
          "LOD choice uses the actual AppKit control action");
    check(s_panel_lod.apply_calls == 1 &&
          !strcmp(s_panel_lod.current, "farther"),
          "live LOD callback receives the selected value");
    check(bw_macos_settings_panel_test_row("graphics", "draw_distance", &row) == 0 &&
          !strcmp(row.value, "farther") &&
          strstr(row.status, "applied immediately"),
          "successful live apply is visible in the row");
    check(stat(s_panel_settings, &current) == 0 &&
          current.st_ino != s_panel_initial_inode,
          "successful save atomically replaces the settings file");
    check(access(s_panel_temp, F_OK) != 0 &&
          panel_file_contains(s_panel_settings, "draw_distance=farther") &&
          panel_file_contains(s_panel_settings, "SYNTHETIC_KEEP=preserved"),
          "atomic save leaves no temporary file and preserves unknown settings");

    check(bw_macos_settings_panel_test_row("graphics", "texture_filtering", &row) == 0 &&
          row.enabled && row.apply_kind == BW_MACOS_SETTINGS_FRAME &&
          !strcmp(row.value, "16") &&
          strstr(row.status, "next frame"),
          "anisotropy control is available as frame-boundary state");
    check(bw_macos_settings_panel_test_select("graphics", "texture_filtering",
                                              "8") == 0,
          "anisotropy choice uses the actual AppKit control action");
    check(s_panel_af.apply_calls == 1 &&
          !strcmp(s_panel_af.pending, "8") &&
          !strcmp(s_panel_af.current, "16"),
          "frame-boundary callback records a request without mutating active AF");
    check(bw_macos_settings_panel_test_row("graphics", "texture_filtering", &row) == 0 &&
          strstr(row.status, "queued for the next frame"),
          "frame-boundary request is visibly queued");

    check(bw_macos_settings_panel_test_row("display", "mode", &row) == 0 &&
          row.enabled && row.apply_kind == BW_MACOS_SETTINGS_RESTART &&
          strstr(row.status, "Restart required"),
          "restart-required control is available");
    check(bw_macos_settings_panel_test_select("display", "mode",
                                              "fullscreen") == 0 &&
          bw_macos_settings_panel_test_row("display", "mode", &row) == 0 &&
          !strcmp(row.value, "fullscreen") &&
          strstr(row.status, "restart required"),
          "restart-required choice remains visibly pending");

    check(setenv("BW_SETTINGS", s_panel_bad_settings, 1) == 0,
          "install isolated failing settings path");
    check(bw_macos_settings_panel_test_select("display", "mode", "window") == 0,
          "save failure is exercised through the AppKit control");
    check(bw_macos_settings_panel_test_row("display", "mode", &row) == 0 &&
          !strcmp(row.value, "fullscreen"),
          "failed save restores the last persisted selection");
    check(bw_macos_settings_panel_test_visible_error(error, sizeof error) &&
          (strstr(error, "could not read settings file") ||
           strstr(error, "could not create temporary settings file")),
          "save failure is presented as an attached AppKit error sheet");
    bw_macos_settings_panel_test_dismiss_error();
    check(!bw_macos_settings_panel_test_visible_error(error, sizeof error),
          "save error sheet can be dismissed without user interaction");
    check(setenv("BW_SETTINGS", s_panel_settings, 1) == 0,
          "restore isolated writable settings path");

    s_panel_lod.fail_apply = 1;
    check(bw_macos_settings_panel_test_select("graphics", "draw_distance",
                                              "4") == 0,
          "live apply failure is exercised through the AppKit control");
    check(s_panel_lod.apply_calls == 2 &&
          !strcmp(s_panel_lod.current, "farther"),
          "failed live callback leaves the active LOD unchanged");
    check(bw_macos_settings_panel_test_row("graphics", "draw_distance", &row) == 0 &&
          !strcmp(row.value, "4") &&
          strstr(row.status, "live apply failed"),
          "saved-but-not-applied state remains visible in the row");
    memset(error, 0, sizeof error);
    check(bw_macos_settings_panel_test_visible_error(error, sizeof error) &&
          strstr(error, "Saved for the next launch") &&
          strstr(error, "synthetic live LOD rejection"),
          "apply failure is presented as an attached AppKit error sheet");
    check(panel_file_contains(s_panel_settings, "draw_distance=4") &&
          access(s_panel_temp, F_OK) != 0,
          "apply failure still leaves the atomic save available for restart");
    bw_macos_settings_panel_test_dismiss_error();
    s_panel_lod.fail_apply = 0;
}

static void panel_lifecycle_changed(int open, void *context)
{
    bw_macos_settings_panel_test_state_t state;
    bw_macos_settings_panel_test_row_t row;
    int opens;
    (void)context;
    check_main();
    atomic_fetch_add(&s->panel_notifications, 1);
    if (open) {
        opens = atomic_fetch_add(&s->panel_open_notifications, 1) + 1;
        if (opens == 1) {
            panel_check_first_open();
        } else if (opens == 2) {
            check(bw_macos_settings_panel_test_state(&state) == 0 &&
                  state.panel_created && state.visible && state.no_pause_notice,
                  "second F1 open creates and shows the real panel again");
            check(xbox_FramebufferKeyDown('C'),
                  "guest key is held until the second F1 focus release");
            check(bw_macos_settings_panel_test_row("display", "mode", &row) == 0 &&
                  !strcmp(row.value, "fullscreen"),
                  "reopened panel reads the restart-pending persisted value");
            check(bw_macos_settings_panel_test_row("graphics", "draw_distance", &row) == 0 &&
                  !strcmp(row.value, "farther"),
                  "reopened live control reflects active LOD after failed apply");
            check(bw_macos_settings_panel_test_row("graphics", "texture_filtering", &row) == 0 &&
                  !strcmp(row.value, "16"),
                  "reopened frame control reflects active AF before frame activation");
        } else {
            check(0, "unexpected extra panel open");
        }
    } else {
        atomic_fetch_add(&s->panel_close_notifications, 1);
        check(bw_macos_settings_panel_test_state(&state) == 0 &&
              state.controller_created && !state.panel_created && !state.visible,
              "second non-repeat F1 closes and releases the real NSPanel");
        check(!xbox_FramebufferKeyDown('A') &&
              !xbox_FramebufferKeyDown('B') &&
              !xbox_FramebufferKeyDown(XBOX_VK_RBUTTON),
              "opening F1 released held guest input and blocked panel-time input");
        check(!bw_macos_mouse_captured() &&
              !atomic_load(&s->relative_mode) &&
              atomic_load(&s->relative_disables) == 1,
              "opening F1 released relative mouse capture");
    }
}

static void *panel_toggle_off_main(void *arg)
{
    int *result = arg;
    *result = bw_macos_settings_panel_toggle();
    return NULL;
}

static int panel_window_server_available(void)
{
    return bw_macos_settings_panel_test_window_server_available();
}

static int panel_integration_setup(void)
{
    struct timespec stamp;
    struct stat initial;
    pthread_t thread;
    char cwd[PATH_MAX];
    char error[512] = {0};
    int off_main_result = 0;

    if (!getcwd(cwd, sizeof cwd) ||
        clock_gettime(CLOCK_REALTIME, &stamp) != 0)
        return 0;
    if (snprintf(s_panel_root, sizeof s_panel_root,
                 "%s/presenter-panel-%lu-%ld", cwd,
                 (unsigned long)getpid(), stamp.tv_nsec) >=
        (int)sizeof s_panel_root ||
        snprintf(s_panel_settings, sizeof s_panel_settings,
                 "%s/settings.ini", s_panel_root) >=
        (int)sizeof s_panel_settings ||
        snprintf(s_panel_bad_parent, sizeof s_panel_bad_parent,
                 "%s/not-a-directory", s_panel_root) >=
        (int)sizeof s_panel_bad_parent ||
        snprintf(s_panel_bad_settings, sizeof s_panel_bad_settings,
                 "%s/settings.ini", s_panel_bad_parent) >=
        (int)sizeof s_panel_bad_settings ||
        snprintf(s_panel_temp, sizeof s_panel_temp, "%s.tmp.%lu",
                 s_panel_settings, (unsigned long)getpid()) >=
        (int)sizeof s_panel_temp)
        return 0;
    if (mkdir(s_panel_root, 0700) != 0)
        return 0;
    s_panel_setup = 1;
    if (!panel_write_text(s_panel_settings,
                          "[advanced]\nSYNTHETIC_KEEP=preserved\n") ||
        !panel_write_text(s_panel_bad_parent, "not a directory") ||
        stat(s_panel_settings, &initial) != 0)
        return 0;
    s_panel_initial_inode = initial.st_ino;
    if (setenv("BW_SETTINGS", s_panel_settings, 1) != 0 ||
        setenv("BW_MOUSE", "1", 1) != 0 ||
        setenv("BW_MOUSE_CAPTURE", "hold", 1) != 0 ||
        bw_macos_mouse_init_from_env(error, sizeof error) != 0)
        return 0;
    if (bw_settings_runtime_register("graphics", "draw_distance",
                                     panel_runtime_get, panel_lod_apply,
                                     &s_panel_lod, error, sizeof error) != 0 ||
        bw_settings_runtime_register("graphics", "texture_filtering",
                                     panel_runtime_get, panel_af_request,
                                     &s_panel_af, error, sizeof error) != 0)
        return 0;
    bw_macos_settings_panel_test_set_lifecycle_callback(
        panel_lifecycle_changed, NULL);
    if (pthread_create(&thread, NULL, panel_toggle_off_main,
                       &off_main_result) != 0)
        return 0;
    pthread_join(thread, NULL);
    check(off_main_result == -1,
          "off-main panel lifecycle calls are rejected");
    return 1;
}

static void panel_integration_cleanup(void)
{
    if (s_panel_setup) {
        unlink(s_panel_temp);
        unlink(s_panel_settings);
        unlink(s_panel_bad_parent);
        rmdir(s_panel_root);
    }
    unsetenv("BW_SETTINGS");
    unsetenv("BW_MOUSE");
    unsetenv("BW_MOUSE_CAPTURE");
}
#endif

#if !defined(BW_REAL_SETTINGS_PANEL_TEST)
int bw_macos_settings_panel_toggle(void)
{
    int was_open;
    check_main();
    atomic_fetch_add(&s->panel_toggles, 1);
    was_open = atomic_load(&s->panel_open);
    atomic_store(&s->panel_open, !was_open);
    atomic_fetch_add(was_open ? &s->panel_closes : &s->panel_opens, 1);
    return !was_open;
}

int bw_macos_settings_panel_is_open(void)
{
    check_main();
    return atomic_load(&s->panel_open);
}

void bw_macos_settings_panel_close(void)
{
    check_main();
    atomic_fetch_add(&s->panel_close_calls, 1);
    if (atomic_exchange(&s->panel_open, 0))
        atomic_fetch_add(&s->panel_closes, 1);
}
#endif

int bw_metal_backend_attach_presenter(void *layer, uint32_t width,
                                      uint32_t height, int vsync)
{
    check(layer != NULL, "direct presenter receives a Metal layer");
    check(width == WIDTH * 2u && height == HEIGHT * 2u,
          "direct presenter receives drawable pixels");
    check(vsync == 1, "direct presenter receives vsync setting");
    atomic_fetch_add(&s->metal_attach, 1);
    return 1;
}

int bw_metal_backend_resize_presenter(uint32_t width, uint32_t height)
{
    check(width == WIDTH * 2u && height == HEIGHT * 2u,
          "direct presenter resize receives drawable pixels");
    atomic_fetch_add(&s->metal_resize, 1);
    return 1;
}

static void sleep_ms(unsigned ms)
{
    struct timespec delay = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static int wait_at_least(atomic_int *value, int target)
{
    for (unsigned i = 0; i < 5000; i++) {
        if (atomic_load(value) >= target)
            return 1;
        sleep_ms(1);
    }
    return 0;
}

static void late_access(void)
{
    if (atomic_load(&s->memory_dead))
        atomic_store(&s->late_memory_access, 1);
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    late_access();
    if (is_case("late-present-error")) {
        atomic_store(&s->gamma_entered, 1);
        while (!atomic_load(&s->gamma_release))
            sched_yield();
    }
    return (ptrdiff_t)(uintptr_t)s_memory;
}

void *xbox_GetMemoryBase(void)
{
    late_access();
    return s_memory;
}

size_t xbox_GetMappedSize(void)
{
    late_access();
    return is_case("frame-contig") || is_case("metal-contig") ? 0x100000u : sizeof s_memory;
}

int bw_xbox_contiguous_memory_region(void **host, uint32_t *va, size_t *bytes)
{
    late_access();
    if (!is_case("frame-contig") && !is_case("metal-contig"))
        return 0;
    *host = s_memory + CONTIG_AT;
    *va = CONTIG_AT;
    *bytes = 0x180000u;
    return 1;
}

void bw_gamma_apply_xrgb(uint32_t *pixels, size_t count)
{
    size_t expected = metal_case()
                    ? (size_t)atomic_load(&s->expected_width) * atomic_load(&s->expected_height)
                    : WIDTH * HEIGHT;
    check(count == expected, "gamma receives the complete copied frame");
    atomic_fetch_add(&s->gamma_calls, 1);
    if (is_case("blocked-copy")) {
        atomic_store(&s->gamma_entered, 1);
        while (!atomic_load(&s->gamma_release))
            sched_yield();
    }
    atomic_store(&s->gamma_input_first, pixels[0]);
    pixels[0] = 0xFFABCDEFu;
}

void xbox_InputSetMainPump(int on)
{
    check_main();
    atomic_store(&s->pump, on);
}

#if !defined(BW_REAL_SETTINGS_PANEL_TEST)
void xbox_InputSdlInit(void)
{
    check_main();
    check(atomic_load(&s->pump) == 1, "main pump set before input enumeration");
    sleep_ms(10);
    atomic_store(&s->input_ready, 1);
}

void xbox_InputSdlEvent(const SDL_Event *event)
{
    check_main();
    if (event->type == SDL_KEYDOWN) {
        atomic_fetch_add(&s->key_events, 1);
        if (event->key.keysym.sym == SDLK_F1)
            atomic_fetch_add(&s->f1_events, 1);
        if (event->key.keysym.sym == SDLK_a ||
            event->key.keysym.sym == SDLK_b ||
            event->key.keysym.sym == SDLK_c)
            atomic_fetch_add(&s->settings_regular_events, 1);
    }
    if (event->type == SDL_KEYUP && event->key.keysym.sym == SDLK_F1)
        atomic_fetch_add(&s->f1_events, 1);
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
        atomic_fetch_add(&s->focus_events, 1);
    if (event->type == SDL_CONTROLLERDEVICEADDED)
        atomic_fetch_add(&s->controller_events, 1);
}
#else
void xbox_InputInit(void)
{
    check_main();
    check(atomic_load(&s->pump) == 1,
          "main pump set before input enumeration");
    sleep_ms(10);
    atomic_store(&s->input_ready, 1);
}

void bw_InputRefreshControllers(void)
{
    check_main();
}
#endif

int bw_test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                           void *(*entry)(void *), void *arg)
{
    if (is_case("thread-fail"))
        return EAGAIN;
    return pthread_create(thread, attr, entry, arg);
}

SDL_bool SDLCALL SDL_SetHint(const char *name, const char *value)
{
    (void)name;
    (void)value;
    check_main();
    return SDL_TRUE;
}

int SDLCALL SDL_Init(Uint32 flags)
{
    check_main();
    check((flags & (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER)) ==
          (SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER), "SDL subsystem mask");
    atomic_fetch_add(&s->init_calls, 1);
    if (is_case("init-fail"))
        return -1;
    atomic_store(&s_sdl_active, 1);
    return 0;
}

const char * SDLCALL SDL_GetCurrentVideoDriver(void)
{
    check_main();
    return is_case("dummy-driver") ? "dummy"
         : is_case("offscreen-driver") ? "offscreen" : "cocoa";
}

const char * SDLCALL SDL_GetError(void) { return "injected SDL failure"; }

#if defined(BW_REAL_SETTINGS_PANEL_TEST)
void SDLCALL SDL_StopTextInput(void)
{
    check_main();
}

int SDLCALL SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    check_main();
    atomic_store(&s->relative_mode, enabled == SDL_TRUE);
    atomic_fetch_add(enabled == SDL_TRUE ? &s->relative_enables
                                        : &s->relative_disables, 1);
    return 0;
}

int SDLCALL SDL_ShowCursor(int toggle)
{
    check_main();
    check(toggle == (atomic_load(&s->relative_mode) ? SDL_DISABLE : SDL_ENABLE),
          "cursor visibility follows relative mouse capture");
    return toggle;
}
#endif

SDL_Window * SDLCALL SDL_CreateWindow(const char *title, int x, int y, int w, int h,
                                      Uint32 flags)
{
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    check_main();
    return is_case("window-fail") ? NULL : (SDL_Window *)&s_window;
}

SDL_MetalView SDLCALL SDL_Metal_CreateView(SDL_Window *window)
{
    check_main();
    check(window == (SDL_Window *)&s_window, "Metal view uses the created window");
    return (SDL_MetalView)&s_metal_view;
}

void SDLCALL SDL_Metal_DestroyView(SDL_MetalView view)
{
    check_main();
    check(view == (SDL_MetalView)&s_metal_view, "destroy the created Metal view");
}

void * SDLCALL SDL_Metal_GetLayer(SDL_MetalView view)
{
    check_main();
    check(view == (SDL_MetalView)&s_metal_view, "get the created Metal layer");
    return &s_metal_view;
}

void SDLCALL SDL_Metal_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
    check_main();
    check(window == (SDL_Window *)&s_window, "query the created Metal window");
    if (w) *w = (int)WIDTH * 2;
    if (h) *h = (int)HEIGHT * 2;
}

Uint32 SDLCALL SDL_GetWindowFlags(SDL_Window *window)
{
    check_main();
    check(window == (SDL_Window *)&s_window, "window flags use the created window");
    return is_case("hidden-window") ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
}

SDL_Renderer * SDLCALL SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
    (void)window; (void)index; (void)flags;
    check_main();
    return is_case("renderer-fail") ? NULL : (SDL_Renderer *)&s_renderer;
}

int SDLCALL SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h)
{
    (void)renderer;
    check_main();
    check(w == (int)WIDTH && h == (int)HEIGHT, "logical framebuffer dimensions");
    return is_case("logical-size-fail") ? -1 : 0;
}

int SDLCALL SDL_GetRendererInfo(SDL_Renderer *renderer, SDL_RendererInfo *info)
{
    (void)renderer;
    check_main();
    memset(info, 0, sizeof *info);
    info->name = "synthetic";
    return 0;
}

Uint32 SDLCALL SDL_RegisterEvents(int count)
{
    check_main();
    check(count == 2, "register completion and frame events");
    if (is_case("event-fail"))
        return (Uint32)-1;
    s_done_event = SDL_USEREVENT + 10;
    s_frame_event = s_done_event + 1;
    return s_done_event;
}

static int take_event(SDL_Event *event)
{
    if (s_queue_head == s_queue_tail)
        return 0;
    *event = s_queue[s_queue_head++ % 128];
    return 1;
}

int SDLCALL SDL_WaitEventTimeout(SDL_Event *event, int timeout)
{
    struct timespec until;
    int result;
    check_main();
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout / 1000;
    until.tv_nsec += (long)(timeout % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_queue_lock);
    while (s_queue_head == s_queue_tail) {
        if (pthread_cond_timedwait(&s_queue_ready, &s_queue_lock, &until) == ETIMEDOUT)
            break;
    }
    result = take_event(event);
    pthread_mutex_unlock(&s_queue_lock);
    return result;
}

int SDLCALL SDL_PollEvent(SDL_Event *event)
{
    int result;
    check_main();
    pthread_mutex_lock(&s_queue_lock);
    result = take_event(event);
    pthread_mutex_unlock(&s_queue_lock);
    return result;
}

int SDLCALL SDL_PushEvent(SDL_Event *event)
{
    if (is_case("lost-completion") && event->type == s_done_event)
        return 0;
    pthread_mutex_lock(&s_queue_lock);
    if (s_queue_tail - s_queue_head >= 128) {
        pthread_mutex_unlock(&s_queue_lock);
        return 0;
    }
    s_queue[s_queue_tail++ % 128] = *event;
    pthread_cond_signal(&s_queue_ready);
    pthread_mutex_unlock(&s_queue_lock);
    return 1;
}

SDL_Texture * SDLCALL SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access,
                                        int w, int h)
{
    (void)renderer; (void)access;
    check_main();
    check(w == (metal_case() ? atomic_load(&s->expected_width) : (int)WIDTH) &&
          h == (metal_case() ? atomic_load(&s->expected_height) : (int)HEIGHT),
          "texture dimensions");
    check(format == SDL_PIXELFORMAT_RGB888 || format == SDL_PIXELFORMAT_RGB565,
          "supported texture format");
    if (metal_case())
        check(format == SDL_PIXELFORMAT_RGB888, "Metal snapshots use RGB888 upload");
    atomic_store(&s->texture_width, w);
    atomic_store(&s->texture_height, h);
    atomic_fetch_add(&s->texture_creations, 1);
    return is_case("texture-fail") ? NULL : (SDL_Texture *)&s_texture;
}

int SDLCALL SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
                              const void *pixels, int pitch)
{
    uint32_t first = 0;
    (void)texture; (void)rect;
    check_main();
    if (is_case("update-fail"))
        return -1;
    memcpy(&first, pixels, pitch == (int)PITCH16 ? sizeof(uint16_t) : sizeof first);
    atomic_store(&s->uploaded_pixel, first);
    if (metal_case()) {
        int width = atomic_load(&s->texture_width);
        int height = atomic_load(&s->texture_height);
        const uint32_t *p = pixels;
        size_t count = (size_t)width * height;
        check(pitch == width * 4, "Metal texture pitch");
        for (size_t i = 0; i < count && i < 6; i++)
            atomic_store(&s->uploaded_pixels[i], p[i]);
        atomic_store(&s->uploaded_last, p[count - 1]);
    }
    atomic_fetch_add(&s->updates, 1);
    return 0;
}

int SDLCALL SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    (void)renderer; (void)r; (void)g; (void)b; (void)a;
    check_main();
    return is_case("render-fail") ? -1 : 0;
}

int SDLCALL SDL_RenderClear(SDL_Renderer *renderer)
{
    (void)renderer;
    check_main();
    return 0;
}

int SDLCALL SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                            const SDL_Rect *src, const SDL_Rect *dst)
{
    (void)renderer; (void)texture; (void)src; (void)dst;
    check_main();
    return 0;
}

void SDLCALL SDL_RenderPresent(SDL_Renderer *renderer)
{
    (void)renderer;
    check_main();
    atomic_fetch_add(&s->presents, 1);
}

void SDLCALL SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
    (void)window; (void)title;
    check_main();
}

void SDLCALL SDL_DestroyTexture(SDL_Texture *texture)
{
    (void)texture;
    check_main();
    atomic_fetch_add(&s->texture_destroyed, 1);
}

void SDLCALL SDL_DestroyRenderer(SDL_Renderer *renderer)
{
    (void)renderer;
    check_main();
    atomic_fetch_add(&s->renderer_destroyed, 1);
    if (atomic_load(&s->worker_active))
        atomic_store(&s->worker_violation, 1);
}

void SDLCALL SDL_DestroyWindow(SDL_Window *window)
{
    (void)window;
    check_main();
    atomic_fetch_add(&s->window_destroyed, 1);
    if (atomic_load(&s->worker_active))
        atomic_store(&s->worker_violation, 1);
}

void SDLCALL SDL_QuitSubSystem(Uint32 flags)
{
    (void)flags;
    check_main();
    atomic_fetch_add(&s->quit_calls, 1);
    if (atomic_load(&s->worker_active))
        atomic_store(&s->worker_violation, 1);
    atomic_store(&s_sdl_active, 0);
}

void SDLCALL SDL_Quit(void)
{
    check_main();
    atomic_fetch_add(&s->quit_calls, 1);
    if (atomic_load(&s->worker_active))
        atomic_store(&s->worker_violation, 1);
    atomic_store(&s_sdl_active, 0);
}

static void *usb_worker(void *unused)
{
    (void)unused;
    atomic_store(&s->worker_active, 1);
    while (!atomic_load(&s->worker_stop)) {
        if (!atomic_load(&s_sdl_active) || !atomic_load(&s->pump))
            atomic_store(&s->worker_violation, 1);
        atomic_fetch_add(&s->worker_ticks, 1);
        sleep_ms(1);
    }
    atomic_store(&s->worker_active, 0);
    return NULL;
}

static void *frame_producer(void *unused)
{
    (void)unused;
    if (is_case("metal-late-worker")) {
        BwMetalFramebuffer frame = {FRAME_AT, 28, 3, 2, BW_METAL_PIXEL_BGRA8, 1, 1};
        atomic_store(&s->worker_active, 1);
        while (!atomic_load(&s->worker_stop)) {
            const BwMetalFramebuffer *pending = bw_test_presenter_running() ? &frame : NULL;
            check(bw_sdl_framebuffer_prepare_metal(pending), "late callback is safe");
            xbox_FramebufferWindowPresent(frame.guest_va, frame.pitch);
            atomic_fetch_add(&s->worker_ticks, 1);
            sleep_ms(1);
        }
        atomic_store(&s->worker_active, 0);
    } else if (is_case("late-worker")) {
        atomic_store(&s->worker_active, 1);
        while (!atomic_load(&s->worker_stop)) {
            xbox_FramebufferWindowPresent(FRAME_AT, PITCH32);
            atomic_fetch_add(&s->worker_ticks, 1);
            sleep_ms(1);
        }
        atomic_store(&s->worker_active, 0);
    } else {
        xbox_FramebufferWindowPresent(is_case("late-present-error") ? MEMORY_SIZE - 4
                                                                     : FRAME_AT,
                                      PITCH32);
    }
    atomic_store(&s->producer_done, 1);
    return NULL;
}

static void *release_copy(void *unused)
{
    (void)unused;
    if (is_case("late-present-error")) {
        while (bw_test_presenter_running())
            sched_yield();
    } else {
        sleep_ms(100);
    }
    atomic_store(&s->gamma_release, 1);
    return NULL;
}

static void push_input_events(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_KEYDOWN;
    SDL_PushEvent(&event);
    memset(&event, 0, sizeof event);
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    SDL_PushEvent(&event);
    memset(&event, 0, sizeof event);
    event.type = SDL_CONTROLLERDEVICEADDED;
    SDL_PushEvent(&event);
}

static void push_key_event(Uint32 type, SDL_Keycode key, Uint8 repeat)
{
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = type;
    event.key.keysym.sym = key;
    event.key.repeat = repeat;
    SDL_PushEvent(&event);
}

static void push_settings_panel_events(void)
{
    push_key_event(SDL_KEYDOWN, SDLK_a, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 1);
    push_key_event(SDL_KEYDOWN, SDLK_b, 0);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_c, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 1);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
}

#if defined(BW_REAL_SETTINGS_PANEL_TEST)
static void push_settings_panel_integration_events(void)
{
    SDL_Event event;
    push_key_event(SDL_KEYDOWN, SDLK_a, 0);
    memset(&event, 0, sizeof event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    SDL_PushEvent(&event);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 1);
    push_key_event(SDL_KEYDOWN, SDLK_b, 0);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_c, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 0);
    push_key_event(SDL_KEYDOWN, SDLK_F1, 1);
    push_key_event(SDL_KEYUP, SDLK_F1, 0);
}
#endif

static uint8_t sample_base(unsigned channel, unsigned x, unsigned y)
{
    static const unsigned start[3] = {11, 37, 73};
    return (uint8_t)(start[channel] + (x % 5) * 7 + (y % 5) * 9);
}

static void fill_metal_frame(const BwMetalFramebuffer *frame)
{
    static const unsigned dx[3] = {1, 5, 9}, dy[3] = {3, 7, 11};
    for (uint32_t y = 0; y < frame->height; y++) {
        uint8_t *row = s_memory + frame->guest_va + (size_t)y * frame->pitch;
        for (uint32_t x = 0; x < frame->width; x++) {
            uint8_t *p = row + (size_t)x * 4;
            for (unsigned c = 0; c < 3; c++)
                p[c] = (uint8_t)(sample_base(c, x / frame->aa_sx, y / frame->aa_sy) +
                                 dx[c] * (x % frame->aa_sx) +
                                 dy[c] * (y % frame->aa_sy));
            p[3] = frame->pixel_format == BW_METAL_PIXEL_BGRX8_ZERO
                 ? 0 : (uint8_t)(1 + (x + 3 * y) % 254);
        }
    }
}

static uint32_t expected_metal_rgb(const BwMetalFramebuffer *frame, unsigned x, unsigned y)
{
    static const unsigned dx[3] = {1, 5, 9}, dy[3] = {3, 7, 11};
    uint32_t result = 0;
    for (unsigned c = 0; c < 3; c++) {
        unsigned extra = (frame->aa_sx == 2 ? dx[c] : 0) +
                         (frame->aa_sy == 2 ? dy[c] : 0);
        unsigned value = sample_base(c, x, y) + (extra + 1) / 2;
        result |= (uint32_t)value << (8 * c);
    }
    return result;
}

static void check_metal_upload(const BwMetalFramebuffer *frame)
{
    unsigned width = frame->width / frame->aa_sx;
    unsigned height = frame->height / frame->aa_sy;
    unsigned count = width * height;
    for (unsigned i = 0; i < count && i < 6; i++) {
        uint32_t pixel = i ? atomic_load(&s->uploaded_pixels[i])
                           : atomic_load(&s->gamma_input_first);
        check((pixel & 0x00ffffffu) == expected_metal_rgb(frame, i % width, i / width),
              "format/AA-aware RGB result before gamma");
    }
    if (count > 1)
        check((atomic_load(&s->uploaded_last) & 0x00ffffffu) ==
              expected_metal_rgb(frame, width - 1, height - 1),
              "last output pixel, including batch boundaries");
    check(atomic_load(&s->uploaded_pixel) == 0xFFABCDEFu,
          "gamma is applied after the Metal resolve");
}

static void *wrong_metal_present_thread(void *arg)
{
    BwMetalFramebuffer *frame = arg;
    xbox_FramebufferWindowPresent(frame->guest_va, frame->pitch);
    return NULL;
}

static int metal_guest(void)
{
    BwMetalFramebuffer frame = {0};
    unsigned sx = is_case("metal-2x1") || is_case("metal-2x2") ||
                  is_case("metal-batched") || is_case("metal-resize") ? 2 : 1;
    unsigned sy = is_case("metal-1x2") || is_case("metal-2x2") ||
                  is_case("metal-batched") || is_case("metal-resize") ? 2 : 1;
    unsigned width = 3, height = is_case("metal-batched") ? 35 : 2;
    int padded = !is_case("metal-1x");
    int mapped_guard = is_case("metal-guarded") || is_case("metal-guard-padding");
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    bw_sdl_framebuffer_expect_metal();
    frame.guest_va = is_case("metal-contig") ? CONTIG_AT : FRAME_AT;
    frame.width = width * sx;
    frame.height = height * sy;
    frame.pitch = frame.width * 4 + (padded ? 16 : 0);
    frame.pixel_format = is_case("metal-xzero-1x") || is_case("metal-2x2") ||
                         is_case("metal-batched")
                       ? BW_METAL_PIXEL_BGRX8_ZERO : BW_METAL_PIXEL_BGRA8;
    frame.aa_sx = sx;
    frame.aa_sy = sy;
    if (mapped_guard) {
        width = height = 2;
        frame.guest_va = (uint32_t)page;
        frame.width = frame.height = 2;
        frame.pitch = (uint32_t)(is_case("metal-guard-padding") ? page * 2 : page);
    }
    atomic_store(&s->expected_width, (int)width);
    atomic_store(&s->expected_height, (int)height);

    if (!is_case("metal-outside") && !is_case("metal-span-wrap"))
        fill_metal_frame(&frame);
    if (mapped_guard) {
        check((uintptr_t)s_memory % page == 0, "test memory is page-aligned");
        check(mprotect(s_memory + page * 2, page, PROT_NONE) == 0,
              "protect a framebuffer sample or padding page");
    }
    if (is_case("metal-late-worker")) {
        check(pthread_create(&s_producer, NULL, frame_producer, NULL) == 0,
              "start concurrent Metal producer");
        s_producer_created = 1;
        check(wait_at_least(&s->worker_ticks, 3), "Metal producer is active");
        return 37;
    }
    if (is_case("metal-outside"))
        frame.guest_va = MEMORY_SIZE - 4;
    if (is_case("metal-span-wrap"))
        frame.guest_va = UINT32_MAX - 8u;
    if (is_case("metal-bad-format"))
        frame.pixel_format = 999;
    if (is_case("metal-bad-aa"))
        frame.aa_sx = 0;
    if (is_case("metal-bad-sy"))
        frame.aa_sy = 3;
    if (is_case("metal-bad-geometry"))
        frame.aa_sx = 2;
    if (is_case("metal-bad-pitch"))
        frame.pitch = frame.width * 4 - 1;
    if (is_case("metal-bad-dimensions"))
        frame.height = 0;
    if (is_case("metal-bad-output")) {
        frame.width = UINT32_MAX / 4u;
        frame.pitch = frame.width * 4u;
    }
    if (!strncmp(s_case, "metal-bad-", 10)) {
        check(!bw_sdl_framebuffer_prepare_metal(is_case("metal-bad-null") ? NULL : &frame),
              "invalid Metal descriptor is rejected");
        return 37;
    }
    if (is_case("metal-replace")) {
        BwMetalFramebuffer stale = frame;
        stale.guest_va += 0x200;
        check(bw_sdl_framebuffer_prepare_metal(&stale), "prepare an unconsumed frame");
    }
    if (!is_case("metal-missing") && !is_case("metal-missing-zero")) {
        check(bw_sdl_framebuffer_prepare_metal(&frame), "prepare Metal frame");
        if (is_case("metal-descriptor-copy")) {
            BwMetalFramebuffer saved = frame;
            memset(&frame, 0, sizeof frame);
            xbox_FramebufferWindowPresent(saved.guest_va, saved.pitch);
            check(wait_at_least(&s->updates, 1), "copied descriptor reaches renderer");
            check_metal_upload(&saved);
            return 37;
        }
    }
    if (is_case("metal-wrong-thread")) {
        pthread_t thread;
        check(pthread_create(&thread, NULL, wrong_metal_present_thread, &frame) == 0,
              "start wrong-thread present");
        pthread_join(thread, NULL);
        return 37;
    }
    xbox_FramebufferWindowPresent(is_case("metal-mismatch-va") ? frame.guest_va + 4
                                 : is_case("metal-missing-zero") ? 0 : frame.guest_va,
                                 is_case("metal-mismatch-pitch") ? frame.pitch + 4 : frame.pitch);
    if (is_case("metal-direct")) {
        SDL_Event event;
        check(atomic_load(&s->metal_attach) == 1, "direct presenter attached once");
        check(!atomic_load(&s->updates) && !atomic_load(&s->gamma_calls),
              "direct presentation skips the CPU upload and gamma path");
        memset(&event, 0, sizeof event);
        event.type = SDL_WINDOWEVENT;
        event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
        check(SDL_PushEvent(&event) == 1, "queue direct-presenter resize");
        check(wait_at_least(&s->metal_resize, 1), "resize reaches direct presenter");
        return 37;
    }
    if (is_case("metal-guarded") || is_case("metal-outside") ||
        is_case("metal-span-wrap") || is_case("metal-missing") ||
        is_case("metal-missing-zero") || is_case("metal-mismatch-va") ||
        is_case("metal-mismatch-pitch"))
        return 37;
    check(wait_at_least(&s->updates, 1), "Metal frame reaches renderer");
    check_metal_upload(&frame);
    check(s_memory[frame.guest_va] == sample_base(0, 0, 0),
          "Metal resolve and gamma never modify guest memory");
    if (is_case("metal-reuse")) {
        xbox_FramebufferWindowPresent(frame.guest_va, frame.pitch);
        return 37;
    }
    if (is_case("metal-resize")) {
        width = 2;
        height = 3;
        frame.width = width * sx;
        frame.height = height * sy;
        frame.pitch = frame.width * 4 + 16;
        fill_metal_frame(&frame);
        atomic_store(&s->expected_width, (int)width);
        atomic_store(&s->expected_height, (int)height);
        check(bw_sdl_framebuffer_prepare_metal(&frame), "prepare resized frame");
        xbox_FramebufferWindowPresent(frame.guest_va, frame.pitch);
        check(wait_at_least(&s->updates, 2), "resized frame reaches renderer");
        check_metal_upload(&frame);
        check(atomic_load(&s->texture_creations) == 2 &&
              atomic_load(&s->texture_destroyed) == 1, "texture is recreated on resize");
    }
    return 37;
}

static int guest_main(void)
{
    uint32_t pixel = 0xFF112233u;
    uint32_t address = is_case("frame-contig") ? CONTIG_AT : FRAME_AT;
    static const uint16_t title[] = {'T', 'e', 's', 't', 0};
    atomic_fetch_add(&s->guest_calls, 1);
    if (is_case("headless") || is_case("headless-window-off") || is_case("metal-headless")) {
        check(pthread_equal(pthread_self(), s_main), "headless guest uses the calling thread");
        if (is_case("metal-headless")) {
            bw_sdl_framebuffer_expect_metal();
            check(bw_sdl_framebuffer_prepare_metal(NULL), "headless callback is a no-op");
            xbox_FramebufferWindowPresent(0, 0);
        }
        return 37;
    }
    check(!pthread_equal(pthread_self(), s_main), "visible guest runs on a worker");
    check(atomic_load(&s->input_ready) && atomic_load(&s->pump),
          "guest starts only after main-thread input initialization");

    if (metal_case())
        return metal_guest();
    if (is_case("lost-completion"))
        return 37;
    if (is_case("settings-panel")) {
        push_settings_panel_events();
        return 37;
    }
#if defined(BW_REAL_SETTINGS_PANEL_TEST)
    if (is_case("settings-panel-integration")) {
        push_settings_panel_integration_events();
        return 37;
    }
#endif
    if (is_case("close")) {
        SDL_Event event;
        check(pthread_create(&s_worker, NULL, usb_worker, NULL) == 0, "start USB-like worker");
        check(wait_at_least(&s->worker_active, 1), "USB-like worker started");
        memset(&event, 0, sizeof event);
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
        for (;;)
            sleep_ms(10);
    }
    if (is_case("invalid-format")) {
        xbox_FramebufferWindowPresent(FRAME_AT, WIDTH * 3);
        return 37;
    }
    if (is_case("invalid-address")) {
        xbox_FramebufferWindowPresent(MEMORY_SIZE - 4, PITCH32);
        return 37;
    }
    memcpy(s_memory + address, &pixel, sizeof pixel);
    if (is_case("frame-16")) {
        uint16_t p16 = 0x1234;
        memcpy(s_memory + address, &p16, sizeof p16);
    }
    xbox_FramebufferWindowSetTitle(title, 4);
    xbox_FramebufferWindowSet(address, is_case("frame-16") ? PITCH16 : PITCH32);

    if (is_case("blocked-copy") || is_case("late-worker") || is_case("late-present-error")) {
        check(pthread_create(&s_producer, NULL, frame_producer, NULL) == 0,
              "start concurrent framebuffer producer");
        s_producer_created = 1;
        if (is_case("blocked-copy") || is_case("late-present-error")) {
            check(wait_at_least(&s->gamma_entered, 1), "frame processing is in flight");
            check(pthread_create(&s_releaser, NULL, release_copy, NULL) == 0,
                  "start frame-processing releaser");
            s_releaser_created = 1;
        } else {
            check(wait_at_least(&s->worker_ticks, 3), "late producer is active");
        }
        return 37;
    }
    if (is_case("frame-32")) {
        check(pthread_create(&s_worker, NULL, usb_worker, NULL) == 0, "start USB-like worker");
        s_worker_created = 1;
        check(wait_at_least(&s->worker_active, 1), "USB-like worker started");
        push_input_events();
    }
    xbox_FramebufferWindowPresent(address, is_case("frame-16") ? PITCH16 : PITCH32);
    if (is_case("texture-fail") || is_case("update-fail"))
        for (;;)
            sleep_ms(10);
    check(wait_at_least(&s->updates, 1), "frame reached the main-thread renderer");
    return 37;
}

static void record_atexit(void) { atomic_fetch_add(&s->atexit_called, 1); }

static int runtime_case(void)
{
    return is_case("close") || is_case("texture-fail") || is_case("update-fail") ||
           is_case("render-fail") || is_case("invalid-format") || is_case("invalid-address") ||
           is_case("late-present-error") || !strncmp(s_case, "metal-bad-", 10) ||
           is_case("metal-guarded") || is_case("metal-outside") ||
           is_case("metal-span-wrap") || is_case("metal-missing") ||
           is_case("metal-missing-zero") || is_case("metal-mismatch-va") ||
           is_case("metal-mismatch-pitch") || is_case("metal-wrong-thread") ||
           is_case("metal-reuse");
}

static void check_no_teardown(void)
{
    check(!atomic_load(&s->quit_calls), "SDL/controller subsystem remains alive");
    check(!atomic_load(&s->window_destroyed), "window remains alive while workers can use SDL");
    check(!atomic_load(&s->renderer_destroyed), "renderer remains alive while workers can use SDL");
    check(atomic_load(&s->texture_destroyed) == (is_case("metal-resize") ? 1 : 0),
          "published texture remains alive");
    check(!atomic_load(&s->worker_violation), "worker never observed SDL teardown");
}

static void *wrong_thread(void *unused)
{
    (void)unused;
    return (void *)(intptr_t)xbox_HostWindowMain(guest_main);
}

int main(int argc, char **argv)
{
    int rc;
    if (argc != 2)
        return 2;
    s_case = argv[1];
#if defined(BW_REAL_SETTINGS_PANEL_TEST)
    if (is_case("settings-panel-integration") &&
        !panel_window_server_available()) {
        fprintf(stderr,
                "SKIP settings-panel-integration: no active macOS WindowServer session\n");
        return 77;
    }
#endif
    s_main = pthread_self();
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (s == MAP_FAILED)
        return 2;
    memset(s, 0, sizeof *s);
    unsetenv("RECOMP_HEADLESS");
    setenv("RECOMP_FB_WINDOW", "1", 1);
    if (is_case("headless") || is_case("metal-headless"))
        setenv("RECOMP_HEADLESS", "1", 1);
    if (is_case("headless-window-off"))
        setenv("RECOMP_FB_WINDOW", "0", 1);
    if (is_case("metal-direct"))
        bw_sdl_framebuffer_expect_metal();
#if defined(BW_REAL_SETTINGS_PANEL_TEST)
    if (is_case("settings-panel-integration") &&
        !panel_integration_setup()) {
        fprintf(stderr, "FAIL settings-panel-integration: setup failed\n");
        panel_integration_cleanup();
        munmap(s, sizeof *s);
        return 2;
    }
#endif

    if (runtime_case()) {
        pid_t child = fork();
        int status = 0;
        if (child < 0)
            return 2;
        if (!child) {
            s_main = pthread_self();
            atexit(record_atexit);
            (void)xbox_HostWindowMain(guest_main);
            _exit(90);
        }
        if (waitpid(child, &status, 0) != child)
            return 2;
        check(WIFEXITED(status), "runtime failure/close exits the child normally");
        check(WIFEXITED(status) && WEXITSTATUS(status) == (is_case("close") ? 0 : 1),
              "runtime failure/close has the correct exit status");
        check(!atomic_load(&s->atexit_called), "process exit does not run teardown handlers");
        check_no_teardown();
        if (is_case("invalid-format") || is_case("invalid-address") ||
            is_case("late-present-error") || (metal_case() && !is_case("metal-reuse")))
            check(!atomic_load(&s->gamma_calls) && !atomic_load(&s->updates),
                  "invalid frame never reaches gamma or SDL");
        if (is_case("late-present-error"))
            check(atomic_load(&s->gamma_release),
                  "late snapshot error is raised only after shutdown begins");
    } else if (is_case("wrong-thread")) {
        pthread_t thread;
        void *result = NULL;
        check(pthread_create(&thread, NULL, wrong_thread, NULL) == 0, "start off-main caller");
        pthread_join(thread, &result);
        check((intptr_t)result != 0 && !atomic_load(&s->init_calls),
              "off-main host call fails before SDL initialization");
        check(!atomic_load(&s->guest_calls), "off-main host never starts guest");
    } else if (is_case("no-callback")) {
        check(xbox_HostWindowMain(NULL) != 0, "missing guest callback fails");
        check(!atomic_load(&s->init_calls), "missing callback does not initialize SDL");
    } else {
        int startup_failure = strstr(s_case, "fail") || strstr(s_case, "driver") ||
                              is_case("hidden-window");
        rc = xbox_HostWindowMain(guest_main);
        if (startup_failure) {
            check(rc != 0, "visible-window startup failures are nonzero");
            check(!atomic_load(&s->guest_calls), "startup failure never runs guest");
            check(!atomic_load(&s->input_ready), "startup failure never opens input devices");
            check(atomic_load(&s->quit_calls) == !is_case("init-fail"),
                  "only successfully initialized SDL subsystems are released");
        } else if (is_case("headless") || is_case("headless-window-off") ||
                   is_case("metal-headless")) {
            check(rc == 37 && atomic_load(&s->guest_calls) == 1, "explicit headless callback");
            check(!atomic_load(&s->init_calls), "explicit headless mode does not initialize video");
        } else {
            check(rc == 37 && atomic_load(&s->guest_calls) == 1, "guest completion status");
            check(atomic_load(&s->input_ready) && atomic_load(&s->pump),
                  "main-thread controller pump remains active");
            check_no_teardown();
            if (is_case("frame-32") || is_case("frame-contig")) {
                uint32_t address = is_case("frame-contig") ? CONTIG_AT : FRAME_AT;
                uint32_t guest_pixel = 0;
                memcpy(&guest_pixel, s_memory + address, sizeof guest_pixel);
                check(atomic_load(&s->uploaded_pixel) == 0xFFABCDEFu,
                      "renderer receives the gamma-adjusted copy");
                check(guest_pixel == 0xFF112233u, "gamma never modifies guest memory");
                check(atomic_load(&s->gamma_calls) == 1, "gamma runs once per 32-bit snapshot");
            }
            if (is_case("frame-16")) {
                check(atomic_load(&s->uploaded_pixel) == 0x1234u, "16-bit frame is preserved");
                check(!atomic_load(&s->gamma_calls), "32-bit gamma is not applied to RGB565");
            }
            if (is_case("frame-32")) {
                check(atomic_load(&s->key_events) && atomic_load(&s->focus_events) &&
                      atomic_load(&s->controller_events), "keyboard, focus and hotplug reach input");
                check(atomic_load(&s->worker_active), "controller worker survives presenter return");
                check(xbox_HostWindowMain(guest_main) != 0, "presenter cannot restart with live workers");
            }
            if (is_case("settings-panel")) {
                check(atomic_load(&s->panel_toggles) == 5,
                      "F1 repeat and key-up never toggle the panel");
                check(atomic_load(&s->panel_opens) == 3 &&
                      atomic_load(&s->panel_closes) == 3 &&
                      !atomic_load(&s->panel_open),
                      "panel opens, closes, reopens, and is closed at shutdown");
                check(atomic_load(&s->panel_close_calls) == 1,
                      "presenter closes the panel when the guest returns");
                check(atomic_load(&s->focus_events) == 3,
                      "each open releases guest focus state");
                check(!atomic_load(&s->f1_events),
                      "F1 down, repeat, and matching key-up are consumed");
                check(atomic_load(&s->settings_regular_events) == 2,
                      "guest keyboard input is blocked only while the panel is active");
            }
#if defined(BW_REAL_SETTINGS_PANEL_TEST)
            if (is_case("settings-panel-integration")) {
                bw_macos_settings_panel_test_state_t state;
                check(atomic_load(&s->panel_notifications) == 3 &&
                      atomic_load(&s->panel_open_notifications) == 2 &&
                      atomic_load(&s->panel_close_notifications) == 1,
                      "only non-repeat F1 key-downs toggle the real panel");
                check(bw_macos_settings_panel_test_state(&state) == 0 &&
                      state.controller_created && !state.panel_created &&
                      !state.visible && state.row_count == 0,
                      "presenter shutdown closes and releases the real panel");
                check(!xbox_FramebufferKeyDown(XBOX_VK_F1) &&
                      !xbox_FramebufferKeyDown('A') &&
                      !xbox_FramebufferKeyDown('B') &&
                      !xbox_FramebufferKeyDown('C'),
                      "F1 down/repeat/up are consumed and focus releases guest keys");
                check(atomic_load(&s->relative_enables) == 1 &&
                      atomic_load(&s->relative_disables) == 1 &&
                      !bw_macos_mouse_captured(),
                      "relative mouse mode is entered once and released by F1");
                check(panel_file_contains(s_panel_settings, "mode=fullscreen") &&
                      panel_file_contains(s_panel_settings, "texture_filtering=8") &&
                      panel_file_contains(s_panel_settings, "draw_distance=4") &&
                      access(s_panel_temp, F_OK) != 0,
                      "isolated settings contain every successful panel save");
            }
#endif
            if (is_case("blocked-copy"))
                check(atomic_load(&s->gamma_release),
                      "host waits for the in-flight snapshot before returning");
            if (is_case("late-worker") || is_case("metal-late-worker")) {
                int ticks = atomic_load(&s->worker_ticks);
                check(atomic_load(&s->worker_active), "producer remains alive after presenter return");
                atomic_store(&s->memory_dead, 1);
                check(wait_at_least(&s->worker_ticks, ticks + 10), "late producer continued");
                check(!atomic_load(&s->late_memory_access),
                      "late present calls no longer touch guest memory");
            }
            atomic_store(&s->worker_stop, 1);
            if (s_worker_created)
                pthread_join(s_worker, NULL);
            if (s_producer_created) {
                pthread_join(s_producer, NULL);
                check(atomic_load(&s->producer_done), "frame producer exited cleanly");
            }
            if (s_releaser_created)
                pthread_join(s_releaser, NULL);
        }
    }
    rc = atomic_load(&s->errors) ? 1 : 0;
#if defined(BW_REAL_SETTINGS_PANEL_TEST)
    if (is_case("settings-panel-integration"))
        panel_integration_cleanup();
#endif
    munmap(s, sizeof *s);
    return rc;
}
